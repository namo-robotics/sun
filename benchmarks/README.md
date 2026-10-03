# Benchmark containers

Benchmark frameworks and comparison toolchains live in a separate image.
The devcontainer builds only the root Dockerfile; it does not install Rust,
Go, .NET, Matplotlib, PyTorch, or CuPy for benchmarks.

Build from the repository root:

```sh
docker build -t sun-dev:local .
docker build -f benchmarks/Dockerfile --target cpu -t sun-benchmarks:cpu .
# Optional NVIDIA comparisons (Linux x86-64, CUDA 12.4 PyTorch wheels):
docker build -f benchmarks/Dockerfile --target gpu -t sun-benchmarks:gpu .
```

The benchmark image reuses the development image's compiler and CUDA toolkit.
`--build-arg SUN_DEV_IMAGE=your-image:tag` selects another compatible development
image. The GPU target adds Python 3.12, PyTorch, and CuPy under `/opt`; it does not
change the development image or install Python packages into the workspace.
Its framework versions are fixed for repeatable comparisons, rather than tracking
the newest releases. ARM64 and Jetson framework images are not provided here.

Run the CPU image with the checkout mounted:

```sh
docker run --rm -it --user "$(id -u):$(id -g)" \
  -e HOME=/workspaces/sun/tmp/benchmark-home \
  -v "$PWD:/workspaces/sun" -w /workspaces/sun sun-benchmarks:cpu bash
```

Inside the container, create the writable home and build Sun in a separate
directory to avoid mixing host and container CMake caches:

```sh
mkdir -p "$HOME"
cmake -S . -B tmp/benchmark-build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build tmp/benchmark-build -j$(($(nproc)/2))
SUN=$PWD/tmp/benchmark-build/sun python3 benchmarks/string-creation/run.py
```

For GPU benchmarks the host needs a working NVIDIA driver and
[NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html).
Launch the GPU image with the same mount and user settings:

```sh
docker run --rm -it --gpus all --user "$(id -u):$(id -g)" \
  -e HOME=/workspaces/sun/tmp/benchmark-home \
  -v "$PWD:/workspaces/sun" -w /workspaces/sun sun-benchmarks:gpu bash
```

Then run:

```sh
mkdir -p "$HOME"
cmake -S . -B tmp/benchmark-build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DSUN_ENABLE_CUDA=ON
cmake --build tmp/benchmark-build -j$(($(nproc)/2))
tmp/benchmark-build/sun --lib-path tmp/benchmark-build \
  -lcublas -lcudart -lpthread benchmarks/gpu-matmul/main.sun
python3 benchmarks/gpu-matmul/compare.py pytorch
python3 benchmarks/gpu-matmul/compare.py cupy
```

See each benchmark's README for timing methodology. The GPU Python environment
is for the matrix comparison; use `/usr/bin/python3` for CPU chart generation
when running the GPU image.
