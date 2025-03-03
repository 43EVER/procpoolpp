# 查找所有示例源文件
file(GLOB EXAMPLE_SOURCES "*.cpp")

# 为每个示例创建可执行文件
foreach(EXAMPLE_SOURCE ${EXAMPLE_SOURCES})
    # 获取文件名（不含扩展名）作为示例名
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_SOURCE} NAME_WE)
    
    # 创建可执行文件
    add_executable(example_${EXAMPLE_NAME} ${EXAMPLE_SOURCE})
    
    # 链接库
    target_link_libraries(example_${EXAMPLE_NAME} PRIVATE procpoolpp)
    
    # 设置输出目录
    set_target_properties(example_${EXAMPLE_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
    )
    
    # 如果需要，可以为示例设置特定的编译选项
    # target_compile_options(example_${EXAMPLE_NAME} PRIVATE
    #     $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
    #     $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
    #     $<$<CXX_COMPILER_ID:MSVC>:/W4>
    # )
endforeach()