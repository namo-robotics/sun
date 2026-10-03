# syntax=docker/dockerfile:1
# Extend the compiler environment without adding benchmark packages to it.
ARG SUN_DEV_IMAGE=sun-dev:local
FROM ${SUN_DEV_IMAGE} AS cpu

USER root
RUN apt-get update --error-on=any && apt-get install -y --no-install-recommends \
    rustc cargo golang-go nodejs python3-matplotlib libicu78 \
    && rm -rf /var/lib/apt/lists/*

# The string benchmark targets .NET 8, which is installed only in this image.
RUN curl -fsSL --retry 3 https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh \
    && bash /tmp/dotnet-install.sh --channel 8.0 --install-dir /opt/dotnet --no-path \
    && rm /tmp/dotnet-install.sh
ENV DOTNET_ROOT=/opt/dotnet \
    PATH="/opt/dotnet:${PATH}"
WORKDIR /workspaces/sun
CMD ["/bin/bash"]

# CUDA comparison wheels use a separate Python version from the base OS.
FROM cpu AS gpu
COPY --from=ghcr.io/astral-sh/uv:0.10.0 /uv /usr/local/bin/uv
ENV UV_PYTHON_INSTALL_DIR=/opt/benchmark-python
RUN uv python install 3.12 \
    && uv venv --python 3.12 /opt/gpu-bench
COPY benchmarks/gpu-matmul/requirements.txt /opt/gpu-bench/requirements.txt
RUN uv pip install --no-cache --python /opt/gpu-bench/bin/python \
    --index-url https://download.pytorch.org/whl/cu124 torch==2.6.0 \
    && uv pip install --no-cache --python /opt/gpu-bench/bin/python \
    -r /opt/gpu-bench/requirements.txt \
    && /opt/gpu-bench/bin/python -c "import torch, cupy; assert torch.version.cuda == '12.4'"
ENV PATH="/opt/gpu-bench/bin:${PATH}" \
    NVIDIA_DRIVER_CAPABILITIES=compute,utility
