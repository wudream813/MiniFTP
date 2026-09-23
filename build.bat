@echo off
REM MiniFTP 一键构建(Windows)
REM 优先 CMake + MSVC,退化 MinGW g++
cd /d "%~dp0"
where cmake >nul 2>nul
if %errorlevel%==0 (
  echo ==^> 用 CMake 构建...
  if not exist build mkdir build
  cd build
  cmake -DCMAKE_BUILD_TYPE=Release ..
  if errorlevel 1 (
    echo CMake 配置失败,请安装 Visual Studio(含 C++ 桌面开发)后重试。
    pause & exit /b 1
  )
  cmake --build . --config Release -j
  echo ==^> 完成: build\bin\miniftp.exe
  pause & exit /b 0
)
where g++ >nul 2>nul
if %errorlevel%==0 (
  echo ==^> 未找到 cmake,用 g++ 直接编译...
  if not exist bin mkdir bin
  g++ -std=c++17 -O2 -o bin\miniftp.exe src\main.cpp src\net.cpp src\utils.cpp src\ftp_server.cpp src\ftp_session.cpp -lws2_32
  echo ==^> 完成: bin\miniftp.exe
  pause & exit /b 0
)
echo 未找到 cmake 和 g++,请先安装其一:
echo   1) Visual Studio 2022(含 C++ 桌面开发)+ CMake
echo   2) 或 MSYS2 MinGW-w64
pause & exit /b 1
