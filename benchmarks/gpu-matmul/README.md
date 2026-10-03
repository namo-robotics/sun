# GPU matrix multiplication benchmark

Use the separate [benchmark container](../README.md) to run Sun, PyTorch, and
CuPy together. These dependencies are not installed in the devcontainer.

Build the optional CUDA library and run from the repository root:

```sh
build/sun --lib-path build -lcublas -lcudart -lpthread benchmarks/gpu-matmul/main.sun
```

Add the toolkit's library directory to `-L` and `LD_LIBRARY_PATH` if needed.
The benchmark reports upload (including device allocation), allocating products,
products with reusable outputs, and download separately. Each operation waits
for completion, so host timing includes launch and synchronization overhead.
A warmup multiplication runs before the timed arithmetic loops. Results depend
on the device and toolkit; there is no performance pass/fail threshold.

## PyTorch and CuPy comparisons

Run the same workload in Python using either framework:

```sh
python3 benchmarks/gpu-matmul/compare.py pytorch
python3 benchmarks/gpu-matmul/compare.py cupy
```

These are optional benchmark dependencies. Install a CUDA-enabled PyTorch build
following the [PyTorch installation instructions](https://pytorch.org/get-started/locally/).
For CUDA 12, CuPy provides the `cupy-cuda12x` package; follow the
[CuPy installation instructions](https://docs.cupy.dev/en/stable/install.html)
for your platform. Each command only imports the selected framework.

All three programs use device zero, two contiguous 512×512 float32 matrices
filled with ones, one warmup product, ten allocating products, and ten products
with reusable output storage. Arithmetic timings are **totals for ten products**;
divide by ten for average latency. Upload includes both input matrices; download
copies one result into existing host storage. Every downloaded element is checked
against 512 outside the timed regions. Initialization and host input creation are
excluded, but uploads can include first-use allocation overhead.

The Python comparisons synchronize after each upload, multiplication, and
download to match Sun's synchronous API. PyTorch uses
[`torch.mm(..., out=...)`](https://docs.pytorch.org/docs/stable/generated/torch.mm.html)
and CuPy uses
[`cupy.matmul(..., out=...)`](https://docs.cupy.dev/en/stable/reference/generated/cupy.matmul.html)
for output reuse. TF32 is disabled, and PyTorch uses
[`highest` float32 precision](https://docs.pytorch.org/docs/stable/generated/torch.set_float32_matmul_precision.html).
No autograd graph or JIT compilation is involved.

Run each command several times on the same idle GPU and record the GPU, driver,
toolkit, and framework versions with any reported results. Compare the four
timings separately. Python frameworks retain their default memory pools, while
Sun currently allocates and zeroes fresh device storage for each allocating
product. Their CUDA libraries may also choose different multiplication
algorithms. These results compare the actual APIs, including those costs; they
do not isolate language overhead. Reusable outputs provide the closer arithmetic
comparison. JAX compilation and multi-operation throughput are outside this
small synchronous benchmark's scope.
