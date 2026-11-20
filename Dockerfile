FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates clang clang-format cmake g++ gdb git libclang-rt-18-dev \
    linux-tools-generic ninja-build \
    pybind11-dev python3 python3-matplotlib python3-pip python3-venv \
    && rm -rf /var/lib/apt/lists/*
ENV CC=clang CXX=clang++
WORKDIR /workspace
