#include <iostream>
#include <chrono>
#include <procpoolpp/process_executor.hpp>

int main() {
    auto p = procpoolpp::ProcessExecutor(4, 1000, []() {
        auto f = [](const unsigned char* data, size_t size, unsigned char* outputBuffer, size_t* outputSize) -> int {
            std::cout << "开始处理数据..." << std::endl;
            
            // 记录开始时间（微秒级别）
            auto start = std::chrono::high_resolution_clock::now();
            
            // 将输入数据复制到输出缓冲区
            memcpy(outputBuffer, data, size);
            
            // 设置输出大小
            *outputSize = size;
            
            // 记录结束时间并计算耗时（微秒级别）
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            
            std::cout << "数据处理完成! 耗时: " << duration << " 微秒, 数据大小: " << size << " 字节" << std::endl;
            
            return 0; // 成功返回0
        };

        return f;
    });
    
    std::cout << "fuck" << std::endl;
    // 这里可以添加测试代码，例如：
    // 100M
    auto testData = std::vector<unsigned char>(3 * (1 << 20), 100);
    auto outputBuffer = std::vector<unsigned char>(200 * (1 << 20));
    size_t outputSize;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    int a = p.submit(testData.data(), testData.size(), outputBuffer.data(), &outputSize, deadline);
    if (a) {
        std::cout << a << std::endl;
    }


    std::cout << "处理完成，输入大小: " << testData.size() << " 字节, 输出大小: " << outputSize << " 字节" << std::endl;
}