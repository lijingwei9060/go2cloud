# Makefile — go2cloud 块级迁移工具编译
#
# 目标:
#   make all        — 构建 client 和 server (自动检测平台)
#   make client     — 仅构建客户端
#   make server     — 仅构建服务端
#   make clean      — 清理构建产物
#   make client_msvc — Windows MSVC 客户端
#   make server_msvc — Windows MSVC 服务端
#
# 依赖:
#   - zstd (libzstd)   — Zstandard 压缩库
#   - sqlite3          — SQLite 数据库
#   - pthread (Linux)  — POSIX 线程
#   - ws2_32 (Windows) — Winsock

# ============================================================
# 平台检测
# ============================================================

PLATFORM ?= $(if $(filter Windows%,$(OS)),windows,linux)
$(info Building for: $(PLATFORM))

# ============================================================
# GCC / MinGW 配置
# ============================================================

CC       = gcc
CFLAGS   = -O2 -Wall -Wextra
INCLUDES = -Iinclude

# 源文件
SERVER_SRC = server/main.c server/log.c server/session.c \
             server/block_writer.c server/protocol_decoder.c server/ack.c
CLIENT_SRC = client/main.c client/log.c client/hash.c client/msgpack.c \
             client/wire.c client/queue.c client/pool.c client/timer.c \
             client/sqlite.c client/volume.c client/block_io.c client/vss.c \
             client/driver_inject.c client/syschk.c client/dcbt.c

# 目标和依赖
SERVER_OBJ = $(SERVER_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

SERVER_BIN = receiver$(if $(filter windows,$(PLATFORM)),.exe,)
CLIENT_BIN = client$(if $(filter windows,$(PLATFORM)),.exe,)

# ============================================================
# 平台特定
# ============================================================

ifeq ($(PLATFORM),windows)
    # MinGW-w64
    LIBS_SERVER = -lzstd -lpthread -lws2_32
    LIBS_CLIENT = -lzstd -lsqlite3 -lole32 -lvssapi -lws2_32 -lpthread
    LDFLAGS     = -static
else
    # Linux
    LIBS_SERVER = -lzstd -lpthread
    LIBS_CLIENT = -lzstd -lsqlite3 -lpthread
    LDFLAGS     =
endif

# ============================================================
# 构建目标
# ============================================================

.PHONY: all server client clean server_msvc client_msvc

all: server client

server: $(SERVER_BIN)

client: $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_SRC) include/protocol.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SERVER_SRC) $(LIBS_SERVER) $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_SRC) include/protocol.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(CLIENT_SRC) $(LIBS_CLIENT) $(LDFLAGS)

# ============================================================
# MSVC 编译 (仅 Windows)
# ============================================================

client_msvc:
	cl /c /O2 /utf-8 \
	   client\main.c client\log.c client\hash.c client\msgpack.c \
	   client\wire.c client\queue.c client\pool.c client\timer.c \
	   client\sqlite.c client\volume.c client\block_io.c \
	   client\driver_inject.c client\syschk.c client\dcbt.c \
	   /Iinclude /Id:\vcpkg\installed\x64-windows\include
	cl /c /O2 /utf-8 /Tpclient\vss.c \
	   /Iinclude /Id:\vcpkg\installed\x64-windows\include
	link /OUT:client.exe main.obj log.obj hash.obj msgpack.obj wire.obj \
	   queue.obj pool.obj timer.obj sqlite.obj volume.obj block_io.obj \
	   driver_inject.obj syschk.obj dcbt.obj vss.obj \
	   /LIBPATH:d:\vcpkg\installed\x64-windows\lib \
	   zstd.lib sqlite3.lib ole32.lib vssapi.lib ws2_32.lib

server_msvc:
	cl /O2 /utf-8 /Fe:receiver.exe \
	   server\main.c server\log.c server\session.c \
	   server\block_writer.c server\protocol_decoder.c server\ack.c \
	   /Iinclude /Id:\vcpkg\installed\x64-windows\include \
	   /link /LIBPATH:d:\vcpkg\installed\x64-windows\lib zstd.lib ws2_32.lib

# ============================================================
# 清理
# ============================================================

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN)
	rm -f server/*.o client/*.o
