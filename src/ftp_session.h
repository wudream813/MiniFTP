#pragma once
// MiniFTP - 单个客户端会话(控制连接 + 数据连接)

#include <atomic>
#include <cstdint>
#include <string>

#include "net.h"

namespace miniftp {

struct ServerConfig {
    std::string root;          // 共享根目录(绝对路径)
    uint16_t port = 2121;      // 控制端口
    std::string user;          // 指定用户名(空=允许匿名)
    std::string pass;          // 指定密码(空=空密码即可)
    bool allow_anonymous = true;
    bool allow_write = true;   // false=只读
    std::string pasv_ip;       // 被动模式宣告 IP(空=自动)
    uint16_t pasv_min = 0;     // 被动端口范围,0=临时端口
    uint16_t pasv_max = 0;
};

// 处理一个控制连接(阻塞直到 QUIT/断开)。在独立线程里调用。
void handle_ftp_session(socket_t ctrl, std::string client_ip,
                        const ServerConfig& cfg,
                        std::atomic<bool>& server_running);

} // namespace miniftp
