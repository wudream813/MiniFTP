#include "ftp_server.h"
#include "utils.h"

namespace miniftp {

bool FtpServer::start(std::string& err) {
    uint16_t real = 0;
    listen_ = create_listen_socket(cfg_.port, &real);
    if (listen_ == INVALID_SOCK) {
        err = "bind port " + std::to_string(cfg_.port) + " failed: " + last_net_error() +
              " (端口被占用? 换一个: miniftp --port 2122)";
        return false;
    }
    cfg_.port = real;
    running_ = true;
    accept_thread_ = std::thread(&FtpServer::accept_loop, this);
    return true;
}

void FtpServer::stop() {
    bool was = running_.exchange(false);
    if (!was && listen_ == INVALID_SOCK) return;
    if (listen_ != INVALID_SOCK) { close_socket(listen_); listen_ = INVALID_SOCK; }
    if (accept_thread_.joinable()) {
        // 避免在 accept 线程自己身上 join(暂无此调用路径,防御一下)
        if (std::this_thread::get_id() != accept_thread_.get_id())
            accept_thread_.join();
        else
            accept_thread_.detach();
    }
    std::lock_guard<std::mutex> lk(workers_mtx_);
    for (auto& t : workers_) {
        if (t.joinable()) {
            // 会话线程多为阻塞在 recv 上;进程退出时由 detach 收尾
            // 这里 detach,避免 stop() 卡死。
            t.detach();
        }
    }
    workers_.clear();
}

void FtpServer::join() {
    if (accept_thread_.joinable()) accept_thread_.join();
}

void FtpServer::accept_loop() {
    log_msg("srv", "listening on 0.0.0.0:" + std::to_string(cfg_.port) +
                   " root=" + cfg_.root);
    while (running_) {
        bool timed_out = false;
        std::string peer;
        socket_t c = accept_with_timeout(listen_, 500, &peer, &timed_out);
        if (!running_) { if (c != INVALID_SOCK) close_socket(c); break; }
        if (c == INVALID_SOCK) continue; // 超时,继续轮询 running_
        if (peer.empty()) peer = socket_peer_ip(c);
        if (peer.empty()) peer = "unknown";
        try {
            std::lock_guard<std::mutex> lk(workers_mtx_);
            workers_.emplace_back([c, peer, cfg = cfg_, &run = running_]() mutable {
                handle_ftp_session(c, peer, cfg, run);
            });
        } catch (...) {
            close_socket(c);
        }
    }
}

} // namespace miniftp
