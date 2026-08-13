# syntax=docker/dockerfile:1
FROM ubuntu:26.04

LABEL devcontainer.feature="LLVM 20 Dev Environment (Ubuntu 26.04 LTS)"

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=Etc/UTC

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Core build tools
    build-essential cmake ninja-build git python3 python3-pip \
    # LLVM 20 full development suite (native since 25.10)
    clang-20 lldb-20 lld-20 \
    llvm-20 llvm-20-dev llvm-20-tools \
    libllvm-20-ocaml-dev \
    clangd-20 clang-format-20 clang-tidy-20 clang-tools-20 \
    # Common LLVM dev dependencies
    libzstd-dev zlib1g-dev libxml2-dev libedit-dev libncurses-dev \
    libcurl4-openssl-dev libpfm4-dev libdw-dev libcapstone-dev \
    # Quality-of-life tools
    gdb ccache vim less htop wget curl unzip \
    # Profilers. valgrind (with callgrind_annotate) works unprivileged and
    # gives exact per-function instruction counts -- run it under
    # `ulimit -n 4096`, since Docker's default fd limit makes valgrind abort.
    # perf additionally needs the container started with --cap-add=PERFMON
    # (or --privileged) and a writable /proc/sys/kernel/perf_event_paranoid,
    # otherwise it reports "No permission to enable task-clock event".
    valgrind linux-tools-common linux-tools-generic \
    && apt-get clean && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# Set modern LLVM 20 as default (usually already the case, but explicit is safer)
RUN update-alternatives --install /usr/bin/clang     clang     /usr/bin/clang-20     100 \
 && update-alternatives --install /usr/bin/clang++   clang++   /usr/bin/clang++-20   100 \
 && update-alternatives --install /usr/bin/clangd    clangd    /usr/bin/clangd-20    100 \
 && update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-20 100


RUN apt-get update && apt-get install -y --no-install-recommends \
    # Debian packaging tools (scripts/build-deb.sh)
    debhelper devscripts \
    libgtest-dev \
    protobuf-compiler \
    libprotobuf-dev \
    openssh-client \
    sudo \
    bash-completion \
    locales \
    # Cross-compilation to AArch64: toolchain+sysroot to link `sun --target
    # aarch64-linux-gnu -c` output, qemu-user to run the result on x86
    # (qemu-aarch64 -L /usr/aarch64-linux-gnu <binary>)
    g++-aarch64-linux-gnu \
    qemu-user \
    && locale-gen en_US.UTF-8 \
    && apt-get clean && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

ENV LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8

# Grant sudo to existing ubuntu user (UID 1000 already exists in Ubuntu 24.04+)
RUN echo "ubuntu ALL=(root) NOPASSWD:ALL" > /etc/sudoers.d/ubuntu \
    && chmod 0440 /etc/sudoers.d/ubuntu

# Eager-load git completion (needed for alias completion)
RUN echo '[ -f /usr/share/bash-completion/completions/git ] && . /usr/share/bash-completion/completions/git' > /etc/profile.d/git-completion.sh

CMD ["/bin/bash"]