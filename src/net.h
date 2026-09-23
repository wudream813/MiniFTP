#pragma once
// MiniFTP - 跨平台 socket 封装层 (Windows Winsock / POSIX)
// 只依赖标准库 + 系统 socket API,无第三方库。

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socket_t = SOCKET;
  constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  using socket_t = int;
  constexpr socket_t INVALID_SOCK = -1;
#endif

namespace miniftp {

// Windows 下 WSAStartup/WSACleanup 的 RAII 封装,POSIX 下为空操作。
class NetInit {
public:
    NetInit();
    ~NetInit();
    bool ok() const { return ok_; }
    std::string error() const { return err_; }
private:
    bool ok_ = true;
    std::string err_;
};

void close_socket(socket_t s);
std::string last_net_error();

// 创建监听 socket,bind 0.0.0.0:port(port=0 则系统分配临时端口)
// 成功返回 socket,并通过 out_port 写回实际端口;失败返回 INVALID_SOCK。
socket_t create_listen_socket(uint16_t port, uint16_t* out_port, int backlog = 8);

// 带超时的 accept(毫秒),超时返回 INVALID_SOCK 且 timed_out=true。
socket_t accept_with_timeout(socket_t listen_sock, int timeout_ms,
                             std::string* out_peer_ip, bool* timed_out);

// 连接到远端 TCP,带超时(毫秒)。成功返回 socket,失败 INVALID_SOCK。
socket_t tcp_connect(const std::string& host, uint16_t port, int timeout_ms = 10000);

// 把 buf 全部发完。成功 true。
bool send_all(socket_t s, const char* buf, size_t len);

// 收一行(以 \n 结尾,自动去掉尾部 \r\n)。返回 false 表示连接断开/出错。
bool recv_line(socket_t s, std::string& out, int timeout_ms = 0);

// 取 socket 本地端 IP(点分十进制)。失败返回 ""。
std::string socket_local_ip(socket_t s);

// 取 socket 对端 IP。失败返回 ""。
std::string socket_peer_ip(socket_t s);

// 本机所有局域网 IPv4(过滤 127.0.0.1)。跨平台实现。
std::vector<std::string> local_lan_ips();

// "192.168.1.5" -> {192,168,1,5}
bool parse_ipv4(const std::string& ip, int out[4]);

} // namespace miniftp
