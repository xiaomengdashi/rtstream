cmake_minimum_required(VERSION 3.16)
project(rtstream C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra)

# 桌面渲染客户端开关（依赖 GLFW；第三方库统一经 cmake/deps.cmake FetchContent 拉取）
option(RTSTREAM_ENABLE_GL_CLIENT "Enable OpenGL/GLFW/ImGui desktop client" ON)
include(cmake/deps.cmake)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

find_package(PkgConfig REQUIRED)
# IMPORTED_TARGET：以 PkgConfig::xxx 形式传递 include/link 目录，避免丢 -L 路径
pkg_check_modules(X264    REQUIRED IMPORTED_TARGET x264)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL  REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWS     REQUIRED IMPORTED_TARGET libswscale)

add_subdirectory(src)
add_subdirectory(app)
