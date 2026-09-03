# 入口层

find_package(Threads REQUIRED)

add_executable(rtstream_server server_main.cpp)
target_link_libraries(rtstream_server PRIVATE rtstream_web cli11 Threads::Threads)

# 客户端：有 GL 客户端库时链接（OpenGL+ImGui 窗口渲染），
# 否则自动降级为仅 headless（链路验证），渲染由服务端 Web 面板承担
if(TARGET rtstream_gl)
    add_executable(rtstream_client client_main.cpp)
    target_link_libraries(rtstream_client PRIVATE rtstream_gl cli11 Threads::Threads)
else()
    add_executable(rtstream_client client_main.cpp)
    target_link_libraries(rtstream_client PRIVATE rtstream_core cli11 Threads::Threads)
endif()
