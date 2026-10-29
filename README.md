<p align="center">
  <img src="assets/sun_banner.jpg" alt="Sun Logo">
</p>

<h1 align="center">The Sun Programming Language</h1>

<p align="center">
  <a href="https://github.com/namo-robotics/sun/actions/workflows/ci.yml">
    <img src="https://github.com/namo-robotics/sun/actions/workflows/ci.yml/badge.svg" alt="CI">
  </a>
</p>

<p align="center">
  <a href="https://namo-robotics.github.io/sun/">Documentation</a>
</p>

## Build on Ubuntu/Debian

Install native compiler and test dependencies, then build:

```bash
./scripts/install-dependencies.sh
./build.sh --reconfigure
```

The installer uses your configured APT repositories, which must provide LLVM 20
and its matching libclang development package. It uses `sudo` when needed.
Pass `--yes` to accept package installation prompts or `--dry-run` to preview
commands without changing the system. The Dockerfile uses this same installer.

Static musl linking is the preferred Linux default, including stdlib and TLS
tests. On x86_64 Linux, the installer downloads the complete x86_64 and AArch64
musl toolchains from musl.cc into `/opt/cross`, including musl-built `libstdc++`.
It exposes their prefixed commands in `/usr/local/bin`; ensure that directory is
on your `PATH`. Existing installations are reused. The supplied musl archives
require an x86_64 host.

CMake downloads and builds the pinned Protobuf dependency. CUDA, GNU
cross-compilers, and release packaging tools are separate dependencies;
the Dockerfile installs additional tools for those workflows.

To build and install `sun`, `sun-lsp`, and the built standard Moon bundles:

```bash
./build.sh --install
```

Installation copies binaries to `/usr/local/bin` and bundles to
`/usr/local/lib/sun`, preserving target subdirectories. Plain `./build.sh` only
builds. Use `--prefix /absolute/path` with either operation to choose a different
installation location.

```bash
./build.sh --uninstall
```

Uninstall skips the build and removes the Sun binaries and standard bundles at
that prefix. It leaves system dependencies and unrelated files in place.
