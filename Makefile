# MiniFTP - 免 CMake 快捷构建(Linux/macOS/MinGW)
# 用法: make        -> 生成 ./bin/miniftp
#       make run    -> 构建并启动(默认 ./data,2121 端口)
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
SRC = src/main.cpp src/net.cpp src/utils.cpp src/ftp_server.cpp src/ftp_session.cpp
BIN = bin/miniftp

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifeq ($(UNAME_S),Linux)
  LDLIBS += -pthread
endif

all: $(BIN)

$(BIN): $(SRC)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $(BIN) $(SRC) $(LDLIBS)

run: $(BIN)
	./$(BIN)

clean:
	rm -rf bin build

.PHONY: all run clean
