cmake_minimum_required(VERSION 3.14)
project(procpoolpp VERSION 0.1.0 LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 Boost 依赖
find_package(Boost REQUIRED COMPONENTS iostreams system filesystem)

# 查找 Babylon 依赖
set(BUILD_DEPS ON)

include(FetchContent)
FetchContent_Declare(
  babylon
  URL "https://github.com/baidu/babylon/releases/download/v1.4.2/v1.4.2.tar.gz"
  URL_HASH SHA256=d60ee9cd86a777137bf021c8861e97438a69cc857659d5eb39af9e8464434cf1
)
FetchContent_MakeAvailable(babylon)

# 创建一个 INTERFACE 库目标（header-only 库专用）
add_library(${PROJECT_NAME} INTERFACE)

# 设置包含目录
target_include_directories(${PROJECT_NAME} INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

# 链接 Babylon 和 Boost 依赖
target_link_libraries(${PROJECT_NAME} INTERFACE 
    babylon
    Boost::iostreams
    Boost::system
    Boost::filesystem
)

# 添加示例（可选）
add_subdirectory(examples)