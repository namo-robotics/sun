# GPU matrix multiplication benchmark

Compares Sun, PyTorch, and CuPy on 512×512 float32 matrices. Use the
[GPU benchmark container and CUDA build instructions](../README.md) for setup.

## Run

From the repository root (adjust build and CUDA library paths as needed):

```sh
build/sun --lib-path build -lcublas -lcudart -lpthread benchmarks/gpu-matmul/main.sun
python3 benchmarks/gpu-matmul/compare.py pytorch
python3 benchmarks/gpu-matmul/compare.py cupy
```

Reports upload, allocating products, products with reusable outputs, and download
separately. After one warmup, arithmetic timings are **totals for ten products**.
Every operation synchronizes; results are validated outside the timed regions.
TF32 is disabled. Allocation and memory-pool differences affect results, so
reusable outputs give the closer arithmetic comparison. Compare runs on the same
idle GPU; these timings do not isolate language overhead.

## Charts

CI uploads `gpu-matmul.png` with the GPU logs, linked from the job summary.
To plot locally, save each run's output as `benchmark-sun.log`,
`benchmark-pytorch.log`, or `benchmark-cupy.log` in `tmp/gpu-matmul`, then run:

```sh
python3 -m pip install -r benchmarks/gpu-matmul/plot-requirements.txt
python3 benchmarks/gpu-matmul/plot.py tmp/gpu-matmul tmp/gpu-matmul/gpu-matmul.png
```

Plotting needs only Matplotlib, no GPU. In the benchmark container, skip the
install and use `/usr/bin/python3` for plotting; Matplotlib is already installed.
