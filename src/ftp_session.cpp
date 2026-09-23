#include "ftp_session.h"
#include "utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace miniftp {
namespace fs = std::filesystem;

namespace {

class Session {
public:
    Session(socket_t ctrl, std::string client_ip, const ServerConfig& cfg)
        : ctrl_(ctrl), client_ip_(std::move(client_ip)), cfg_(cfg) {}

    void run() {
        log_msg(client_ip_, "connected");
        reply(220, "MiniFTP ready. (https://github.com/miniftp)");
        std::string line;
        while (true) {
            if (!recv_line(ctrl_, line, 0)) break; // 断开
            if (line.empty()) continue;
            std::string cmd, arg;
            split_cmd(line, cmd, arg);
            log_msg(client_ip_, "C> " + cmd + (arg.empty() ? "" : " " + mask_arg(cmd, arg)));
            if (!dispatch(cmd, arg)) break; // QUIT
        }
        cleanup();
        log_msg(client_ip_, "disconnected");
    }

private:
    socket_t ctrl_;
    std::string client_ip_;
    ServerConfig cfg_;

    std::string username_;
    bool authenticated_ = false;
    std::string cwd_ = "/";       // FTP 视角当前目录
    char type_ = 'I';             // I=binary, A=ascii
    socket_t pasv_listen_ = INVALID_SOCK;
    uint16_t pasv_port_ = 0;
    std::string active_ip_;
    uint16_t active_port_ = 0;
    bool has_active_ = false;
    uint64_t rest_offset_ = 0;
    std::string rename_from_;     // RNFR 暂存(fs 路径)

    // ---------- 基础 ----------
    static void split_cmd(const std::string& line, std::string& cmd, std::string& arg) {
        size_t sp = line.find(' ');
        if (sp == std::string::npos) { cmd = to_upper(trim(line)); arg = ""; }
        else {
            cmd = to_upper(trim(line.substr(0, sp)));
            arg = trim(line.substr(sp + 1));
        }
    }
    static std::string mask_arg(const std::string& cmd, const std::string& arg) {
        if (cmd == "PASS") return "***";
        return arg;
    }

    void reply(int code, const std::string& msg) {
        std::string line = std::to_string(code) + " " + msg + "\r\n";
        send_all(ctrl_, line.c_str(), line.size());
    }
    void reply_multi(int code, const std::vector<std::string>& lines) {
        std::string out;
        for (size_t i = 0; i < lines.size(); ++i) {
            out += std::to_string(code);
            out += (i + 1 < lines.size()) ? "-" : " ";
            out += lines[i];
            out += "\r\n";
        }
        send_all(ctrl_, out.c_str(), out.size());
    }

    bool need_auth() {
        if (authenticated_) return true;
        reply(530, "Please login first.");
        return false;
    }
    bool need_write() {
        if (cfg_.allow_write) return true;
        reply(550, "Server is read-only.");
        return false;
    }

    void cleanup() {
        close_pasv();
        close_socket(ctrl_);
        ctrl_ = INVALID_SOCK;
    }
    void close_pasv() {
        if (pasv_listen_ != INVALID_SOCK) { close_socket(pasv_listen_); pasv_listen_ = INVALID_SOCK; }
        pasv_port_ = 0;
    }
    void reset_data_addr() {
        close_pasv();
        has_active_ = false;
    }

