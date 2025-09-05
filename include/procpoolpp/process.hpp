#pragma once

#include <iostream>
#include <vector>
#include <cstdint>
#include <functional>
#include <mutex>
#include <chrono>
#include <thread>
#include <unordered_map>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>

// 定义处理函数的函数指针类型
using ConvertFunction = std::function<int(const unsigned char* data, size_t size, unsigned char* outputBuffer, size_t* outputSize)>;
using HandlerFunction = std::function<void(const std::vector<unsigned char>&, std::vector<unsigned char>&)>;

namespace procpoolpp {

// 子进程句柄类，负责管理进程信息
struct SubprocessHandle {
    pid_t pid;
    int read_fd;
    int write_fd;

    SubprocessHandle() : pid(-1), read_fd(-1), write_fd(-1) {}

    ~SubprocessHandle() {
        if (pid > 0) {
            int status = 0;
            // 等待子进程退出，避免僵尸进程
            waitpid(pid, &status, 0);
        }
        if (read_fd > 0) {
            close(read_fd);
        }
        if (write_fd > 0) {
            close(write_fd);
        }
    }

    void set_pipes(int r_fd, int w_fd) {
        read_fd = r_fd;
        write_fd = w_fd;
    }

    bool write_data(const void* data, size_t size) {
        const char* buffer = static_cast<const char*>(data);
        size_t bytes_written = 0;

        while (bytes_written < size) {
            ssize_t result = write(write_fd, buffer + bytes_written, size - bytes_written);
            if (result < 0) {
                return false;
            }
            bytes_written += result;
        }
        return true;
    }

    bool read_data(void* data, size_t size) {
        char* buffer = static_cast<char*>(data);
        size_t bytes_read = 0;

        while (bytes_read < size) {
            ssize_t result = read(read_fd, buffer + bytes_read, size - bytes_read);
            if (result <= 0) {
                return false;
            }
            bytes_read += result;
        }
        return true;
    }
};

// 子进程控制器类，负责启动、管理子进程，并与之通信
class SubprocessController {
public:
    enum SubprocessCommand {
        TERMINATE,
        RUN_TASK
    };

    explicit SubprocessController(ConvertFunction handler, int64_t max_mem_in_bytes = -1) :
            _pid(-1), _task_handler(handler), _max_mem_in_bytes(max_mem_in_bytes) {}

    ~SubprocessController() {
        terminate();
    }

    // 启动子进程
    bool start() {
        std::lock_guard<std::mutex> lock(_state_mutex);

        if (_pid > 0) {
            return true; // 子进程已启动
        }
        int pipe_to_child[2];     // 父进程写 -> 子进程读
        int pipe_from_child[2];   // 子进程写 -> 父进程读

        if (pipe(pipe_to_child) == -1 || pipe(pipe_from_child) == -1) {
            std::cerr << "Error: Failed to create pipes." << std::endl;
            return false;
        }

        _pid = fork();
        if (_pid == -1) {
            std::cerr << "Error: Failed to fork process." << std::endl;
            return false;
        }

        if (_pid == 0) {
            // 子进程代码
            close(pipe_to_child[1]);    // 关闭父进程写入端
            close(pipe_from_child[0]);  // 关闭父进程读取端

            int child_read_fd = pipe_to_child[0];
            int child_write_fd = pipe_from_child[1];

            // 设置内存限制
            if (_max_mem_in_bytes > 0) {
                struct rlimit64 limit;
                limit.rlim_cur = _max_mem_in_bytes;  // 软限制
                limit.rlim_max = _max_mem_in_bytes;  // 硬限制
                if (setrlimit64(RLIMIT_RSS, &limit) != 0) {
                    std::cerr << "Error: Failed to set memory limit." << std::endl;
                    exit(1);
                }
                std::cout << "[subprocess]"
                    << " process_id: " << getpid()
                    << " memory limit: " << _max_mem_in_bytes
                    << std::endl;
            } else {
                std::cout << "[subprocess]"
                    << " process_id: " << getpid()
                    << " no memory limit"
                    << std::endl;
            }

            int result = _process_stream(child_read_fd, child_write_fd);
            std::cerr << "[subprocess]"
                << " process_id: " << getpid()
                << " exiting with code: " << result
                << std::endl;

            close(child_read_fd);
            close(child_write_fd);
            exit(result);
        } else {
            // 父进程代码
            close(pipe_to_child[0]);    // 关闭子进程读取端
            close(pipe_from_child[1]);  // 关闭子进程写入端

            _handle.set_pipes(pipe_from_child[0], pipe_to_child[1]);
            _handle.pid = _pid;
            return true;
        }
    }

