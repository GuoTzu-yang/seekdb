# MiniVectorDB —— 多阶段构建，运行时不含编译工具链
# 题面要求：Ubuntu 24.04 基础镜像、前台运行、stdin/stdout 交互、数据写 /data、禁网
#
# 目录约定（相对 seekdb 仓库 2026-competition-round2 分支根）：
#   Dockerfile                 本文件
#   agent/conversation.json    Agent Prompt 材料（题面默认读取路径）
#   minivectordb/              源码（CMakeLists.txt + src/ + README.md）
# 这样不会与 seekdb 仓库自身的构建文件冲突。

# ---------- 构建阶段 ----------
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      g++ cmake make ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY minivectordb/CMakeLists.txt ./
COPY minivectordb/src ./src
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && ls -la build/minivectordb

# ---------- 运行阶段 ----------
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
# 运行期只保留运行时库，不装 g++/cmake，镜像更小、更接近禁网环境。
# 注意：libgomp1 必须有——OpenMP 并行算距离会让二进制依赖 libgomp.so.1，
# 缺了它容器直接报 "error while loading shared libraries" 起不来。
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 libgomp1 \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /data

COPY --from=build /src/build/minivectordb /usr/local/bin/minivectordb

# 构建后自检：依赖必须齐全（早失败好过评测时才发现）
RUN /usr/local/bin/minivectordb --self-check

# 数据统一落在 /data（题面 2.3）
WORKDIR /data

# 前台运行，stdin 收 SQL、stdout 回结果；不 EXPOSE、不监听端口
# 用 exec 形式保证 PID 1 直接是数据库进程，能正确收到 SIGTERM
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data-dir", "/data"]
