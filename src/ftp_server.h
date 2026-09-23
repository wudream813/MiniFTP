#pragma once
// MiniFTP - 服务器:监听 + 多线程 accept

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ftp_session.h"
#include "net.h"

namespace miniftp {

class FtpServer {
public:
    explicit FtpServer(ServerConfig cfg) : cfg_(std::move(cfg)) {}
    ~FtpServer() { stop(); }

    bool start(std::string& err);
    void stop();
    void join();
    bool running() const { return running_; }
    uint16_t port() const { return cfg_.port; }

private:
    void accept_loop();

    ServerConfig cfg_;
    socket_t listen_ = INVALID_SOCK;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex workers_mtx_;
    std::vector<std::thread> workers_;
};

} // namespace miniftp