    // 发送命令到子进程并处理响应
    int send_command(const std::vector<unsigned char>& input_data,
                            std::vector<unsigned char>& output_data, int& response_code) {
        std::lock_guard<std::mutex> lock(_state_mutex);
        if (_pid <= 0) {
            return 1; // 忙碌或子进程未启动
        }

        try {
            // 发送命令
            int command = static_cast<int>(SubprocessCommand::RUN_TASK);
            if (!_handle.write_data(&command, sizeof(command))) {
                return 2;
            }

            // 发送数据长度和数据内容
            size_t length = input_data.size();
            if (!_handle.write_data(&length, sizeof(length))) {
                return 3;
            }
            if (!_handle.write_data(input_data.data(), length)) {
                return 4;
            }

            // 读取响应码
            int32_t tmp_response_code;
            if (!_handle.read_data(&tmp_response_code, sizeof(tmp_response_code))) {
                return 6;
            }
            response_code = tmp_response_code;

            if (response_code != 0) {
                return 0; // 返回 0 表示命令成功执行，但命令执行失败
            }

            // 读取响应数据长度和数据内容
            size_t response_length = 0;
            if (!_handle.read_data(&response_length, sizeof(response_length))) {
                return 7;
            }

            output_data.resize(response_length);
            if (!_handle.read_data(output_data.data(), response_length)) {
                return 8;
            }
        } catch (...) {
            std::cerr << "Error: An exception occurred during subprocess communication." << std::endl;
            return 100; // 处理错误
        }

        return 0; // 返回 0 表示命令成功执行，并且管道操作成功
    }

    // 终止子进程
    void terminate() {
        // 发送中止请求
        std::lock_guard<std::mutex> lock(_state_mutex);
        if (_pid <= 0) {
            return;
        }

        int command = static_cast<int>(SubprocessCommand::TERMINATE);
        (void)_handle.write_data(&command, sizeof(command));

        // 等待 100ms
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        int status = 0;
        pid_t result = waitpid(_pid, &status, WNOHANG);

        // 如果子进程已经退出，直接返回
        if (result > 0) {
            _pid = -1;
            return;
        }

        // 发送 SIGTERM 信号 (kill -15)，尝试正常终止子进程
        kill(_pid, SIGTERM);

        // 等待最多 500ms，每 50ms 检查一次子进程状态
        for (int i = 0; i < 10; ++i) {
            result = waitpid(_pid, &status, WNOHANG);
            if (result > 0) {
                _pid = -1;
                return;  // 子进程已经退出
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 每 50ms 检查一次
        }

        // 如果子进程仍然未退出，发送 SIGKILL 信号 (kill -9)，强制终止
        kill(_pid, SIGKILL);

        // 发送 SIGKILL 后无需等待，直接清理
        _pid = -1;
    }

private:
    SubprocessHandle _handle;
    pid_t _pid;
    ConvertFunction _task_handler;
    std::mutex _state_mutex;
    int64_t _max_mem_in_bytes;

    // 子进程处理输入和输出流
    int _process_stream(int read_fd, int write_fd) {
        // 预分配一个 4MB 的输入和输出缓冲区
        const size_t buffer_size = 4 * 1024 * 1024; // 4MB
        std::vector<unsigned char> input_buffer(buffer_size);
        std::vector<unsigned char> output_buffer(buffer_size);

        // 创建 SubprocessHandle 用于读写操作
        SubprocessHandle handle;
        handle.set_pipes(read_fd, write_fd);

        while (true) {
            int command_type = 0;
            if (!handle.read_data(&command_type, sizeof(command_type))) {
                return 12;
            }

            if (command_type == static_cast<int>(SubprocessCommand::TERMINATE)) {
                break;
            } else if (command_type == static_cast<int>(SubprocessCommand::RUN_TASK)) {
                size_t length = 0;
                if (!handle.read_data(&length, sizeof(length))) {
                    return 13;
                }

                // 如果输入数据大于缓冲区大小，分多次读取，防止溢出
                size_t remaining = length;
                size_t offset = 0;
                while (remaining > 0) {
                    size_t to_read = std::min(remaining, buffer_size);
                    if (!handle.read_data(input_buffer.data() + offset, to_read)) {
                        return 14;
                    }
                    remaining -= to_read;
                    offset += to_read;
                }

                // 任务处理
                size_t output_size = buffer_size; // 初始设置为 4MB
                int result = _task_handler(input_buffer.data(), length, output_buffer.data(), &output_size);

                int32_t response_code = result;
                if (!handle.write_data(&response_code, sizeof(response_code))) {
                    return 15;
                }

                if (response_code == 0) {
                    // 如果任务成功，写回数据长度和数据内容
                    if (!handle.write_data(&output_size, sizeof(output_size))) {
                        return 16;
                    }

                    if (!handle.write_data(output_buffer.data(), output_size)) {
                        return 17;
                    }
                }
            }
        }
        return 0;
    }
};

}