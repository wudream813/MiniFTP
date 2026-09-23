cmake_minimum_required(VERSION 3.15)
project(miniftp VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认 Release(双击即用,体积小速度快)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(miniftp
  src/main.cpp
  src/net.cpp
  src/utils.cpp
  src/ftp_server.cpp
  src/ftp_session.cpp
)

target_include_directories(miniftp PRIVATE src)

if(WIN32)
  target_link_libraries(miniftp PRIVATE ws2_32)
  # MSVC: UTF-8 源码 + 控制台
  if(MSVC)
    target_compile_options(miniftp PRIVATE /utf-8 /W3)
  endif()
else()
  # Linux/macOS 线程库
  find_package(Threads REQUIRED)
  target_link_libraries(miniftp PRIVATE Threads::Threads)
  target_compile_options(miniftp PRIVATE -Wall -Wextra)
endif()

# 输出到 bin/,方便打包
set_target_properties(miniftp PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
