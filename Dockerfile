FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    gcc-riscv64-linux-gnu \
    g++-riscv64-linux-gnu \
    qemu-user \
    python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app