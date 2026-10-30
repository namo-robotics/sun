# syntax=docker/dockerfile:1
FROM ubuntu:26.04

LABEL devcontainer.feature="LLVM 20 Dev Environment (Ubuntu 26.04 LTS)"

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=Etc/UTC

# Use the same native build and test dependencies as host development.
COPY scripts/install-dependencies.sh /opt/sun/scripts/install-dependencies.sh
RUN bash /opt/sun/scripts/install-dependencies.sh --yes \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

RUN apt-get update --error-on=any && apt-get install -y --no-install-recommends \
    # Additional development tools for the container
    python3-pip lldb-20 libllvm-20-ocaml-dev \
    clangd-20 clang-format-20 clang-tidy-20 clang-tools-20 \
    # NVIDIA CUDA compiler, headers, and libraries; GPU drivers belong on the host.
    nvidia-cuda-toolkit \
    # Profiling and disassembly development libraries
    libpfm4-dev libdw-dev libcapstone-dev \
    # Compiler API reference generation
    doxygen \
    # Quality-of-life tools
    gdb vim less htop wget unzip \
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
    protobuf-compiler \
    libprotobuf-dev \
    libprotoc-dev \
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

# Supply the Git-library example's musl OpenSSL and zlib archives in fresh containers.
COPY scripts/fetch-openssl.sh /opt/sun/scripts/fetch-openssl.sh
RUN bash /opt/sun/scripts/fetch-openssl.sh --arch x86_64 \
    && rm -rf /opt/sun/tmp
ENV SUN_EXAMPLE_NATIVE_LIBS="/opt/sun/third_party/openssl/x86_64-linux-musl"

# GitHub CLI from the official apt repo (newer than the Ubuntu archive build)
RUN mkdir -p -m 755 /etc/apt/keyrings \
    && curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg -o /etc/apt/keyrings/githubcli-archive-keyring.gpg \
    && chmod go+r /etc/apt/keyrings/githubcli-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" > /etc/apt/sources.list.d/github-cli.list \
    && apt-get update && apt-get install -y --no-install-recommends gh \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# Grant sudo to existing ubuntu user (UID 1000 already exists in Ubuntu 24.04+)
RUN echo "ubuntu ALL=(root) NOPASSWD:ALL" > /etc/sudoers.d/ubuntu \
    && chmod 0440 /etc/sudoers.d/ubuntu

# Eager-load git completion (needed for alias completion)
RUN echo '[ -f /usr/share/bash-completion/completions/git ] && . /usr/share/bash-completion/completions/git' > /etc/profile.d/git-completion.sh

USER ubuntu
CMD ["/bin/bash"]
