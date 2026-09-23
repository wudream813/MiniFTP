#include "net.h"

#include <chrono>
#include <cstring>
#include <thread>

#ifndef _WIN32
  #include <ifaddrs.h>
  #include <net/if.h>
#endif

namespace miniftp {

NetInit::NetInit() {
#ifdef _WIN32
    WSADATA wsa;
    int r = ::WSAStartup(MAKEWORD(2, 2), &wsa);
    if (r != 0) {
        ok_ = false;
        err_ = "WSAStartup failed, code=" + std::to_string(r);
    }
#endif
}

NetInit::~NetInit() {
#ifdef _WIN32
    if (ok_) ::WSACleanup();
#endif
}

void close_socket(socket_t s) {
    if (s == INVALID_SOCK) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

std::string last_net_error() {
#ifdef _WIN32
    int e = ::WSAGetLastError();
    return "WSA err " + std::to_string(e);
#else
    return std::string(::strerror(errno)) + " (errno " + std::to_string(errno) + ")";
#endif
}

socket_t create_listen_socket(uint16_t port, uint16_t* out_port, int backlog) {
    socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) return INVALID_SOCK;

    int one = 1;
#ifdef _WIN32
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#else
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        close_socket(s);
        return INVALID_SOCK;
    }
    if (::listen(s, backlog) != 0) {
        close_socket(s);
        return INVALID_SOCK;
    }
    if (out_port) {
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(s, (sockaddr*)&bound, &len) == 0) {
            *out_port = ntohs(bound.sin_port);
        } else {
            *out_port = port;
        }
    }
    return s;
}

static bool wait_readable(socket_t s, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
#ifdef _WIN32
    FD_SET(s, &rfds);
#else
    FD_SET(s, &rfds);
#endif
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    int r = ::select(0, &rfds, nullptr, nullptr, timeout_ms < 0 ? nullptr : &tv);
#else
    int r = ::select((int)s + 1, &rfds, nullptr, nullptr, timeout_ms < 0 ? nullptr : &tv);
#endif
    return r > 0;
}

socket_t accept_with_timeout(socket_t listen_sock, int timeout_ms,
                             std::string* out_peer_ip, bool* timed_out) {
    if (timed_out) *timed_out = false;
    if (timeout_ms > 0 && !wait_readable(listen_sock, timeout_ms)) {
        if (timed_out) *timed_out = true;
        return INVALID_SOCK;
    }
    sockaddr_in cli{};
    socklen_t len = sizeof(cli);
    socket_t c = ::accept(listen_sock, (sockaddr*)&cli, &len);
    if (c == INVALID_SOCK) return INVALID_SOCK;
    if (out_peer_ip) {
        char buf[64] = {0};
#ifdef _WIN32
        ::InetNtopA(AF_INET, &cli.sin_addr, buf, sizeof(buf));
#else
        ::inet_ntop(AF_INET, &cli.sin_addr, buf, sizeof(buf));
#endif
        *out_peer_ip = buf;
    }
    return c;
}

socket_t tcp_connect(const std::string& host, uint16_t port, int timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        if (res) ::freeaddrinfo(res);
        return INVALID_SOCK;
    }
    socket_t s = INVALID_SOCK;
    for (addrinfo* p = res; p; p = p->ai_next) {
        s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCK) continue;
        sockaddr_in* a = (sockaddr_in*)p->ai_addr;
        a->sin_port = htons(port);

#ifdef _WIN32
        // Windows: 非阻塞 connect + select 超时
        u_long nb = 1;
        ::ioctlsocket(s, FIONBIO, &nb);
        int r = ::connect(s, (sockaddr*)a, sizeof(*a));
        if (r != 0) {
            int e = ::WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
                fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
                timeval tv{}; tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;
                r = ::select(0, nullptr, &wfds, nullptr, &tv);
                if (r <= 0) { close_socket(s); s = INVALID_SOCK; continue; }
            } else { close_socket(s); s = INVALID_SOCK; continue; }
        }
        nb = 0;
        ::ioctlsocket(s, FIONBIO, &nb);
