# MiniFTP 🗂️ — 零配置跨平台 FTP 服务器(C++)

[![Build & Release](https://github.com/wudream813/MiniFTP/actions/workflows/release.yml/badge.svg)](https://github.com/wudream813/MiniFTP/actions/workflows/release.yml)

> 💡 不想自己编译?直接去 [Releases](https://github.com/wudream813/MiniFTP/releases) 下载对应系统的可执行文件,双击即用。

一个`.cpp`直出的迷你 FTP **服务器**:**双击即跑,无需任何配置**,
局域网内另一台电脑打开文件管理器就能拖文件。

- ✅ 纯 C++17,零第三方依赖(只用标准库 + 系统 socket)
- ✅ 跨平台:Windows / macOS / Linux 同一份代码
- ✅ 标准 FTP 协议(RFC 959 子集),兼容 **文件资源管理器 / Finder / FileZilla / `ftp` 命令**
- ✅ 点击即传:启动后直接打印 `ftp://IP:端口`,粘贴即连,拖拽即传
- ✅ 断点续传(REST)、主动/被动模式、中文文件名、目录 conf 限制在共享目录内

## 1. 5 秒上手(以 Windows 为例)

1. 构建得到 `miniftp.exe`(见第 2 节),**双击运行**,看到类似输出:
   ```
   共享目录: D:\miniftp\data
   端口    : 2121
   在对方电脑用下面任一地址连接:
     ftp://192.168.1.10:2121/
   ```
2. 要收文件的电脑,打开**文件资源管理器**,地址栏粘贴 `ftp://192.168.1.10:2121/` 回车。
3. 直接**拖文件进去 = 上传,拖出来 = 下载**,和本地文件夹一样。✅
4. 传完关闭黑窗口即停止服务。

> macOS:Finder → 前往 → 连接服务器(`⌘K`)→ 输入地址。
> 更推荐装 [FileZilla](https://filezilla-project.org/):主机填 IP,端口填 2121,
> 用户名 `anonymous`,密码任意 → 快速连接。

## 2. 构建

| 平台 | 方法 |
|---|---|
| Windows | 双击 `build.bat`(需 VS2022 C++ 桌面开发,或 MinGW g++)→ 得 `build\bin\miniftp.exe` |
| Linux/macOS | `./build.sh`(有 cmake 用 cmake,否则用 g++ 直编)→ 得 `build/bin/miniftp` |
| 极简 | `make` → 得 `bin/miniftp`(Linux/macOS/MinGW) |

手动 CMake:
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j
./bin/miniftp
```

## 3. 命令行选项(全可选,不填就是零配置)

```
miniftp [选项]
  --port N          控制端口,默认 2121(21 需管理员权限)
  --root DIR        共享目录,默认 ./data(不存在自动创建)
  --user NAME       指定用户名(设置后关闭匿名登录)
  --pass PWD        指定密码(默认空)
  --read-only       只读模式(禁止上传/删除/新建)
  --pasv-ip IP      被动模式宣告 IP(云服务器/NAT 后填公网 IP)
  --pasv-port A-B   被动端口范围,如 50000-50100(方便防火墙放行)
  --help            帮助
```

示例:
```bash
miniftp                                # 零配置:匿名可读写 ./data
miniftp --user boss --pass 123456      # 需要账号密码
miniftp --root /srv/share --read-only  # 只允许下载
miniftp --port 21                      # 标准端口(需管理员/root)
```

## 4. 支持的 FTP 命令

登录/会话:`USER PASS QUIT REIN SYST FEAT OPTS NOOP HELP`
目录:`PWD CWD CDUP MKD RMD DELE RNFR RNTO STAT`
文件:`TYPE STRU MODE SIZE MDTM LIST NLST MLSD MLST RETR STOR APPE REST ABOR`
连接:`PASV EPSV PORT EPRT`(主动+被动全支持)

## 5. 目录结构

```
miniftp/
├── CMakeLists.txt      跨平台构建(MSVC/GCC/Clang)
├── Makefile            免 CMake 快捷构建
├── build.sh / build.bat 一键构建脚本
├── src/
│   ├── main.cpp        启动入口:参数解析/打印连接地址/信号退出
│   ├── net.h/.cpp      跨平台 socket 层(Winsock/POSIX 适配)
│   ├── utils.h/.cpp    日志/路径归一/时间格式
│   ├── ftp_session.h/.cpp  会话:命令分发/文件传输/目录操作
│   └── ftp_server.h/.cpp   监听 + 每连接一线程
├── test_client.py      自测脚本(python3,覆盖 16 项)
└── README.md
```

## 6. 自测(已在 Linux 实测 ALL PASS)

```bash
./build/bin/miniftp --port 2121 --root ./testdata &
python3 test_client.py   # 登录/上传/下载/中文名/5MB/续传/目录/主动模式...
```

覆盖:`NLST/STOR/RETR(5MB md5)/中文名/SIZE/MDTM/REST 续传/MKD/CWD/RNFR/RNTO/DELE/RMD/FEAT/SYST/MLSD/PORT`。

## 7. 常见问题

- **连不上?** 先 `ping` 对方 IP;Windows 首次运行会弹防火墙提示→ 点“允许”;
  公司网/虚拟机注意是否同网段。
- **能连上但列不出目录/传不了?** 99% 是数据连接问题:客户端切**被动模式(PASV)**;
  云服务器加 `--pasv-ip 公网IP --pasv-port 50000-50100` 并在安全组放行这些端口。
- **要用 21 端口?** `miniftp --port 21`,Windows 右键“以管理员运行”,Linux 加 `sudo`。
- **中文乱码?** 本服务按 UTF-8 收发(FileZilla 默认即 UTF-8);极老的 `ftp.exe` 可能显示 `?`,
  不影响传输内容。
- **安全吗?** FTP 明文传输,只适合**可信局域网**快传;公网请加 VPN 或改用 SFTP。
  目录已做 `..` 越狱防护,所有操作限制在 `--root` 内。

## 8. 后续可加(欢迎提需求)

- `--token 配对码` 显示 4 位码,客户端输码即连(更像 AirDrop)
- 局域网 UDP 广播自动发现
- 传输进度条 / 限速
- TLS 显式加密(FTPS)
