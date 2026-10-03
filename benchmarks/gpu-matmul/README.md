# GPU matrix multiplication benchmark

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
