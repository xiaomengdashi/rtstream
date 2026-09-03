#pragma once

// 内嵌 WebSocket 服务器（RFC 6455 最小实现）：
// - 握手：HTTP GET + Sec-WebSocket-Key → SHA1+Base64 Accept
// - 帧：服务端发送 unmasked binary/text；接收客户端 masked 帧，自动聚合分片
// - 每客户端接收线程；发送按客户端串行（mutex）
// 消息回调在接收线程执行，注意线程安全。

#include <atomic>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/logger.h"
#include "common/net_utils.h"
#include "common/sha1.h"

namespace rtstream {

class WsServer {
public:
    ~WsServer() { stop(); }

    using MessageFn = std::function<void(int client_id, const std::string& text)>;

    void set_on_message(MessageFn fn) { on_message_ = std::move(fn); }

    // 由 HTTP 服务器"过户"连接：请求行与头部（含 Sec-WebSocket-Key）已被
    // HTTP 解析消费，此处直接构造 101 响应并挂载客户端循环。
    bool adopt(int fd, const std::string& ws_key) {
        if (ws_key.empty() || !send_handshake_response(fd, ws_key)) {
            close_fd(fd);
            return false;
        }
        running_ = true;   // WS 挂载在 HTTP 上，无独立 accept 线程；有客户端过户即视为活跃
        auto c = std::make_shared<Client>();
        c->id = next_id_++;
        c->fd = fd;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            clients_.push_back(c);
        }
        c->thread = std::thread([this, c] { client_loop(c); });
        RTS_LOGI("ws", "client #%d adopted", c->id);
        return true;
    }