    // ---------- 路径 ----------
    // FTP 参数 -> 归一化 FTP 路径(以 / 开头)
    std::string to_ftp_path(const std::string& arg) {
        if (arg.empty()) return cwd_;
        return join_ftp(cwd_, arg);
    }
    // FTP 路径 -> 本地文件系统绝对路径
    std::string to_fs(const std::string& ftp_path) {
        std::string rel = ftp_path;
        if (!rel.empty() && rel[0] == '/') rel.erase(rel.begin());
        fs::path p(cfg_.root);
        if (!rel.empty()) {
            // rel 里是 / 分隔,逐段拼,防止 Windows 把 / 当转义问题
            for (auto& seg : split(rel, '/')) {
                if (!seg.empty()) p /= seg;
            }
        }
        return p.string();
    }
    // 参数 -> fs 路径(一步到位)
    std::string arg_to_fs(const std::string& arg) {
        return to_fs(to_ftp_path(arg));
    }
    // 安全检查:fs 路径必须在 root 内(含 root 本身)
    bool inside_root(const std::string& fs_path) {
        try {
            fs::path r = fs::weakly_canonical(fs::path(cfg_.root));
            fs::path p = fs::weakly_canonical(fs::path(fs_path));
            auto ri = r.begin(), pi = p.begin();
            for (; ri != r.end(); ++ri, ++pi) {
                if (pi == p.end() || *pi != *ri) return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    // ---------- 数据连接 ----------
    std::string pasv_advertise_ip() {
        if (!cfg_.pasv_ip.empty()) return cfg_.pasv_ip;
        // 优先用控制连接本地端 IP(客户端能连上这个 IP,数据连接大概率也行)
        std::string lip = socket_local_ip(ctrl_);
        if (!lip.empty() && lip != "0.0.0.0") return lip;
        // 退化:取第一块网卡 IP
        auto ips = local_lan_ips();
        if (!ips.empty()) return ips[0];
        return "127.0.0.1";
    }

    uint16_t pick_pasv_port() {
        if (cfg_.pasv_min == 0 || cfg_.pasv_max <= cfg_.pasv_min) return 0; // 临时端口
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> d(cfg_.pasv_min, cfg_.pasv_max);
        return (uint16_t)d(rng);
    }

    bool enter_pasv(bool extended, std::string& resp) {
        reset_data_addr();
        uint16_t real = 0;
        socket_t ls = INVALID_SOCK;
        // 端口范围内重试几次
        for (int i = 0; i < 10; ++i) {
            uint16_t want = pick_pasv_port();
            ls = create_listen_socket(want, &real);
            if (ls != INVALID_SOCK) break;
            if (cfg_.pasv_min == 0) break;
        }
        if (ls == INVALID_SOCK) return false;
        pasv_listen_ = ls;
        pasv_port_ = real;
        if (extended) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Entering Extended Passive Mode (|||%u|)", (unsigned)real);
            resp = buf;
        } else {
            std::string ip = pasv_advertise_ip();
            int q[4] = {127, 0, 0, 1};
            parse_ipv4(ip, q);
            char buf[128];
            snprintf(buf, sizeof(buf), "Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
                     q[0], q[1], q[2], q[3], (real >> 8) & 0xFF, real & 0xFF);
            resp = buf;
        }
        return true;
    }

    // 打开数据连接:被动 accept / 主动 connect。失败返回 INVALID_SOCK(已回复 425)。
    socket_t open_data() {
        socket_t d = INVALID_SOCK;
        if (pasv_listen_ != INVALID_SOCK) {
            bool to = false;
            std::string peer;
            d = accept_with_timeout(pasv_listen_, 30000, &peer, &to);
            close_pasv(); // 一次性
            if (d == INVALID_SOCK) {
                reply(425, to ? "Data connection timed out." : "Cannot open data connection.");
                return INVALID_SOCK;
            }
        } else if (has_active_) {
            d = tcp_connect(active_ip_, active_port_, 10000);
            has_active_ = false;
            if (d == INVALID_SOCK) {
                reply(425, "Cannot connect to " + active_ip_ + ":" + std::to_string(active_port_) + ".");
                return INVALID_SOCK;
            }
        } else {
            reply(425, "Use PASV or PORT first.");
            return INVALID_SOCK;
        }
        return d;
    }

    // ---------- 命令分发 ----------
    bool dispatch(const std::string& cmd, const std::string& arg) {
        if (cmd == "USER") return cmd_user(arg);
        if (cmd == "PASS") return cmd_pass(arg);
        if (cmd == "QUIT") { reply(221, "Bye."); return false; }
        if (cmd == "SYST") { reply(215, "UNIX Type: L8"); return true; }
        if (cmd == "FEAT") return cmd_feat();
        if (cmd == "OPTS") return cmd_opts(arg);
        if (cmd == "NOOP") { reply(200, "OK"); return true; }
        if (cmd == "HELP") return cmd_help();
        if (cmd == "TYPE") return cmd_type(arg);
        if (cmd == "STRU") {
            if (to_upper(arg) == "R" || arg.empty()) { reply(200, "Structure R ok."); }
            else reply(504, "Only structure R supported.");
            return true;
        }
        if (cmd == "MODE") {
            if (to_upper(arg) == "S" || arg.empty()) { reply(200, "Mode S ok."); }
            else reply(504, "Only mode S supported.");
            return true;
        }
        if (cmd == "PWD" || cmd == "XPWD") {
            if (!need_auth()) return true;
            reply(257, "\"" + cwd_ + "\" is current directory.");
            return true;
        }
        if (cmd == "CWD" || cmd == "XCWD") return cmd_cwd(arg);
        if (cmd == "CDUP" || cmd == "XCUP") return cmd_cwd("..");
        if (cmd == "MKD" || cmd == "XMKD") return cmd_mkd(arg);
        if (cmd == "RMD" || cmd == "XRMD") return cmd_rmd(arg);
        if (cmd == "DELE") return cmd_dele(arg);
        if (cmd == "RNFR") return cmd_rnfr(arg);
        if (cmd == "RNTO") return cmd_rnto(arg);
        if (cmd == "SIZE") return cmd_size(arg);
        if (cmd == "MDTM") return cmd_mdtm(arg);
        if (cmd == "PASV") return cmd_pasv(false);
        if (cmd == "EPSV") return cmd_pasv(true);
        if (cmd == "PORT") return cmd_port(arg);
        if (cmd == "EPRT") return cmd_eprt(arg);
        if (cmd == "LIST") return cmd_list(arg, false);
        if (cmd == "NLST") return cmd_list(arg, true);
        if (cmd == "MLSD") return cmd_mlsd(arg);
        if (cmd == "MLST") return cmd_mlst(arg);
        if (cmd == "STAT") return cmd_stat(arg);
        if (cmd == "RETR") return cmd_retr(arg);
        if (cmd == "STOR") return cmd_stor(arg, false);
        if (cmd == "APPE") return cmd_stor(arg, true);
        if (cmd == "REST") return cmd_rest(arg);
        if (cmd == "ABOR") { reset_data_addr(); rest_offset_ = 0; reply(226, "Aborted."); return true; }
        if (cmd == "REIN") {
            authenticated_ = false; username_.clear(); cwd_ = "/";
            reset_data_addr(); rest_offset_ = 0;
            reply(220, "Ready for new user."); return true;
        }
        reply(502, "Command '" + cmd + "' not implemented.");
        return true;
    }

    // ---------- 登录 ----------
    bool cmd_user(const std::string& arg) {
        if (arg.empty()) { reply(501, "Missing username."); return true; }
        username_ = arg;
        authenticated_ = false;
        // 匿名且允许匿名 -> 提示输密码(任意)
        reply(331, "Username ok, need password.");
        return true;
    }
    bool cmd_pass(const std::string& arg) {
        if (username_.empty()) { reply(503, "Send USER first."); return true; }
        std::string u = to_upper(username_);
        bool is_anon = (u == "ANONYMOUS" || u == "FTP");
        bool ok = false;
        if (is_anon && cfg_.allow_anonymous && cfg_.user.empty()) {
            ok = true; // 纯匿名模式:任何密码都行
        } else if (!cfg_.user.empty()) {
            ok = (username_ == cfg_.user && arg == cfg_.pass);
        } else if (cfg_.allow_anonymous) {
            ok = true; // 默认零配置:任何用户都放行
        }
        if (ok) {
            authenticated_ = true;
            cwd_ = "/";
            reply(230, "Login successful. Welcome to MiniFTP.");
            log_msg(client_ip_, "login ok as '" + username_ + "'");
        } else {
            reply(530, "Login incorrect.");
            log_msg(client_ip_, "login FAILED as '" + username_ + "'");
        }
        return true;
    }

    // ---------- 能力 ----------
    bool cmd_feat() {
        reply_multi(211, {"Features:", " UTF8", " PASV", " EPSV", " EPRT",
                          " MDTM", " SIZE", " REST STREAM", " MLST type*;size*;modify*;",
                          " TVFS", "End"});
        return true;
    }
    bool cmd_opts(const std::string& arg) {
        std::string a = to_upper(arg);
        if (a == "UTF8 ON" || a == "UTF-8 ON") { reply(200, "UTF8 enabled."); return true; }
        reply(502, "Option not supported.");
        return true;
    }
    bool cmd_help() {
        reply_multi(214, {"MiniFTP commands:",
                          " USER PASS QUIT SYST FEAT OPTS NOOP HELP",
                          " TYPE STRU MODE PWD CWD CDUP MKD RMD DELE RNFR RNTO",
                          " SIZE MDTM PASV EPSV PORT EPRT LIST NLST MLSD MLST STAT",
                          " RETR STOR APPE REST ABOR",
                          "End"});
        return true;
    }
    bool cmd_type(const std::string& arg) {
        std::string a = to_upper(arg);
        if (a == "I" || a == "L 8") { type_ = 'I'; reply(200, "Type set to I (binary)."); }
        else if (a == "A" || a == "A N") { type_ = 'A'; reply(200, "Type set to A."); }
        else reply(504, "Only TYPE I and TYPE A supported.");
        return true;
    }

    // ---------- 目录 ----------
    bool cmd_cwd(const std::string& arg) {
        if (!need_auth()) return true;
        if (arg.empty()) { reply(501, "Missing path."); return true; }
        std::string np = to_ftp_path(arg);
        std::string fsp = to_fs(np);
        if (!inside_root(fsp)) { reply(550, "Access denied."); return true; }
        std::error_code ec;
        if (!fs::is_directory(fs::path(fsp), ec) || ec) { reply(550, "Directory not found."); return true; }
        cwd_ = np;
        reply(250, "CWD ok. \"" + cwd_ + "\"");
        return true;
    }
    bool cmd_mkd(const std::string& arg) {
        if (!need_auth() || !need_write()) return true;
        if (arg.empty()) { reply(501, "Missing path."); return true; }
        std::string fsp = arg_to_fs(arg);
        if (!inside_root(fsp)) { reply(550, "Access denied."); return true; }
        std::error_code ec;
        if (fs::create_directories(fs::path(fsp), ec) && !ec) {
            reply(257, "\"" + to_ftp_path(arg) + "\" created.");
        } else {
            reply(550, "Cannot create directory.");
        }
        return true;
    }
    bool cmd_rmd(const std::string& arg) {
        if (!need_auth() || !need_write()) return true;
        if (arg.empty()) { reply(501, "Missing path."); return true; }
        std::string fsp = arg_to_fs(arg);
        if (!inside_root(fsp) || fsp == cfg_.root) { reply(550, "Access denied."); return true; }
        std::error_code ec;
        auto n = fs::remove_all(fs::path(fsp), ec);
        if (!ec && n > 0) reply(250, "Directory removed.");
        else reply(550, "Cannot remove directory.");
        return true;
    }
    bool cmd_dele(const std::string& arg) {
        if (!need_auth() || !need_write()) return true;
        if (arg.empty()) { reply(501, "Missing path."); return true; }
        std::string fsp = arg_to_fs(arg);
        if (!inside_root(fsp)) { reply(550, "Access denied."); return true; }
        std::error_code ec;
        if (fs::remove(fs::path(fsp), ec) && !ec) reply(250, "File deleted.");
        else reply(550, "Cannot delete file.");
        return true;
    }
    bool cmd_rnfr(const std::string& arg) {
        if (!need_auth() || !need_write()) return true;
        if (arg.empty()) { reply(501, "Missing path."); return true; }
        std::string fsp = arg_to_fs(arg);
        if (!inside_root(fsp)) { reply(550, "Access denied."); return true; }
        std::error_code ec;
        if (!fs::exists(fs::path(fsp), ec) || ec) { reply(550, "File not found."); return true; }
        rename_from_ = fsp;
        reply(350, "Ready for RNTO.");
        return true;
    }
    bool cmd_rnto(const std::string& arg) {
        if (!need_auth() || !need_write()) return true;
        if (rename_from_.empty()) { reply(503, "Send RNFR first."); return true; }
        if (arg.empty()) { reply(501, "Missing path."); return true; }
        std::string fsp = arg_to_fs(arg);
        if (!inside_root(fsp)) { rename_from_.clear(); reply(550, "Access denied."); return true; }
        std::error_code ec;
        fs::rename(fs::path(rename_from_), fs::path(fsp), ec);
        rename_from_.clear();
        if (!ec) reply(250, "Renamed ok.");
        else reply(550, "Cannot rename.");
        return true;
    }
    bool cmd_size(const std::string& arg) {
        if (!need_auth()) return true;
        if (arg.empty()) { reply(501, "Missing path."); return true; }
        std::string fsp = arg_to_fs(arg);
        if (!inside_root(fsp)) { reply(550, "Access denied."); return true; }
        std::error_code ec;
        if (fs::is_directory(fs::path(fsp), ec) || !fs::exists(fs::path(fsp), ec)) {
            reply(550, "Not a regular file."); return true;
        }
        reply(213, std::to_string(file_size_safe(fsp)));
        return true;
    }
    bool cmd_mdtm(const std::string& arg) {
        if (!need_auth()) return true;
        if (arg.empty()) { reply(501, "Missing path."); return true; }
        std::string fsp = arg_to_fs(arg);
        if (!inside_root(fsp)) { reply(550, "Access denied."); return true; }
        std::error_code ec;
        if (!fs::exists(fs::path(fsp), ec)) { reply(550, "File not found."); return true; }
        reply(213, file_time_utc(fsp));
        return true;
    }

    // ---------- 数据地址 ----------
    bool cmd_pasv(bool extended) {
        if (!need_auth()) return true;
        std::string resp;
        if (!enter_pasv(extended, resp)) {
            reply(425, "Cannot enter passive mode: " + last_net_error());
            return true;
        }
        reply(extended ? 229 : 227, resp);
        return true;
    }
    bool cmd_port(const std::string& arg) {
        if (!need_auth()) return true;
        auto parts = split(arg, ',');
        if (parts.size() != 6) { reply(501, "Bad PORT format. Use h1,h2,h3,h4,p1,p2"); return true; }
        try {
            int h[4], p1 = std::stoi(parts[4]), p2 = std::stoi(parts[5]);
            for (int i = 0; i < 4; ++i) h[i] = std::stoi(parts[i]);
            for (int i = 0; i < 4; ++i) if (h[i] < 0 || h[i] > 255) throw 0;
            if (p1 < 0 || p1 > 255 || p2 < 0 || p2 > 255) throw 0;
            reset_data_addr();
            char ip[32]; snprintf(ip, sizeof(ip), "%d.%d.%d.%d", h[0], h[1], h[2], h[3]);
            active_ip_ = ip;
            active_port_ = (uint16_t)(p1 * 256 + p2);
            has_active_ = true;
            reply(200, "PORT ok.");
        } catch (...) {
            reply(501, "Bad PORT numbers.");
        }
        return true;
    }
    bool cmd_eprt(const std::string& arg) {
        if (!need_auth()) return true;
        // |1|192.168.1.1|5000|
        std::string t = trim(arg);
        if (t.size() < 7 || t.front() != '|') { reply(501, "Bad EPRT format."); return true; }
        char delim = '|';
        auto p = split(t.substr(1), delim);
        if (p.size() < 3) { reply(501, "Bad EPRT format."); return true; }
        if (p[0] != "1") { reply(522, "Only IPv4 supported."); return true; }
        int q[4];
        if (!parse_ipv4(p[1], q)) { reply(501, "Bad IP."); return true; }
        try {
            int port = std::stoi(p[2]);
            if (port <= 0 || port > 65535) throw 0;
            reset_data_addr();
            active_ip_ = p[1];
            active_port_ = (uint16_t)port;
            has_active_ = true;
            reply(200, "EPRT ok.");
        } catch (...) {
            reply(501, "Bad port.");
        }
        return true;
    }

    // ---------- 列表 ----------
    std::string list_line(const fs::directory_entry& e) {
        std::string name = e.path().filename().string();
        std::string fsp = e.path().string();
        std::error_code ec;
        bool is_dir = e.is_directory(ec);
        uint64_t sz = is_dir ? 0 : file_size_safe(fsp);
        char buf[512];
        snprintf(buf, sizeof(buf), "%s 1 owner group %10llu %s %s\r\n",
                 is_dir ? "drwxr-xr-x" : "-rw-r--r--",
                 (unsigned long long)sz,
                 file_time_list(fsp).c_str(), name.c_str());
        return std::string(buf);
    }
    std::string mlsd_line(const fs::directory_entry& e) {
        std::string name = e.path().filename().string();
        std::string fsp = e.path().string();
        std::error_code ec;
        bool is_dir = e.is_directory(ec);
        char buf[1024];
        if (is_dir) {
            snprintf(buf, sizeof(buf), "type=dir;modify=%s; %s\r\n",
                     file_time_utc(fsp).c_str(), name.c_str());
        } else {
            snprintf(buf, sizeof(buf), "type=file;size=%llu;modify=%s; %s\r\n",
                     (unsigned long long)file_size_safe(fsp),
                     file_time_utc(fsp).c_str(), name.c_str());
        }
        return std::string(buf);
    }

    // LIST/MLSD 的目标目录解析:支持 "LIST", "LIST /dir", "LIST -l"(忽略选项)
    bool resolve_list_dir(const std::string& arg, std::string& out_fsdir) {
        std::string a = trim(arg);
        // 去掉 "-l / -a" 之类选项
        while (!a.empty() && a[0] == '-') {
            size_t sp = a.find(' ');
            if (sp == std::string::npos) { a.clear(); break; }
            a = trim(a.substr(sp + 1));
        }
        std::string fsp = a.empty() ? to_fs(cwd_) : arg_to_fs(a);
        if (!inside_root(fsp)) return false;
        std::error_code ec;
        if (!fs::exists(fs::path(fsp), ec)) return false;
        out_fsdir = fsp;
        return true;
    }

    bool cmd_list(const std::string& arg, bool names_only) {
        if (!need_auth()) return true;
        std::string target;
        if (!resolve_list_dir(arg, target)) { reply(550, "Path not found."); return true; }
        socket_t d = open_data();
        if (d == INVALID_SOCK) return true;
        reply(150, "Opening data connection for listing.");

        std::ostringstream oss;
        try {
            std::error_code ec;
            if (fs::is_directory(fs::path(target), ec)) {
                std::vector<fs::directory_entry> items;
                for (auto& e : fs::directory_iterator(fs::path(target), ec)) items.push_back(e);
                std::sort(items.begin(), items.end(), [](auto& a, auto& b) {
                    return a.path().filename().string() < b.path().filename().string();
                });
                for (auto& e : items) {
                    if (names_only) oss << e.path().filename().string() << "\r\n";
                    else oss << list_line(e);
                }
            } else {
                fs::directory_entry e{fs::path(target)};
                if (names_only) oss << e.path().filename().string() << "\r\n";
                else oss << list_line(e);
            }
        } catch (...) { /* 空目录也算成功 */ }
        std::string data = oss.str();
        bool ok = data.empty() ? true : send_all(d, data.c_str(), data.size());
        close_socket(d);
        reply(ok ? 226 : 426, ok ? "Listing done." : "Data connection broken.");
        return true;
    }

    bool cmd_mlsd(const std::string& arg) {
        if (!need_auth()) return true;
        std::string target;
        if (!resolve_list_dir(arg, target)) { reply(550, "Path not found."); return true; }
        socket_t d = open_data();
        if (d == INVALID_SOCK) return true;
        reply(150, "Opening data connection for MLSD.");
        std::ostringstream oss;
        try {
            std::error_code ec;
            if (fs::is_directory(fs::path(target), ec)) {
                for (auto& e : fs::directory_iterator(fs::path(target), ec)) oss << mlsd_line(e);
            } else {
                oss << mlsd_line(fs::directory_entry(fs::path(target)));
            }
        } catch (...) {}
        std::string data = oss.str();
        bool ok = data.empty() ? true : send_all(d, data.c_str(), data.size());
        close_socket(d);
        reply(ok ? 226 : 426, ok ? "MLSD done." : "Data connection broken.");
        return true;
    }

    bool cmd_mlst(const std::string& arg) {
        if (!need_auth()) return true;
        std::string fsp = arg.empty() ? to_fs(cwd_) : arg_to_fs(trim(arg));
        if (!inside_root(fsp)) { reply(550, "Access denied."); return true; }
        std::error_code ec;
        if (!fs::exists(fs::path(fsp), ec)) { reply(550, "Path not found."); return true; }
        fs::directory_entry e{fs::path(fsp)};
        std::string line = mlsd_line(e);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        reply_multi(250, {"Listing " + (arg.empty() ? cwd_ : trim(arg)), " " + line, "End"});
        return true;
    }

    bool cmd_stat(const std::string& arg) {
        if (!need_auth()) return true;
        std::string target = arg.empty() ? to_fs(cwd_) : arg_to_fs(trim(arg));
        if (!inside_root(target)) { reply(550, "Access denied."); return true; }
        std::vector<std::string> lines{"Status:"};
        try {
            std::error_code ec;
            if (fs::is_directory(fs::path(target), ec)) {
                for (auto& e : fs::directory_iterator(fs::path(target), ec)) {
                    std::string l = list_line(e);
                    while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
                    lines.push_back(" " + l);
                }
            } else {
                std::string l = list_line(fs::directory_entry(fs::path(target)));
                while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
                lines.push_back(" " + l);
            }
        } catch (...) {}
        lines.push_back("End");
        reply_multi(211, lines);
        return true;
    }

    // ---------- 传文件 ----------
    bool cmd_rest(const std::string& arg) {
        if (!need_auth()) return true;
        try {
            long long v = std::stoll(trim(arg));
            if (v < 0) throw 0;
            rest_offset_ = (uint64_t)v;
            reply(350, "Restarting at " + std::to_string(v) + ".");
        } catch (...) {
            reply(501, "Bad offset.");
        }
        return true;
    }

    bool cmd_retr(const std::string& arg) {
        if (!need_auth()) return true;
        if (arg.empty()) { reply(501, "Missing path."); return true; }
        std::string fsp = arg_to_fs(arg);
        if (!inside_root(fsp)) { reply(550, "Access denied."); return true; }
        std::error_code ec;
        if (!fs::exists(fs::path(fsp), ec) || fs::is_directory(fs::path(fsp), ec)) {
            reply(550, "File not found."); return true;
        }
        std::ifstream f(fsp, std::ios::binary);
        if (!f) { reply(550, "Cannot open file."); return true; }
        uint64_t total = file_size_safe(fsp);
        if (rest_offset_ > total) { reply(550, "Offset beyond file size."); rest_offset_ = 0; return true; }
        if (rest_offset_ > 0) f.seekg((std::streamoff)rest_offset_);

        socket_t d = open_data();
        if (d == INVALID_SOCK) { rest_offset_ = 0; return true; }
        reply(150, "Opening data connection for " + fs::path(fsp).filename().string() +
                   " (" + std::to_string(total - rest_offset_) + " bytes).");

        char buf[65536];
        uint64_t sent = 0;
        bool ok = true;
        auto t0 = std::chrono::steady_clock::now();
        while (f) {
            f.read(buf, sizeof(buf));
            std::streamsize n = f.gcount();
            if (n <= 0) break;
            if (!send_all(d, buf, (size_t)n)) { ok = false; break; }
            sent += (uint64_t)n;
        }
        close_socket(d);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        double speed = ms > 0 ? (sent / 1024.0 / 1024.0) / (ms / 1000.0) : 0;
        char tail[128];
        snprintf(tail, sizeof(tail), " %llu bytes, %.1f MB/s", (unsigned long long)sent, speed);
        log_msg(client_ip_, std::string("RETR ") + to_ftp_path(arg) + tail);
        reply(ok ? 226 : 426, ok ? ("Transfer done." + std::string(tail)) : "Data connection broken.");
        rest_offset_ = 0;
        return true;
    }

    bool cmd_stor(const std::string& arg, bool append) {
        if (!need_auth() || !need_write()) return true;
        if (arg.empty()) { reply(501, "Missing path."); return true; }
        std::string fsp = arg_to_fs(arg);
        if (!inside_root(fsp)) { reply(550, "Access denied."); return true; }
        // 父目录必须存在(客户端应先 MKD;这里自动建父目录更省心)
        try {
            fs::path parent = fs::path(fsp).parent_path();
            if (!parent.empty()) { std::error_code ec; fs::create_directories(parent, ec); }
        } catch (...) {}

        socket_t d = open_data();
        if (d == INVALID_SOCK) { rest_offset_ = 0; return true; }

        std::ofstream f;
        if (append) f.open(fsp, std::ios::binary | std::ios::app);
        else if (rest_offset_ > 0) {
            // 断点续传:先以读写打开并 seek,文件不存在则新建
            f.open(fsp, std::ios::binary | std::ios::in | std::ios::out);
            if (!f) f.open(fsp, std::ios::binary | std::ios::out);
            else f.seekp((std::streamoff)rest_offset_);
            if (!f) { close_socket(d); rest_offset_ = 0; reply(550, "Cannot open file."); return true; }
        } else {
            f.open(fsp, std::ios::binary | std::ios::trunc);
        }
        if (!f) { close_socket(d); rest_offset_ = 0; reply(550, "Cannot open file."); return true; }

        reply(150, "Opening data connection for upload.");
        char buf[65536];
        uint64_t got = 0;
        auto t0 = std::chrono::steady_clock::now();
        while (true) {
#ifdef _WIN32
            int n = ::recv(d, buf, sizeof(buf), 0);
            if (n <= 0) break;
#else
            ssize_t n = ::recv(d, buf, sizeof(buf), 0);
            if (n <= 0) break;
#endif
            f.write(buf, n);
            if (!f) break;
            got += (uint64_t)n;
        }
        f.close();
        close_socket(d);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        double speed = ms > 0 ? (got / 1024.0 / 1024.0) / (ms / 1000.0) : 0;
        char tail[128];
        snprintf(tail, sizeof(tail), " %llu bytes, %.1f MB/s", (unsigned long long)got, speed);
        log_msg(client_ip_, std::string(append ? "APPE " : "STOR ") + to_ftp_path(arg) + tail);
        reply(226, "Upload done." + std::string(tail));
        rest_offset_ = 0;
        return true;
    }
};

} // namespace

void handle_ftp_session(socket_t ctrl, std::string client_ip,
                        const ServerConfig& cfg,
                        std::atomic<bool>& server_running) {
    (void)server_running;
    Session s(ctrl, std::move(client_ip), cfg);
    s.run();
}

} // namespace miniftp
