// MiniFTP - 零配置跨平台 FTP 服务器
// 双击即跑:默认端口 2121,共享 ./data,匿名可读写。
// 用法见 --help 与 README.md。

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#ifdef _WIN32
  #include <windows.h>
#endif

#include "ftp_server.h"
#include "net.h"
#include "utils.h"

namespace fs = std::filesystem;
using miniftp::FtpServer;
using miniftp::NetInit;
using miniftp::ServerConfig;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

static void print_help(const char* prog) {
    std::cout <<
        "MiniFTP v1.0 - 零配置跨平台 FTP 服务器\n"
        "\n用法:\n"
        "  " << prog << " [选项]          双击直接运行,全部默认即可\n"
        "\n选项:\n"
        "  --port N        控制端口,默认 2121(21 需管理员权限,故默认 2121)\n"
        "  --root DIR      共享目录,默认 ./data(不存在自动创建)\n"
        "  --user NAME     指定用户名(设置后关闭匿名,必须用该用户登录)\n"
        "  --pass PWD      指定密码(默认空)\n"
        "  --read-only     只读模式(禁止上传/删除/新建)\n"
        "  --pasv-ip IP    被动模式宣告的 IP(云服务器/NAT 后必填公网 IP)\n"
        "  --pasv-port A-B 被动端口范围,如 50000-50100(方便防火墙放行)\n"
        "  --help          显示本帮助\n"
        "\n示例:\n"
        "  " << prog << "                              零配置启动\n"
        "  " << prog << " --port 21 --root /srv/ftp     标准端口 + 指定目录\n"
        "  " << prog << " --user boss --pass 123456     需要账号密码\n"
        "  " << prog << " --read-only                   只允许下载\n"
        "\n客户端连接(任选其一):\n"
        "  1) 文件资源管理器地址栏输入 ftp://本机IP:2121\n"
        "  2) FileZilla: 主机=本机IP 端口=2121 用户=anonymous 密码任意\n"
        "  3) 命令行: ftp 本机IP 2121\n";
}

static bool parse_pasv_range(const std::string& s, uint16_t& a, uint16_t& b) {
    size_t dash = s.find('-');
    if (dash == std::string::npos) return false;
    try {
        int x = std::stoi(s.substr(0, dash)), y = std::stoi(s.substr(dash + 1));
        if (x <= 0 || y <= 0 || x > 65535 || y > 65535 || x > y) return false;
        a = (uint16_t)x; b = (uint16_t)y;
        return true;
    } catch (...) { return false; }
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Windows 控制台 UTF-8(文件名中文不乱码)
    ::SetConsoleOutputCP(65001);
    ::SetConsoleCP(65001);
#endif

    ServerConfig cfg;
    cfg.root = ""; // 稍后默认 ./data

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_val = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << name << " 缺少参数值\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h" || a == "/?") { print_help(argv[0]); return 0; }
        else if (a == "--port") {
            cfg.port = (uint16_t)std::stoi(need_val("--port"));
            if (cfg.port == 0) { std::cerr << "端口无效\n"; return 2; }
        }
        else if (a == "--root") cfg.root = need_val("--root");
        else if (a == "--user") cfg.user = need_val("--user");
        else if (a == "--pass") cfg.pass = need_val("--pass");
        else if (a == "--read-only") cfg.allow_write = false;
        else if (a == "--pasv-ip") cfg.pasv_ip = need_val("--pasv-ip");
        else if (a == "--pasv-port") {
            if (!parse_pasv_range(need_val("--pasv-port"), cfg.pasv_min, cfg.pasv_max)) {
                std::cerr << "被动端口范围格式错误,示例: --pasv-port 50000-50100\n";
                return 2;
            }
        }
        else { std::cerr << "未知选项: " << a << " (用 --help 查看)\n"; return 2; }
    }

    NetInit net;
    if (!net.ok()) { std::cerr << "网络初始化失败: " << net.error() << "\n"; return 1; }

    // 默认共享目录 ./data
    if (cfg.root.empty()) cfg.root = "data";
    std::error_code ec;
    fs::create_directories(fs::path(cfg.root), ec);
    if (ec) { std::cerr << "创建共享目录失败: " << cfg.root << " " << ec.message() << "\n"; return 1; }
    cfg.root = fs::absolute(fs::path(cfg.root), ec).string();
    if (ec) { std::cerr << "解析共享目录失败\n"; return 1; }

    if (!cfg.user.empty()) cfg.allow_anonymous = false;

    // 放一个说明文件,方便确认共享目录位置
    {
        fs::path how = fs::path(cfg.root) / "MiniFTP共享目录说明.txt";
        if (!fs::exists(how, ec)) {
            std::ofstream f(how);
            if (f) {
                f << "这里是 MiniFTP 的共享目录。\n"
                  << "把要分享的文件放到这里,对方用 FTP 客户端连接即可下载。\n"
                  << "对方上传的文件也会出现在这里。\n"
                  << "本文件可安全删除。\n";
            }
        }
    }

    std::signal(SIGINT, on_signal);