    bool start(uint16_t port, std::string& error) {
        listen_fd_ = create_tcp_socket();
        if (listen_fd_ < 0) { error = "ws socket: " + errno_str(); return false; }
        set_reuseaddr(listen_fd_);
        if (!bind_socket(listen_fd_, port)) {
            error = "ws bind " + std::to_string(port) + ": " + errno_str();
            return false;
        }
        if (::listen(listen_fd_, 8) < 0) { error = "ws listen: " + errno_str(); return false; }
        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });
        RTS_LOGI("ws", "listening on %u", port);
        return true;
    }

    void stop() {
        running_ = false;
        close_fd(listen_fd_);
        std::vector<std::shared_ptr<Client>> snapshot;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            snapshot = clients_;
            for (auto& c : snapshot) close_fd(c->fd);
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& c : snapshot)
            if (c->thread.joinable()) c->thread.join();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            clients_.clear();
        }
    }

    void broadcast_binary(const uint8_t* data, size_t len) {
        broadcast(0x2, data, len);
    }

    void broadcast_text(const std::string& text) {
        broadcast(0x1, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

private:
    struct Client {
        int id = 0;
        int fd = -1;
        std::thread thread;
        std::mutex send_mtx;
        std::atomic<bool> alive{true};
    };

    void accept_loop() {
        while (running_) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_) break;
                if (errno == EINTR) continue;
                break;
            }
            if (!do_handshake(fd)) {
                close_fd(fd);
                continue;
            }
            auto c = std::make_shared<Client>();
            c->id = next_id_++;
            c->fd = fd;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                clients_.push_back(c);
            }
            c->thread = std::thread([this, c] { client_loop(c); });
            RTS_LOGI("ws", "client #%d connected", c->id);
        }
    }

    static bool read_line(int fd, std::string& line, size_t maxlen = 8192) {
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

    // 独立监听模式：读取请求行与头部并完成握手（当前主要走 HTTP 过户路径）
    static bool do_handshake(int fd) {
        std::string line;
        if (!read_line(fd, line)) return false;
        if (line.rfind("GET ", 0) != 0) return false;
        std::string key;
        while (read_line(fd, line)) {
            if (line.empty()) break;
            std::string lo;
            lo.reserve(line.size());
            for (char ch : line)
                lo += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const std::string kkey = "sec-websocket-key:";
            if (lo.rfind(kkey, 0) == 0) {
                size_t s = kkey.size();
                while (s < line.size() && line[s] == ' ') s++;
                key = line.substr(s);
            }
        }
        if (key.empty()) return false;
        return send_handshake_response(fd, key);
    }

    static bool send_handshake_response(int fd, const std::string& key) {
        const char* guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string accept_src = key + guid;
        uint8_t digest[20];
        sha1(reinterpret_cast<const uint8_t*>(accept_src.data()), accept_src.size(), digest);
        std::string accept = base64_encode(digest, 20);
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        return send_all(fd, resp.data(), resp.size());
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

    void broadcast(uint8_t opcode, const uint8_t* data, size_t len) {
        std::vector<std::shared_ptr<Client>> snapshot;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            snapshot = clients_;
        }
        std::vector<uint8_t> frame;
        frame.reserve(len + 10);
        frame.push_back(0x80 | opcode);  // FIN + opcode
        if (len < 126) {
            frame.push_back(static_cast<uint8_t>(len));
        } else if (len <= 0xFFFF) {
            frame.push_back(126);
            frame.push_back(static_cast<uint8_t>(len >> 8));
            frame.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<uint8_t>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
        }
        frame.insert(frame.end(), data, data + len);

        for (auto& c : snapshot) {
            if (!c->alive) continue;
            std::lock_guard<std::mutex> lk(c->send_mtx);
            if (!send_all(c->fd, frame.data(), frame.size())) {
                RTS_LOGW("ws", "client #%d send failed (errno=%d %s), dropping",
                         c->id, errno, errno_str().c_str());
                c->alive = false;
                close_fd(c->fd);
            }
        }
    }

    // 接收循环：解析帧（支持分片聚合与 ping/pong）
    void client_loop(std::shared_ptr<Client> c) {
        std::vector<uint8_t> buf;
        uint8_t chunk[16384];
        int cur_opcode = 0;
        std::vector<uint8_t> msg;
        while (c->alive && running_) {
            ssize_t n = ::recv(c->fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buf.insert(buf.end(), chunk, chunk + n);

            for (;;) {
                if (buf.size() < 2) break;
                bool fin = buf[0] & 0x80;
                uint8_t opcode = buf[0] & 0x0F;
                bool masked = buf[1] & 0x80;
                uint64_t len = buf[1] & 0x7F;
                size_t hdr = 2;
                if (len == 126) {
                    if (buf.size() < 4) break;
                    len = (uint64_t(buf[2]) << 8) | buf[3];
                    hdr = 4;
                } else if (len == 127) {
                    if (buf.size() < 10) break;
                    len = 0;
                    for (int i = 2; i < 10; ++i) len = (len << 8) | buf[i];
                    hdr = 10;
                }
                uint8_t mask[4] = {0, 0, 0, 0};
                if (masked) {
                    if (buf.size() < hdr + 4) break;
                    std::memcpy(mask, buf.data() + hdr, 4);
                    hdr += 4;
                }
                if (buf.size() < hdr + len) break;

                std::vector<uint8_t> payload(buf.begin() + hdr, buf.begin() + hdr + len);
                if (masked)
                    for (size_t i = 0; i < payload.size(); ++i)
                        payload[i] ^= mask[i % 4];
                buf.erase(buf.begin(), buf.begin() + static_cast<long>(hdr + len));

                if (opcode == 0x8) {  // close
                    c->alive = false;
                    break;
                }
                if (opcode == 0x9) {  // ping → pong
                    std::vector<uint8_t> pong;
                    pong.push_back(0x8A);
                    pong.push_back(static_cast<uint8_t>(payload.size() & 0x7F));
                    pong.insert(pong.end(), payload.begin(), payload.end());
                    std::lock_guard<std::mutex> lk(c->send_mtx);
                    send_all(c->fd, pong.data(), pong.size());
                    continue;
                }
                if (opcode == 0xA) continue;  // pong
                if (opcode == 0x1 || opcode == 0x2) {
                    cur_opcode = opcode;
                    msg = payload;
                } else if (opcode == 0x0) {  // continuation
                    msg.insert(msg.end(), payload.begin(), payload.end());
                }
                if (fin && !msg.empty()) {
                    if (cur_opcode == 0x1 && on_message_) {
                        std::string text(msg.begin(), msg.end());
                        on_message_(c->id, text);
                    }
                    msg.clear();
                }
            }
        }
        c->alive = false;
        if (running_)
            RTS_LOGW("ws", "client #%d recv loop exit (errno=%d %s)",
                     c->id, errno, errno_str().c_str());
        close_fd(c->fd);
        RTS_LOGI("ws", "client #%d disconnected", c->id);
    }

    std::atomic<bool> running_{false};
    std::atomic<int> next_id_{1};
    int listen_fd_ = -1;
    std::thread accept_thread_;
    std::mutex mtx_;
    std::vector<std::shared_ptr<Client>> clients_;
    MessageFn on_message_;
};

} // namespace rtstream
