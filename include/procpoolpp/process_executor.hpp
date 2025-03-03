#pragma once

#include <babylon/executor.h>
#include <babylon/future.h>

#include <procpoolpp/process.hpp>
#include <procpoolpp/process_pool.hpp>

namespace procpoolpp {

class ProcessExecutor {
private:
    ProcessPool _p_pool;
    ::babylon::ThreadPoolExecutor _t_executor;

public:
    ProcessExecutor(
                    int num_processes, 
                    int queue_size,
                    std::function<ConvertFunction()> convert_factory,
                    int64_t memory_limit_in_bytes = -1
                ) :
                            _p_pool(convert_factory, num_processes, memory_limit_in_bytes) {
        
        
        _t_executor.set_worker_number(num_processes);
        _t_executor.set_local_capacity(queue_size);
        _t_executor.set_global_capacity(std::min(1, num_processes * queue_size / 2));
        _t_executor.set_enable_work_stealing(false);
        if (_t_executor.start()) {
            std::cerr << "start failed" << std::endl;
        };
    }

    // 0: 成功
    // 1-100: 转换函数的失败
    // 1001: 未找到可用的进程
    // 1002: 子进程执行失败
    ::babylon::Future<int> async_submit(const unsigned char* tileData,
                        size_t tileSize,
                        unsigned char* outputBuffer,
                        size_t* outputSize,
                        std::chrono::steady_clock::time_point deadline) {
        
        auto result = _t_executor.execute(
            [this, outputBuffer, outputSize, deadline](const unsigned char* tileData, size_t tileSize) -> int {
                // 1. 获取空闲进程
                auto processGuard = _p_pool.getProcessWithGuard();
                auto now = std::chrono::steady_clock::now();

                // 直到获取到空闲进程或者超时
                while (!processGuard && now < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 每1毫秒检测一次
                    now = std::chrono::steady_clock::now(); // 更新时间
                    processGuard = _p_pool.getProcessWithGuard();
                }

                if (!processGuard) {
                    std::cerr << "Error: No available process found before deadline." << std::endl;
                    return 1001; // 未找到可用的进程
                }
                
                // 2. 发送转换任务
                std::vector<unsigned char> result;
                int response_code;
                int process_code = processGuard->get_process()->send_command(
                    std::vector<unsigned char>(tileData, tileData + tileSize),
                    result,
                    response_code
                );

                if (process_code != 0) {
                    std::cerr << "convert failed, process_code: " << process_code << std::endl;
                    processGuard->set_fail();
                    return 1002;
                }
                *outputSize = result.size();
                std::copy(result.begin(), result.end(), outputBuffer);
                return response_code;
            },
            tileData, tileSize
        );

        return result;
    }

    // 同步版 submit
    int submit(const unsigned char* tileData,
                        size_t tileSize,
                        unsigned char* outputBuffer,
                        size_t* outputSize,
                        std::chrono::steady_clock::time_point deadline) {
        auto future = async_submit(tileData, tileSize, outputBuffer, outputSize, deadline);
        if (!future.valid()) {
            return 1003; // 无法提交任务（可能是线程池队列已满）
        }
        return future.get();
    }
};

}