#ifdef SIGTERM
    std::signal(SIGTERM, on_signal);
#endif

    FtpServer server(cfg);
    std::string err;
    if (!server.start(err)) {
        std::cerr << "启动失败: " << err << "\n";
#ifdef _WIN32
        std::cerr << "按回车退出...\n"; std::cin.get();
#endif
        return 1;
    }

    // ---------- 打印“点击即用”的连接信息 ----------
    auto ips = miniftp::local_lan_ips();
    std::cout << "\n"
              << "  __  __ _       _ _____ _____ ____  \n"
              << " |  \\/  (_)_ __ (_)  ___|_   _|  _ \\ \n"
              << " | |\\/| | | '_ \\| | |_    | | | |_) |\n"
              << " | |  | | | | | | |  _|   | | |  __/ \n"
              << " |_|  |_|_|_| |_|_|_|     |_| |_|    \n"
              << "\n  MiniFTP 已启动(FTP Server,兼容 FileZilla/资源管理器)\n"
              << "  ------------------------------------------------\n"
              << "  共享目录: " << cfg.root << "\n"
              << "  端口    : " << cfg.port
              << (cfg.port == 21 ? "" : " (标准 FTP 是 21,本机测试用 2121 免管理员权限)") << "\n"
              << "  账号    : " << (cfg.user.empty() ? "anonymous (任意密码,开箱即用)"
                                                    : cfg.user + " (已设密码保护)") << "\n"
              << "  权限    : " << (cfg.allow_write ? "可上传/下载" : "只读(仅下载)") << "\n"
              << "  ------------------------------------------------\n";
    if (ips.empty()) {
        std::cout << "  本机访问: ftp://127.0.0.1:" << cfg.port << "/\n";
        std::cout << "  (未检测到局域网 IP,只有本机能连)\n";
    } else {
        std::cout << "  在对方电脑用下面任一地址连接:\n";
        for (auto& ip : ips)
            std::cout << "    ftp://" << ip << ":" << cfg.port << "/\n";
        std::cout << "  本机测试: ftp://127.0.0.1:" << cfg.port << "/\n";
    }
    std::cout << "  ------------------------------------------------\n"
              << "  连接方式: 资源管理器地址栏粘贴上面地址回车,\n"
              << "            或 FileZilla 主机填 IP、端口填 " << cfg.port << "。\n"
              << "  传文件  : 直接拖拽复制粘贴,和本地文件夹一样。\n"
              << "  停止服务: 按 Ctrl+C,或关闭本窗口。\n"
              << "  ------------------------------------------------\n\n";

    // 等待退出信号
    while (!g_stop && server.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "\n正在停止服务...\n";
    server.stop();
    std::cout << "已退出。\n";
    return 0;
}
