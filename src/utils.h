#pragma once
// MiniFTP - 小工具:日志 / 字符串 / 时间

#include <cstdint>
#include <string>
#include <vector>

namespace miniftp {

void log_msg(const std::string& tag, const std::string& msg);

std::string trim(const std::string& s);
std::string to_upper(std::string s);
std::vector<std::string> split(const std::string& s, char delim);

// "/a//b/./c/../d" -> "/a/b/d"(纯词法归一,不碰文件系统)
std::string normalize_ftp_path(const std::string& p);

// 文件 mtime -> "20240101120000"(UTC,供 MDTM/MLSD 用)
std::string file_time_utc(const std::string& fs_path);

// 文件 mtime -> "Jan  2 15:04"(LIST 用)
std::string file_time_list(const std::string& fs_path);

uint64_t file_size_safe(const std::string& fs_path);

// 路径拼接(FTP 风格,分隔符 /)
std::string join_ftp(const std::string& a, const std::string& b);

} // namespace miniftp
