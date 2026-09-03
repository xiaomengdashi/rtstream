#pragma once

// 内嵌 HTTP 静态服务器 + MJPEG 预览通道（/stream.mjpeg）。
// 仅 GET；静态文件从 doc_root 读取；MJPEG 为 multipart/x-mixed-replace。

#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "common/logger.h"
#include "common/net_utils.h"
#include "web/web_previewer.h"

namespace rtstream {

class HttpServer {
public:
    explicit HttpServer(SharedJpeg& sink, std::string doc_root)
        : sink_(sink), doc_root_(std::move(doc_root)) {}
    ~HttpServer() { stop(); }

    void set_ws(WsServer* ws) { ws_ = ws; }

    bool start(uint16_t port, std::string& error) {
        listen_fd_ = create_tcp_socket();
        if (listen_fd_ < 0) { error = "http socket: " + errno_str(); return false; }
        set_reuseaddr(listen_fd_);
        if (!bind_socket(listen_fd_, port)) {
            error = "http bind " + std::to_string(port) + ": " + errno_str();
            return false;
        }
        if (::listen(listen_fd_, 8) < 0) { error = "http listen: " + errno_str(); return false; }
        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });
        RTS_LOGI("http", "listening on %u (root=%s)", port, doc_root_.c_str());
        return true;
    }

    void stop() {
        running_ = false;
        close_fd(listen_fd_);
        if (accept_thread_.joinable()) accept_thread_.join();
    }

private:
    void accept_loop() {
        while (running_) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_) break;
                if (errno == EINTR) continue;
                break;
            }
            set_tcp_nodelay(fd);
            std::thread([this, fd] { handle(fd); }).detach();
        }
    }

    static bool read_request(int fd, std::string& method, std::string& path,
                             bool& websocket_upgrade, std::string& ws_key) {
        std::string line;
        size_t maxlen = 8192;
        if (!read_line(fd, line, maxlen)) return false;
        std::istringstream is(line);
        if (!(is >> method >> path)) return false;
        websocket_upgrade = false;
        // 读剩余头部
        while (read_line(fd, line, maxlen)) {
            if (line.empty()) break;
            std::string lo;
            lo.reserve(line.size());
            for (char c : line) lo += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lo.rfind("upgrade:", 0) == 0 && lo.find("websocket") != std::string::npos)
                websocket_upgrade = true;
            if (lo.rfind("sec-websocket-key:", 0) == 0) {
                size_t v = lo.find(':') + 1;
                while (v < line.size() && line[v] == ' ') v++;
                ws_key = line.substr(v);
            }
        }
        size_t q = path.find('?');
        if (q != std::string::npos) path = path.substr(0, q);
        return true;
    }

    static bool read_line(int fd, std::string& line, size_t maxlen) {
        line.clear();
        char ch;
        while (line.size() < maxlen) {
            ssize_t n = ::recv(fd, &ch, 1, 0);
            if (n <= 0) return false;
            if (ch == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            line += ch;
        }
        return false;
    }

    static bool send_all(int fd, const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t off = 0;
        while (off < len) {
            ssize_t n = ::send(fd, p + off, len - off, MSG_NOSIGNAL);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }

    void handle(int fd) {
        std::string method, path, ws_key;
        bool ws_upgrade = false;
        if (!read_request(fd, method, path, ws_upgrade, ws_key)) {
            close_fd(fd);
            return;
        }
        if (ws_upgrade && ws_) {
            ws_->adopt(fd, ws_key);   // fd 所有权移交 WS（含关闭）
            return;
        }
        if (method != "GET") {
            close_fd(fd);
            return;
        }
        if (path == "/stream.mjpeg") {
            handle_mjpeg(fd);
        } else {
            handle_static(fd, path);
        }
        close_fd(fd);
    }

    void handle_static(int fd, const std::string& path) {
        std::string rel = (path == "/" || path.empty()) ? "/index.html" : path;
        // 防目录穿越
        if (rel.find("..") != std::string::npos) {
            send_simple(fd, 403, "Forbidden");
            return;
        }
        std::string full = doc_root_ + rel;
        std::ifstream in(full, std::ios::binary);
        if (!in) {
            send_simple(fd, 404, "Not Found");
            return;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string body = ss.str();
        std::string type = "application/octet-stream";
        if (rel.size() >= 5 && rel.substr(rel.size() - 5) == ".html") type = "text/html; charset=utf-8";
        else if (rel.size() >= 3 && rel.substr(rel.size() - 3) == ".js") type = "application/javascript; charset=utf-8";
        else if (rel.size() >= 4 && rel.substr(rel.size() - 4) == ".css") type = "text/css; charset=utf-8";
        else if (rel.size() >= 4 && rel.substr(rel.size() - 4) == ".png") type = "image/png";
        else if (rel.size() >= 4 && rel.substr(rel.size() - 4) == ".svg") type = "image/svg+xml";
        std::string head = "HTTP/1.1 200 OK\r\nContent-Type: " + type +
                           "\r\nContent-Length: " + std::to_string(body.size()) +
                           "\r\nCache-Control: no-cache\r\n\r\n";
        send_all(fd, head.data(), head.size());
        send_all(fd, body.data(), body.size());
    }

    void handle_mjpeg(int fd) {
        std::string head =
            "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=rtstream\r\n\r\n";
        if (!send_all(fd, head.data(), head.size())) return;
        uint64_t last_version = 0;
        uint8_t pre[] = "--rtstream\r\nContent-Type: image/jpeg\r\nContent-Length: ";
        while (running_) {
            std::vector<uint8_t> jpg;
            uint32_t w, h;
            {
                std::unique_lock<std::mutex> lk(sink_.mtx);
                if (!sink_.cv.wait_for(lk, std::chrono::milliseconds(500),
                                       [&] { return sink_.version != last_version; }))
                    continue;
                last_version = sink_.version;
                jpg = sink_.data;
                w = sink_.width;
                h = sink_.height;
            }
            (void)w; (void)h;
            char len[32];
            std::snprintf(len, sizeof(len), "%zu\r\n\r\n", jpg.size());
            if (!send_all(fd, pre, sizeof(pre) - 1)) break;
            if (!send_all(fd, len, std::strlen(len))) break;
            if (!send_all(fd, jpg.data(), jpg.size())) break;
            if (!send_all(fd, "\r\n", 2)) break;
        }
    }

    void send_simple(int fd, int code, const char* msg) {
        std::string body = msg;
        std::string resp = "HTTP/1.1 " + std::to_string(code) + " " + msg +
                           "\r\nContent-Type: text/plain\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body;
        send_all(fd, resp.data(), resp.size());
    }

    SharedJpeg& sink_;
    std::string doc_root_;
    WsServer* ws_ = nullptr;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
};

} // namespace rtstream
