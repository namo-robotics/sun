# syntax=docker/dockerfile:1
FROM ubuntu:25.10

LABEL devcontainer.feature="LLVM 20 Dev Environment (Ubuntu 25.10)"

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=Etc/UTC

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Core build tools
    build-essential cmake ninja-build git python3 python3-pip \
    # LLVM 20 full development suite (native in 25.10!)
    clang-20 lldb-20 lld-20 \
    llvm-20 llvm-20-dev llvm-20-tools \
    libllvm-20-ocaml-dev \
    clangd-20 clang-format-20 clang-tidy-20 clang-tools-20 \
    # Common LLVM dev dependencies
    libzstd-dev zlib1g-dev libxml2-dev libedit-dev libncurses-dev \
    libcurl4-openssl-dev libpfm4-dev libdw-dev libcapstone-dev \
    # Quality-of-life tools
    gdb ccache vim less htop wget curl unzip \
    && apt-get clean && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# Set modern LLVM 20 as default (usually already the case, but explicit is safer)
RUN update-alternatives --install /usr/bin/clang     clang     /usr/bin/clang-20     100 \
 && update-alternatives --install /usr/bin/clang++   clang++   /usr/bin/clang++-20   100 \
 && update-alternatives --install /usr/bin/clangd    clangd    /usr/bin/clangd-20    100 \
 && update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-20 100


RUN apt-get update && apt-get install -y --no-install-recommends \
    libgtest-dev \
    openssh-client \
    sudo \
    && apt-get clean && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# Grant sudo to existing ubuntu user (UID 1000 already exists in Ubuntu 25.10+)
RUN echo "ubuntu ALL=(root) NOPASSWD:ALL" > /etc/sudoers.d/ubuntu \
    && chmod 0440 /etc/sudoers.d/ubuntu

CMD ["/bin/bash"]