#include "utils.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace miniftp {
namespace fs = std::filesystem;

static std::mutex g_log_mtx;

void log_msg(const std::string& tag, const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::cout << "[" << ts << "][" << tag << "] " << msg << std::endl;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string to_upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

std::string normalize_ftp_path(const std::string& p) {
    // 统一分隔符
    std::string t = p;
    for (auto& c : t) if (c == '\\') c = '/';
    bool abs = !t.empty() && t[0] == '/';
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= t.size(); ++i) {
        char c = i < t.size() ? t[i] : '/';
        if (c == '/') {
            if (cur.empty() || cur == ".") { /*skip*/ }
            else if (cur == "..") { if (!parts.empty()) parts.pop_back(); }
            else parts.push_back(cur);
            cur.clear();
        } else cur.push_back(c);
    }
    std::string r = abs ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) r += "/";
        r += parts[i];
    }
    if (r.empty()) r = "/";
    return r;
}

static std::time_t fs_mtime(const std::string& fs_path) {
    try {
        auto ftime = fs::last_write_time(fs::path(fs_path));
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        return std::chrono::system_clock::to_time_t(sctp);
    } catch (...) {
        return std::time(nullptr);
    }
}

std::string file_time_utc(const std::string& fs_path) {
    std::time_t t = fs_mtime(fs_path);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tmv);
    return std::string(buf);
}

std::string file_time_list(const std::string& fs_path) {
    std::time_t t = fs_mtime(fs_path);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    static const char* mon[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
    char buf[32];
    std::time_t now = std::time(nullptr);
    double diff = difftime(now, t);
    if (diff < 0) diff = 0;
    // 6 个月内显示时间,否则显示年份(仿 ls -l)
    if (diff < 182.0 * 24 * 3600) {
        snprintf(buf, sizeof(buf), "%s %2d %02d:%02d",
                 mon[tmv.tm_mon % 12], tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    } else {
        snprintf(buf, sizeof(buf), "%s %2d  %04d",
                 mon[tmv.tm_mon % 12], tmv.tm_mday, tmv.tm_year + 1900);
    }
    return std::string(buf);
}

uint64_t file_size_safe(const std::string& fs_path) {
    try {
        if (fs::is_directory(fs::path(fs_path))) return 0;
        return (uint64_t)fs::file_size(fs::path(fs_path));
    } catch (...) {
        return 0;
    }
}

std::string join_ftp(const std::string& a, const std::string& b) {
    if (b.empty()) return a.empty() ? "/" : a;
    if (!b.empty() && b[0] == '/') return normalize_ftp_path(b);
    std::string t = a;
    if (t.empty()) t = "/";
    if (t.back() != '/') t += "/";
    t += b;
    return normalize_ftp_path(t);
}

} // namespace miniftp
