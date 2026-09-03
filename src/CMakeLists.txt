# 业务模块层 + 公共组件层
# 链路（采集/编码/传输/接收/解码/统计）为 header-heavy 设计：
# 仅 x264_encoder 与 AVFoundation 采集有独立源文件；其余模块为头文件。

add_library(rtstream_core STATIC
    encoder/x264_encoder.cpp
)
target_include_directories(rtstream_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(rtstream_core PUBLIC
    nlohmann_json::nlohmann_json
    PkgConfig::X264
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWS
    pthread)

# V4L2 / epoll 为 Linux 专属代码路径（编译期 #if __linux__ 隔离）
if(UNIX AND NOT APPLE)
    target_compile_definitions(rtstream_core PUBLIC RTS_LINUX=1)
endif()

# macOS AVFoundation 摄像头采集（系统框架，无额外依赖）
option(RTSTREAM_ENABLE_AVF "Enable macOS AVFoundation camera capture" ON)
if(APPLE AND RTSTREAM_ENABLE_AVF)
    target_sources(rtstream_core PRIVATE capture/avf_capture.mm)
    set_source_files_properties(capture/avf_capture.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rtstream_core PUBLIC
        "-framework Foundation" "-framework AVFoundation"
        "-framework CoreMedia" "-framework CoreVideo")
    target_compile_definitions(rtstream_core PUBLIC RTSTREAM_HAVE_AVF=1)
endif()

# ---- Web 监控库（头文件实现） ----
add_library(rtstream_web INTERFACE)
target_link_libraries(rtstream_web INTERFACE rtstream_core)

# ---- 桌面渲染客户端：GLFW + OpenGL + GLAD + Dear ImGui ----
option(RTSTREAM_ENABLE_GL_CLIENT "Enable OpenGL/GLFW/ImGui desktop client" ON)

if(RTSTREAM_ENABLE_GL_CLIENT)
    find_package(glfw3 3.3 QUIET)
    if(NOT glfw3_FOUND)
        pkg_check_modules(GLFW3 QUIET glfw3)
    endif()
    if(NOT glfw3_FOUND AND NOT GLFW3_FOUND)
        message(WARNING "GLFW not found — desktop GL client disabled "
                        "(install libglfw3-dev / brew install glfw, "
                        "or pass -DRTSTREAM_ENABLE_GL_CLIENT=OFF)")
        set(RTSTREAM_ENABLE_GL_CLIENT OFF)
    endif()
endif()

if(RTSTREAM_ENABLE_GL_CLIENT)
    if(NOT glfw3_FOUND)
        # pkg-config 找到的 GLFW：包一个同名 INTERFACE target，统一链接方式
        add_library(glfw INTERFACE)
        target_include_directories(glfw INTERFACE ${GLFW3_INCLUDE_DIRS})
        target_link_directories(glfw INTERFACE ${GLFW3_LIBRARY_DIRS})
        target_link_libraries(glfw INTERFACE ${GLFW3_LIBRARIES})
    endif()

    # imgui 已在 cmake/deps.cmake 中声明；glad2 生成 GL 3.3 Core 加载代码（需 python3）
    FetchContent_MakeAvailable(imgui glad)
    find_package(Python COMPONENTS Interpreter REQUIRED)
    set(GLAD_OUT "${CMAKE_CURRENT_BINARY_DIR}/gladsources/glad")
    add_custom_command(
        OUTPUT "${GLAD_OUT}/include/glad/gl.h" "${GLAD_OUT}/src/gl.c"
        COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${glad_SOURCE_DIR}"
                ${Python_EXECUTABLE} -m glad
                --out-path "${GLAD_OUT}" --api "gl:core=3.3" c
        COMMENT "glad2: generating GL 3.3 Core loader"
        VERBATIM)
    add_library(glad STATIC "${GLAD_OUT}/src/gl.c")
    target_include_directories(glad PUBLIC "${GLAD_OUT}/include")
    target_link_libraries(glad PUBLIC ${CMAKE_DL_LIBS})

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC glfw)
    target_compile_options(imgui PRIVATE -w)   # 第三方源码，屏蔽告警

    add_library(rtstream_gl INTERFACE)
    target_link_libraries(rtstream_gl INTERFACE imgui glad rtstream_core)
    target_compile_definitions(rtstream_gl INTERFACE RTSTREAM_ENABLE_GL_CLIENT=1)
endif()