#else
        int flags = ::fcntl(s, F_GETFL, 0);
        ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int r = ::connect(s, (sockaddr*)a, sizeof(*a));
        if (r != 0 && errno != EINPROGRESS) {
            close_socket(s); s = INVALID_SOCK; continue;
        }
        if (r != 0) {
            fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
            timeval tv{}; tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            r = ::select(s + 1, nullptr, &wfds, nullptr, &tv);
            if (r <= 0) { close_socket(s); s = INVALID_SOCK; continue; }
            int so_err = 0; socklen_t sl = sizeof(so_err);
            ::getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &sl);
            if (so_err != 0) { close_socket(s); s = INVALID_SOCK; continue; }
        }
        ::fcntl(s, F_SETFL, flags);
#endif
        break; // 成功
    }
    ::freeaddrinfo(res);
    return s;
}

bool send_all(socket_t s, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = ::send(s, buf + sent, (int)(len - sent), 0);
        if (n == SOCKET_ERROR) return false;
#else
        ssize_t n = ::send(s, buf + sent, len - sent, 0);
        if (n <= 0) return false;
#endif
        sent += (size_t)n;
    }
    return true;
}

bool recv_line(socket_t s, std::string& out, int timeout_ms) {
    out.clear();
    char c = 0;
    std::string acc;
    acc.reserve(256);
    while (true) {
        if (timeout_ms > 0 && !wait_readable(s, timeout_ms)) return false;
#ifdef _WIN32
        int n = ::recv(s, &c, 1, 0);
        if (n <= 0) return false;
#else
        ssize_t n = ::recv(s, &c, 1, 0);
        if (n <= 0) return false;
#endif
        if (c == '\n') break;
        acc.push_back(c);
        if (acc.size() > 8192) break; // 防超长行攻击
    }
    while (!acc.empty() && (acc.back() == '\r' || acc.back() == '\n')) acc.pop_back();
    out = acc;
    return true;
}

std::string socket_local_ip(socket_t s) {
    sockaddr_in a{}; socklen_t len = sizeof(a);
    if (::getsockname(s, (sockaddr*)&a, &len) != 0) return "";
    char buf[64] = {0};
#ifdef _WIN32
    ::InetNtopA(AF_INET, &a.sin_addr, buf, sizeof(buf));
#else
    ::inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
#endif
    return std::string(buf);
}

std::string socket_peer_ip(socket_t s) {
    sockaddr_in a{}; socklen_t len = sizeof(a);
    if (::getpeername(s, (sockaddr*)&a, &len) != 0) return "";
    char buf[64] = {0};
#ifdef _WIN32
    ::InetNtopA(AF_INET, &a.sin_addr, buf, sizeof(buf));
#else
    ::inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
#endif
    return std::string(buf);
}

std::vector<std::string> local_lan_ips() {
    std::vector<std::string> ips;
#ifdef _WIN32
    char host[256] = {0};
    if (::gethostname(host, sizeof(host)) == 0) {
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host, nullptr, &hints, &res) == 0) {
            for (addrinfo* p = res; p; p = p->ai_next) {
                sockaddr_in* a = (sockaddr_in*)p->ai_addr;
                char buf[64] = {0};
                ::InetNtopA(AF_INET, &a->sin_addr, buf, sizeof(buf));
                std::string ip = buf;
                if (ip != "127.0.0.1" && !ip.empty()) ips.push_back(ip);
            }
            ::freeaddrinfo(res);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) == 0) {
        for (ifaddrs* p = list; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!(p->ifa_flags & IFF_UP) || !(p->ifa_flags & IFF_RUNNING)) continue;
            if (p->ifa_flags & IFF_LOOPBACK) continue;
            sockaddr_in* a = (sockaddr_in*)p->ifa_addr;
            char buf[64] = {0};
            if (::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf))) {
                ips.push_back(buf);
            }
        }
        ::freeifaddrs(list);
    }
#endif
    return ips;
}

bool parse_ipv4(const std::string& ip, int out[4]) {
    int a, b, c, d; char tail = 0;
    if (sscanf(ip.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
    return true;
}

} // namespace miniftp
