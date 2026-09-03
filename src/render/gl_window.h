#pragma once

// GLFW 窗口封装：创建窗口 + OpenGL 3.3 Core 上下文。
// GL 函数加载由 glad 完成（见 gl_renderer 的 init 前置步骤）。

#include <string>

#include <GLFW/glfw3.h>
#include <glad/gl.h>

namespace rtstream {

class GlWindow {
public:
    ~GlWindow() {
        if (win_) glfwDestroyWindow(win_);
        glfwTerminate();
    }

    bool open(int width, int height, const std::string& title, std::string& error) {
        if (!glfwInit()) {
            error = "glfwInit failed";
            return false;
        }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);  // macOS 必需
        win_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
        if (!win_) {
            error = "glfwCreateWindow failed";
            glfwTerminate();
            return false;
        }
        glfwMakeContextCurrent(win_);
        glfwSwapInterval(1);  // 垂直同步
        return true;
    }

    // 必须在 MakeContextCurrent 之后调用：glad2 经 glfwGetProcAddress 加载全部 GL 函数
    bool load_gl(std::string& error) {
        if (gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)) == 0) {
            error = "gladLoadGL failed";
            return false;
        }
        return true;
    }

    bool should_close() const { return glfwWindowShouldClose(win_); }
    void poll_events() { glfwPollEvents(); }
    void swap_buffers() { glfwSwapBuffers(win_); }
    void get_size(int& w, int& h) { glfwGetFramebufferSize(win_, &w, &h); }
    GLFWwindow* handle() const { return win_; }

private:
    GLFWwindow* win_ = nullptr;
};

} // namespace rtstream
