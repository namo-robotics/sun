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
`false`; it disables both packed tiles, direct 4×4 tiles, and the SIMD row remainder
path, retaining the scalar loop. The runner fails if it cannot
locate that exact branch. Tracked source files and precompiled bundles are not
modified.

`sun --emit-llvm -O0` exports the complete module. LLVM `opt` then applies O3 to
both variants with loop and SLP vectorization disabled. Unreachable code is
removed, and an IR audit rejects vector floating arithmetic in the scalar build
and requires vector multiply-add in the SIMD build. Thus “scalar” does not mean
unoptimized. Explicit SIMD uses four-lane and eight-lane intrinsics. Larger products
automatically select an isolated AVX2/FMA 4×16 tile when both CPU and OS
support it; small products skip the runtime check. Unsupported CPUs use the
portable 4×8 tile. Both use bounded stack scratch and first-panel beta handling.
The specialized path uses larger cache panels; all paths preserve accumulation
order. This controlled pipeline isolates explicit SIMD; it does not measure the default
Sun compiler pipeline with automatic vectorization enabled. Both Sun variants
use LLVM's portable target. The SIMD implementation can select its isolated
AVX2/FMA kernel at runtime, so host-specific compilation is unnecessary.
OpenBLAS dispatches independently. Sun SIMD permits hardware fused multiply-add;
targets without hardware FMA use ordinary multiply/add in the portable path.
The scalar comparison retains separate multiply/add. Results are checked with
numerical tolerances.

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

The runtime-selected kernel uses eight independent vector accumulators, with
no spills in the measured inner loop. CPU detection checks AVX2, FMA, OSXSAVE,
and enabled XMM/YMM state before entering the non-inlined specialized function.
Tests also exercise emulated CPUs missing these features. The portable fallback
retains 4×8 tiles; the selected path uses 4×16 tiles and larger cache panels.
The work threshold avoids dispatch overhead on small products. Panel and tile
choices remain internal implementation details, not configuration options.

## Packing and remainder measurements

Coarse phase timers in temporary instrumented sources separated A packing,
B packing, and packed-tile execution. On the measured machine, packing accounted
for roughly 9–12% of square-product time. Remainder work and dispatch accounted
for about 47% of the 127×129×131 case before remainder sums were kept in registers.
These diagnostic timings include instrumentation overhead; the normal benchmark
and chart use uninstrumented code.

The remainder kernels now load and store each output sum once, preserve
increasing inner-dimension order, and share A loads across the last one to three
columns. An alternative loop order that reused packed A across column panels
showed mixed gains while increasing B packing, so the existing packing order
was retained. Performance measurements remain specific to CPU and shape.

Further comparisons alternated the current kernel with inner-loop unroll factors
of two, four, and eight across square and irregular products. A factor of two
improved the measured 128–512 square cases by roughly 3–6% and the irregular
cases by roughly 3–4%; it did not improve the 1024 case. Larger factors were less
consistent. The AVX2/FMA kernel therefore requests two-step unrolling, retaining
increasing accumulation order and a cleanup iteration for odd inner dimensions.
This is an internal LLVM optimization hint, not a new user option.

Separate experiments with deeper inner panels, wider column panels, and larger
row blocks produced mixed gains, so full-tile panel sizes remain unchanged.

The selected AVX2/FMA path now handles partial tiles through a separate edge
pass. It reuses packing scratch with deeper inner panels, zero-fills missing
input lanes, and copies only valid output elements through a small stack tile.
Complete tiles retain their existing packing loop. An earlier implementation
added edge checks to that loop and regressed several aligned and single-edge
cases, so it was not retained. A single leftover column retains scalar
accumulation to avoid excessive padding. Alternating before/after measurements
showed roughly 15–17% higher throughput for 127×129×131 and roughly 10% for
255×257×259 with the separate edge pass; these are local measurements, not
performance guarantees. Further square-product tuning should measure
packing and per-tile overhead; performance remains dependent on CPU and shape.

## Packed-block execution

The AVX2/FMA path processes complete packed blocks in one specialized function,
instead of calling it separately for every 4×16 tile. The generated block kernel
has no internal calls or vector stack accesses in the inspected build and emits
one `vzeroupper` on exit. Packing layout, scratch capacity, beta handling, and
per-output accumulation order are unchanged.

Alternating comparisons against saved tile-call binaries favored block execution
by roughly 2–5% on the measured medium-size products; the 1024 case showed little
reliable change. Both row-first and column-first tile orders were tested. The
column-first order did not consistently improve results, so row-first was kept.
Absolute throughput varied substantially between runs; use alternating comparisons
to assess a change rather than comparing charts collected at different times.
