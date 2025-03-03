#pragma once

#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <functional>
#include <map>

#include <procpoolpp/process.hpp>

namespace procpoolpp {

class ProcessPool {
private:
    // 进程守护类，用于自动回收进程资源
    // 线程不安全，且调用方需要保证生命周期 < 对应的 ProcessPool
    class ProcessGuard {
    public:
        ProcessGuard(std::shared_ptr<SubprocessController> process, ProcessPool* pool)
            : _process(process), _pool(pool), _valid(true) {}

        // 标记进程失效
        void set_fail() {
            _valid = false;
        }

        SubprocessController* get_process() {
            return _process.get();
        }

        // 析构时自动回收进程
        ~ProcessGuard() {
            if (!_pool) {
                return;
            }

            if (_valid) {
                // 如果进程有效，放回池中
                _pool->_return_process(_process);
            } else {
                // 如果进程失效，放到待销毁池
                _pool->_set_fail(_process);
            }
        }

    private:
        std::shared_ptr<SubprocessController> _process;
        ProcessPool* _pool;
        bool _valid; // 标记进程是否有效
    };

    std::mutex _mutex;
    std::vector<std::shared_ptr<SubprocessController>> _subprocesses;
    std::vector<std::shared_ptr<SubprocessController>> _wait_for_shutdown;
    std::function<ConvertFunction()> _convert_factory;
    std::atomic<bool> _shutdown_flag;
    std::thread _background_thread;
    int32_t _memory_limit_in_bytes;

public:
    ProcessPool(
            std::function<ConvertFunction()> convert_factory, 
            int num_processes,
            int32_t memory_limit_in_bytes = -1
        )
        : _convert_factory(convert_factory), _shutdown_flag(false), _memory_limit_in_bytes(memory_limit_in_bytes) {
        // 初始化进程池
        for (int i = 0; i < num_processes; ++i) {
            _create_subprocess();
        }

        // 启动后台线程，异步管理进程状态
        _background_thread = std::thread([this]() {
            _t_manage_subprocess();
        });
    }

    ~ProcessPool() {
        _shutdown_flag = true;
        // 等待后台线程结束
        if (_background_thread.joinable()) {
            _background_thread.join();
        }
    }

    // 获取进程，返回一个进程守护对象
    std::shared_ptr<ProcessGuard> getProcessWithGuard() {
        if (_shutdown_flag) {
            return nullptr;
        }

        std::lock_guard lock(_mutex);
        if (_subprocesses.empty()) {
            return nullptr;
        }

        auto process = _subprocesses.back();
        _subprocesses.pop_back();

        // 返回一个 ProcessGuard，确保进程被正确管理
        return std::make_shared<ProcessGuard>(process, this);
    }

private:

    // 创建新的子进程并添加到进程池中
    void _create_subprocess() {
        auto process = std::make_shared<SubprocessController>(_convert_factory(), _memory_limit_in_bytes);
        bool start_succ = process->start();
        if (start_succ) {
            std::lock_guard lock(_mutex);
            _subprocesses.push_back(process);
        } else {
            std::cerr << "Failed to create subprocess." << std::endl;
            std::lock_guard lock(_mutex);
            _wait_for_shutdown.push_back(process);
        }
    }

    // 后台线程，定期检查并管理进程状态
    void _t_manage_subprocess() {
        while (!_shutdown_flag) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // 拷贝待销毁和空闲进程，释放锁后进行销毁和创建操作
            std::vector<std::shared_ptr<SubprocessController>> processes_to_shutdown;
            {
                std::lock_guard lock(_mutex);
                _wait_for_shutdown.swap(processes_to_shutdown);
                // 拷贝待销毁的进程列表
            }
            if (processes_to_shutdown.size()) {
                std::cerr << "[process pool manager thread]"
                    << ", thread_id: " << std::this_thread::get_id()
                    << ", wait_for_shutdown_size: " << processes_to_shutdown.size()
                    << std::endl;
            }

            for (auto& process : processes_to_shutdown) {
                process->terminate();
                this->_create_subprocess();
            }
        }

        // 在关闭时销毁所有进程
        for (auto& process : _subprocesses) {
            process->terminate();
        }
        for (auto& process : _wait_for_shutdown) {
            process->terminate();
        }
        _subprocesses.clear();
    }

    // 将进程标记为失败，待销毁
    void _set_fail(std::shared_ptr<SubprocessController> process) {
        std::lock_guard lock(_mutex);
        _wait_for_shutdown.push_back(process);
    }

    // 将进程归还到池中
    void _return_process(std::shared_ptr<SubprocessController> process) {
        std::lock_guard lock(_mutex);
        _subprocesses.push_back(process);
    }
};

}