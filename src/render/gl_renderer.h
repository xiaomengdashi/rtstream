#pragma once

// YUV(I420) → RGB 渲染器：GL_RED 纹理 ×3 + shader 色度转换（BT.601）。
// GL 函数来自 glad（gladLoadGLLoader 之后方可使用）。

#include <string>

#include <glad/gl.h>

#include "common/logger.h"
#include "common/media_types.h"

namespace rtstream {

class GlRenderer {
public:
    bool init(std::string& error) {
        static const char* vs = R"(
            #version 330 core
            layout (location = 0) in vec2 a_pos;
            layout (location = 1) in vec2 a_uv;
            out vec2 v_uv;
            void main() {
                v_uv = a_uv;
                gl_Position = vec4(a_pos, 0.0, 1.0);
            })";
        static const char* fs = R"(
            #version 330 core
            in vec2 v_uv;
            out vec4 frag;
            uniform sampler2D tex_y;
            uniform sampler2D tex_u;
            uniform sampler2D tex_v;
            void main() {
                float y = texture(tex_y, v_uv).r;
                float u = texture(tex_u, v_uv).r - 0.5;
                float v = texture(tex_v, v_uv).r - 0.5;
                float r = y + 1.402 * v;
                float g = y - 0.344 * u - 0.714 * v;
                float b = y + 1.772 * u;
                frag = vec4(clamp(r,0.,1.), clamp(g,0.,1.), clamp(b,0.,1.), 1.0);
            })";

        prog_ = build_program(vs, fs, error);
        if (prog_ == 0) return false;

        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        // 全屏三角条：pos(2) + uv(2)
        static const GLfloat verts[] = {
            -1.f, -1.f, 0.f, 1.f,
             1.f, -1.f, 1.f, 1.f,
            -1.f,  1.f, 0.f, 0.f,
             1.f,  1.f, 1.f, 0.f,
        };
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<const void*>(8));
        glBindVertexArray(0);

        glUseProgram(prog_);
        glUniform1i(glGetUniformLocation(prog_, "tex_y"), 0);
        glUniform1i(glGetUniformLocation(prog_, "tex_u"), 1);
        glUniform1i(glGetUniformLocation(prog_, "tex_v"), 2);

        glGenTextures(3, tex_);
        for (int i = 0; i < 3; ++i) {
            glBindTexture(GL_TEXTURE_2D, tex_[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        return true;
    }

    // 上传 I420 帧并绘制全屏
    void draw(const Frame& f) {
        const uint8_t* Y = f.data.data();
        const uint8_t* U = Y + static_cast<size_t>(f.width) * f.height;
        const uint8_t* V = U + static_cast<size_t>(f.width) * f.height / 4;
        upload(0, Y, f.width, f.height);
        upload(1, U, f.width / 2, f.height / 2);
        upload(2, V, f.width / 2, f.height / 2);

        glUseProgram(prog_);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
    }

private:
    void upload(int idx, const uint8_t* data, uint32_t w, uint32_t h) {
        glActiveTexture(GL_TEXTURE0 + idx);
        glBindTexture(GL_TEXTURE_2D, tex_[idx]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED,
                     static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
                     GL_RED, GL_UNSIGNED_BYTE, data);
    }

    GLuint build_program(const char* vs, const char* fs, std::string& error) {
        auto compile = [&](GLenum type, const char* src) -> GLuint {
            GLuint sh = glCreateShader(type);
            glShaderSource(sh, 1, &src, nullptr);
            glCompileShader(sh);
            GLint ok = 0;
            glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[512];
                glGetShaderInfoLog(sh, 512, nullptr, log);
                error = std::string("shader compile: ") + log;
                return 0;
            }
            return sh;
        };
        GLuint v = compile(GL_VERTEX_SHADER, vs);
        GLuint f = compile(GL_FRAGMENT_SHADER, fs);
        if (!v || !f) return 0;
        GLuint p = glCreateProgram();
        glAttachShader(p, v);
        glAttachShader(p, f);
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(p, 512, nullptr, log);
            error = std::string("program link: ") + log;
            return 0;
        }
        glDeleteShader(v);
        glDeleteShader(f);
        return p;
    }

    GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
    GLuint tex_[3] = {0, 0, 0};
};

} // namespace rtstream
