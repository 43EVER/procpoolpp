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

#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/smart_ptr.hpp>

// 定义处理函数的函数指针类型
using ConvertFunction = std::function<int(const unsigned char* data, size_t size, unsigned char* outputBuffer, size_t* outputSize)>;
using HandlerFunction = std::function<void(const std::vector<unsigned char>&, std::vector<unsigned char>&)>;

namespace procpoolpp {

// 子进程句柄类，负责管理进程信息
struct SubprocessHandle {
    pid_t pid;
    boost::shared_ptr<boost::iostreams::stream<boost::iostreams::file_descriptor_source>> input_stream;
    boost::shared_ptr<boost::iostreams::stream<boost::iostreams::file_descriptor_sink>> output_stream;

    SubprocessHandle() : pid(-1) {}

    ~SubprocessHandle() {
        if (pid > 0) {
            int status = 0;
            // 等待子进程退出，避免僵尸进程
            waitpid(pid, &status, 0);
        }
    }

    void set_pipes(int read_fd, int write_fd) {
        input_stream = boost::make_shared<boost::iostreams::stream<boost::iostreams::file_descriptor_source>>(
            boost::iostreams::file_descriptor_source(read_fd, boost::iostreams::close_handle));
        output_stream = boost::make_shared<boost::iostreams::stream<boost::iostreams::file_descriptor_sink>>(
            boost::iostreams::file_descriptor_sink(write_fd, boost::iostreams::close_handle));
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

            boost::iostreams::file_descriptor_source input_source(pipe_to_child[0], boost::iostreams::close_handle);
            boost::iostreams::file_descriptor_sink output_sink(pipe_from_child[1], boost::iostreams::close_handle);
            boost::iostreams::stream<boost::iostreams::file_descriptor_source> in_stream(input_source);
            boost::iostreams::stream<boost::iostreams::file_descriptor_sink> out_stream(output_sink);

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
                    << " process_id: " << std::this_thread::get_id()
                    << " memory limit: " << _max_mem_in_bytes
                    << std::endl;
            } else {
                std::cout << "[subprocess]"
                    << " process_id: " << std::this_thread::get_id()
                    << " no memory limit"
                    << std::endl;
            }

            int result = _process_stream(in_stream, out_stream);
            std::cerr << "[subprocess]"
                << " process_id: " << std::this_thread::get_id()
                << " exiting with code: " << result
                << std::endl;
            exit(result);
        } else {
            // 父进程代码
            close(pipe_to_child[0]);    // 关闭子进程读取端
            close(pipe_from_child[1]);  // 关闭子进程写入端

            _handle.set_pipes(pipe_from_child[0], pipe_to_child[1]);
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
            int errorcode = 0;

            // 发送命令
            int command = static_cast<int>(SubprocessCommand::RUN_TASK);
            _handle.output_stream->write(reinterpret_cast<const char*>(&command), sizeof(command));
            errorcode = _check_stream_failure(_handle.output_stream.get(), 2);
            if (errorcode) return errorcode;
            
            // 发送数据长度和数据内容
            size_t length = input_data.size();
            _handle.output_stream->write(reinterpret_cast<const char*>(&length), sizeof(length));
            errorcode = _check_stream_failure(_handle.output_stream.get(), 3);
            if (errorcode) return errorcode;

            _handle.output_stream->write(reinterpret_cast<const char*>(input_data.data()), length);
            errorcode = _check_stream_failure(_handle.output_stream.get(), 4);
            if (errorcode) return errorcode;
            
            _handle.output_stream->flush();
            errorcode = _check_stream_failure(_handle.output_stream.get(), 5);
            if (errorcode) return errorcode;

            // 读取响应码
            int32_t tmp_response_code;
            _handle.input_stream->read(reinterpret_cast<char*>(&tmp_response_code), sizeof(tmp_response_code));
            errorcode = _check_stream_failure(_handle.input_stream.get(), 6);
            if (errorcode) return errorcode;
            response_code = tmp_response_code;

            if (response_code != 0) {
                return 0; // 返回 0 表示管道操作成功，但命令执行失败
            }

            // 读取响应数据长度和数据内容
            size_t response_length = 0;
            _handle.input_stream->read(reinterpret_cast<char*>(&response_length), sizeof(response_length));
            errorcode = _check_stream_failure(_handle.input_stream.get(), 7);
            if (errorcode) return errorcode;

            output_data.resize(response_length);
            _handle.input_stream->read(reinterpret_cast<char*>(output_data.data()), response_length);
            errorcode = _check_stream_failure(_handle.input_stream.get(), 8);
            if (errorcode) return errorcode;
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

        *_handle.output_stream << static_cast<int>(SubprocessCommand::TERMINATE);
        _handle.output_stream->flush();

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

    // 辅助函数，用于检查流操作是否失败
    int _check_stream_failure(std::ios* stream, int errorCode) {
       if (stream->fail()) {
            std::cerr << "Error: Stream operation failed at step " << errorCode
                    << ", stream state: " << stream->rdstate() << std::endl;
            return errorCode;
        }
        return 0;
    }

    // 子进程处理输入和输出流
    int _process_stream(std::istream& in_stream, std::ostream& out_stream) {
        // 预分配一个 4MB 的输入和输出缓冲区
        const size_t buffer_size = 4 * 1024 * 1024; // 4MB
        std::vector<unsigned char> input_buffer(buffer_size);
        std::vector<unsigned char> output_buffer(buffer_size);

        while (true) {
            int errorcode = 0;

            int command_type = 0;
            in_stream.read(reinterpret_cast<char*>(&command_type), sizeof(command_type));
            errorcode = _check_stream_failure(&in_stream, 12);
            if (errorcode) return errorcode;

            if (command_type == static_cast<int>(SubprocessCommand::TERMINATE)) {
                break;
            } else if (command_type == static_cast<int>(SubprocessCommand::RUN_TASK)) {
                size_t length = 0;
                in_stream.read(reinterpret_cast<char*>(&length), sizeof(length));
                errorcode = _check_stream_failure(&in_stream, 13);
                if (errorcode) return errorcode;

                // 如果输入数据大于缓冲区大小，分多次读取，防止溢出
                size_t remaining = length;
                size_t offset = 0;
                while (remaining > 0) {
                    size_t to_read = std::min(remaining, buffer_size);
                    in_stream.read(reinterpret_cast<char*>(input_buffer.data() + offset), to_read);
                    errorcode = _check_stream_failure(&in_stream, 14);
                    if (errorcode) return errorcode;
                    remaining -= to_read;
                    offset += to_read;
                }

                // 任务处理
                size_t output_size = buffer_size; // 初始设置为 4MB
                int result = _task_handler(input_buffer.data(), length, output_buffer.data(), &output_size);

                int32_t response_code = result;
                out_stream.write(reinterpret_cast<const char*>(&response_code), sizeof(response_code));
                errorcode = _check_stream_failure(&out_stream, 15);
                if (errorcode) return errorcode;

                if (response_code == 0) {
                    // 如果任务成功，写回数据长度和数据内容
                    out_stream.write(reinterpret_cast<const char*>(&output_size), sizeof(output_size));
                    errorcode = _check_stream_failure(&out_stream, 16);
                    if (errorcode) return errorcode;

                    out_stream.write(reinterpret_cast<const char*>(output_buffer.data()), output_size);
                    errorcode = _check_stream_failure(&out_stream, 17);
                    if (errorcode) return errorcode;
                }
                out_stream.flush();
                errorcode = _check_stream_failure(&out_stream, 8);
                if (errorcode) return errorcode;
            }
        }
        return 0;
    }
};

}