#!/bin/bash
# MiniFTP 一键构建(Linux/macOS)
set -e
cd "$(dirname "$0")"
if command -v cmake >/dev/null 2>&1; then
  echo "==> 用 CMake 构建..."
  mkdir -p build && cd build
  cmake -DCMAKE_BUILD_TYPE=Release ..
  cmake --build . -j
  echo "==> 完成: build/bin/miniftp"
else
  echo "==> 未找到 cmake,改用 g++ 直接编译..."
  make
  echo "==> 完成: bin/miniftp"
fi
