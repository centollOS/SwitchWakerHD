# the host toolchain for dksh_cache (build.sh): Debian's g++, CMake, Ninja and zlib
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake ninja-build zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
