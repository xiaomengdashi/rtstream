# cmake/deps.cmake —— 第三方依赖统一经 FetchContent 拉取（首次配置需联网）
include(FetchContent)

# ---- Dear ImGui（即时模式 GUI，桌面客户端指标面板；仓库无 CMakeLists，目标在 src 中手工定义） ----
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4
    GIT_SHALLOW TRUE)

# ---- CLI11（命令行解析，官方 release 单头工件）---- 目标：cli11
FetchContent_Declare(cli11
    URL https://github.com/CLIUtils/CLI11/releases/download/v2.4.2/cli11.hpp
    DOWNLOAD_NO_EXTRACT TRUE)

# ---- nlohmann/json（JSON 序列化）---- 目标：nlohmann_json::nlohmann_json
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)

# ---- glad2（OpenGL 函数加载器，配置期生成，需 python3）---- 见 src 中的 glad_add_library
FetchContent_Declare(glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v2.0.8)

# cli11 / json 无条件拉取（server/client 都用）；imgui / glad 仅桌面客户端启用时拉取
FetchContent_MakeAvailable(cli11 json)
add_library(cli11 INTERFACE)
target_include_directories(cli11 INTERFACE "${cli11_SOURCE_DIR}")
