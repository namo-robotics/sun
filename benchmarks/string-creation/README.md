# Integer-to-string creation

Reproduces [Daniel Lemire's benchmark](https://lemire.me/blog/2026/09/25/how-many-strings-can-you-create-per-second/), adding Sun's standard string interpolation. The [original post](https://x.com/lemire/status/2103455184860893435/photo/1) compares the time to create small owned decimal strings.

The latest measurements are in [RESULTS.md](RESULTS.md) and `strings.png`. The
[optimization report](OPTIMIZATION.md) records the formatter, LLVM pipeline, and
small-string-storage changes and the new measurements.

## Method

Each loop converts successive integers to new strings and replaces `buffer[i & 1023]`. The buffer retains 1,024 strings. Each timed run performs 100,000,000 conversions, except Python, which performs 10,000,000 as in the original. The reported result is the best of five runs. Compiled languages and JavaScript receive the original 1,000,000-iteration warm-up; Python retains its original harness without a separate warm-up.

The C++, Go, JavaScript, Nim, Python, and Rust implementations are included here, based on Daniel Lemire's [source at commit `361c1f2ed0291573bcaccaab4ee2d5a96188fe5b`](https://github.com/lemire/Code-used-on-Daniel-Lemire-s-blog/tree/361c1f2ed0291573bcaccaab4ee2d5a96188fe5b/2026/09/24). Their executable benchmark logic is unchanged; documentation comments were added. The Rust manifest and lockfile pin `itoa` to `1.0.18`, the article's version. Python and JavaScript also execute the original harness's other cases; only integer conversion appears in the comparison.

Sun uses ``buf.set(i & 1023, `${i}`)`` with `Vec<String>`. This creates a fresh owned string for each integer and drops the previous slot's string. It uses checked access and no unsafe blocks. Initialization, validation, output, and final destruction are outside the timer; replacement and destruction of overwritten strings are inside it. Sun checks every retained string by parsing it back to the expected integer after warm-up and each timed run. Its output also reports all five elapsed times. The runner checks the original native implementations' retained-length checksums and Sun's checksum against 8,192.

Sun is compiled ahead of time with its default optimizations enabled and `--dynamic`, using a freshly built standard library from this checkout. C++ uses Clang `-O3 -std=c++20` and libstdc++; Rust uses release mode with optimization level 3; Nim uses `-d:danger` and its default C backend. Go uses its normal build defaults. Sun, C++, Rust, and Nim use this machine's glibc allocator where their implementations allocate strings.

The current compiler uses LLVM's standard O3 module pipeline; `-O0` disables it.
The current string implementation stores short text inline, avoiding an allocation
for every integer in this benchmark. Each iteration still creates a new owned
string and replaces the previous ring slot.

All benchmark processes run sequentially, pinned to logical CPU 1, a performance core on an Intel Core Ultra 9 185H. There is no frequency lock or exclusive machine reservation. These are local measurements, not numbers to combine with the article's Apple M4 Max measurements. Best-of-five timing does not quantify uncertainty or establish general language performance.

## Reproduce

Run from the workspace root. Build Sun first and put Clang, Rust/Cargo, Go, Nim, Node.js, Bun, and Python on `PATH`:

```sh
cmake --build build -j 4
python3 benchmarks/string-creation/run.py --cpu 1
```

`SUN`, `CXX`, and `PYTHON` can select alternative compiler/interpreter executables. `--cpu` may be omitted on platforms without `taskset`; select an available performance core on hybrid processors. No benchmark source is downloaded. Cargo needs network access on its first run to obtain the locked Rust dependency. The runner saves build products, caches, full command output, versions, source hashes, `results.json`, and `results.md` under `tmp/string-creation/`. `--output` selects another scratch directory; `benchmarks/string-creation/results/` is an ignored option inside the benchmark tree. It rebuilds `build/stdlib.moon` for the Sun entrypoint.

With Matplotlib installed, regenerate the chart:

```sh
python3 benchmarks/string-creation/plot.py tmp/string-creation/results.json tmp/string-creation/strings.png
```

The current checked-in snapshot is in [RESULTS.md](RESULTS.md), with [machine-readable metadata](results.json), [program output](output.txt), and a [chart](strings.png).

## Included code

- [Sun](bench.sun), [C++](bench.cpp), [Go](bench.go), [Nim](bench.nim), and [Python](bench.py).
- [JavaScript](bench.js), executed by both Node.js and Bun.
- [Rust](rust/src/main.rs), with [Cargo.toml](rust/Cargo.toml) and [Cargo.lock](rust/Cargo.lock).
- [Runner](run.py), [chart generator](plot.py), and optional [chart dependencies](requirements.txt).

## CI

The `string-creation-benchmark` job in [ci.yml](../../.github/workflows/ci.yml)
runs alongside the build/test jobs on its own Ubuntu runner. It has no `needs`,
no downstream jobs depend on it, and job-level `continue-on-error: true` makes
failures informational ([GitHub's workflow semantics](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#jobsjob_idcontinue-on-error)).
There are no performance pass/fail thresholds. Keep `String creation benchmark
(informational)` out of branch protection's required status checks; workflow
files do not edit repository-level branch protection.

The job installs pinned versions of Python, Node.js, Go, Rust, Bun, and Nim;
builds Sun and its standard library from the checked-out commit; and runs every
language sequentially on one available CPU. It publishes a summary and uploads
JSON, Markdown, a PNG chart, and logs as `string-creation-<run-id>-<attempt>`.
Logs are uploaded even if the benchmark fails. The job has a timeout and uses
read-only repository permissions. Hosted-runner hardware and load can change,
so compare languages within a run rather than treating differences between CI
runs as precise regressions.

Generated binaries, Rust/Nim build directories, Python caches, and local `results/` output are ignored by [benchmarks/.gitignore](../.gitignore). The source files, Cargo lockfile, and named result snapshots are intentionally trackable.
