# CPU matrix multiplication

Compare the current Sun `Matrix<f32>.matmul_into` implementation with its explicit
SIMD branch disabled, the same implementation with SIMD enabled, and OpenBLAS
`cblas_sgemm`. NumPy is omitted because its matrix multiplication commonly delegates
to a BLAS implementation. LAPACK supplies higher-level algorithms; GEMM is BLAS.

Run from the repository root, preferably in the [CPU benchmark container](../README.md):

```sh
cmake --build build -j$(($(nproc)/2))
python3 benchmarks/cpu-matmul/run.py --cpu 2
```

Choose an allowed logical CPU (`taskset -pc $$` lists your affinity). Omit `--cpu`
if pinning is unavailable. Host setup requires Linux, Python with Matplotlib,
OpenBLAS development headers, pkg-config, and LLVM/Clang tools matching Sun's LLVM
major version. On the benchmark image these dependencies are installed already.
Defaults use `build/sun`, `clang-20`, `opt-20`, and `llc-20`; override with `SUN`,
`CC`, `OPT`, and `LLC`. A custom OpenBLAS installation can set `OPENBLAS_CFLAGS`
and `OPENBLAS_LIBS` (including its runtime search path).

```sh
python3 benchmarks/cpu-matmul/run.py --sizes 32 128 512 127x129x131 \
  --iterations 10 --samples 7 --warmups 2 --output tmp/cpu-matmul
# Enable host instructions equally for both Sun variants:
python3 benchmarks/cpu-matmul/run.py --native --output tmp/cpu-matmul-native
python3 -m unittest discover -s benchmarks/cpu-matmul -p 'test_*.py'
```

A size `N` means square matrices. `MxKxN` multiplies `M×K` by `K×N`.
All dimensions must be positive and at most 8192. Large scalar cases can take a
long time. This benchmark intentionally covers only non-transposed f32 GEMM,
with alpha one and beta zero; f64 and transposed-B Sun products currently have
no explicit SIMD kernel.

## Outputs

The output directory contains:

- `results.png` and `results.svg`: grouped bar charts of median GFLOP/s, with
  whiskers showing the minimum and maximum throughput across timed batches.
- `results.md`: table of throughput and SIMD speedup, with the chart embedded.
- `results.json`: per-product sample times, checksums, OpenBLAS configuration/core,
  CPU information, affinity, tool versions, source hashes, and vector IR audits.
- `commands.json`, command logs, staged sources, optimized LLVM IR, and native
  disassembly when `objdump` is available.

GFLOP/s uses the conventional `2*M*K*N` operation count. Charts use a linear axis
starting at zero. Results are machine-specific; no speedup threshold is imposed.

## Method

Both Sun variants compile the current standard library sources from temporary
copies. The scalar copy replaces only GEMM's explicit SIMD dispatch condition with
`false`; it disables packed 4×8 tiles, direct 4×4 tiles, and the SIMD row remainder
path, retaining the scalar loop. The runner fails if it cannot
locate that exact branch. Tracked source files and precompiled bundles are not
modified.

`sun --emit-llvm -O0` exports the complete module. LLVM `opt` then applies O3 to
both variants with loop and SLP vectorization disabled. Unreachable code is
removed, and an IR audit rejects vector floating arithmetic in the scalar build
and requires vector multiply/add in the SIMD build. Thus “scalar” does not mean
unoptimized. Explicit SIMD uses four-lane and eight-lane intrinsics. The larger-matrix path
uses bounded stack scratch for panel packing and cache blocking. This
controlled pipeline isolates explicit SIMD; it does not measure the default
Sun compiler pipeline with automatic vectorization enabled. Sun machine code
uses LLVM's default target CPU unless `--native` is selected. Native mode passes
`-mcpu=native` to LLVM for both Sun variants; these executables must stay on the
host they were built for. Metadata records the choice. On an AVX-capable x86
host this allows eight-lane operations to use 256-bit instructions. OpenBLAS
dispatches independently and may use FMA; Sun retains separate multiply/add.

Each backend receives the same deterministic, mixed-sign, exactly representable
inputs. Matrices and output are allocated once, outside timing. Warmups precede
measurement. A shared monotonic clock times repeated products; a shared compiler
barrier keeps each result observable. Every complete output is checked outside
timing, after warmups and each timed batch, against a separate double-precision
algebraic reference. The reference exploits the separable input formula rather
than reproducing the multiplication loop. These structured inputs verify the
benchmark; the standard-library tests provide broader numerical coverage.

All runs use one thread. The OpenBLAS driver explicitly requests and checks one
thread, in addition to setting thread environment variables. Participants run
sequentially, rotating their order between shapes. Timings include normal GEMM
validation/dispatch and output initialization; they exclude allocation, input
initialization, validation of results, and printing. OpenBLAS can still allocate
internal workspace and use packing, tiling, runtime dispatch, and wider vectors.
Consequently the OpenBLAS comparison measures the full implementations, not just
instruction width. Close background workloads and use repeated pinned runs for
performance conclusions.
