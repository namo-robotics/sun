#!/usr/bin/env python3
"""Compare scalar Sun, explicit SIMD Sun, and single-threaded OpenBLAS SGEMM."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).resolve().parent
SIMD_CONDITION = "if (_is<f32>(alpha) and bc == 1 and n >= 4)"
LABELS = {"scalar": "Sun scalar", "simd": "Sun SIMD", "openblas": "OpenBLAS"}


def shape(value):
    """Parse N or MxKxN, using the same bounded positive dimensions as the harness."""
    try:
        dims = tuple(int(part) for part in value.lower().split("x"))
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected N or MxKxN") from error
    if len(dims) == 1:
        dims *= 3
    if len(dims) != 3 or any(d < 1 or d > 8192 for d in dims):
        raise argparse.ArgumentTypeError("expected N or MxKxN with dimensions in 1..8192")
    return dims


def scalar_source(source):
    """Disable only the explicit SIMD branch and fail if the kernel has changed."""
    if source.count(SIMD_CONDITION) != 1:
        raise RuntimeError("GEMM SIMD branch changed; update the scalar staging rule")
    return source.replace(SIMD_CONDITION, "if (false)")


def audit_ir(source, variant):
    """Reject vector floating arithmetic in scalar code and require it in SIMD code."""
    operations = re.findall(r"\b(fadd|fsub|fmul|fdiv|frem)\b[^\n]*<\s*\d+\s+x\s+(?:float|double)\s*>", source)
    if variant == "scalar" and operations:
        raise RuntimeError("scalar IR contains vector floating arithmetic")
    if variant == "simd" and not {"fadd", "fmul"}.issubset(operations):
        raise RuntimeError("SIMD IR lacks vector multiply/add")
    return {op: operations.count(op) for op in sorted(set(operations))}


def parse_samples(output, count, repetitions):
    """Require the requested samples and a successful validation per sample and warmup."""
    samples = [int(v) / repetitions for v in re.findall(r"^sample (\d+)$", output, re.M)]
    checksums = [float(v) for v in re.findall(r"^checksum (.+)$", output, re.M)]
    if len(samples) != count or any(v <= 0 for v in samples) or len(checksums) != count + 1:
        raise RuntimeError("incomplete benchmark output or invalid timings")
    if any(v != checksums[0] for v in checksums):
        raise RuntimeError("output changed between benchmark samples")
    return samples, checksums[0]


def plot_results(results, output):
    """Save a grouped throughput chart with sample ranges using a headless backend."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(max(8, len(results) * 1.8), 5))
    width = 0.25
    colors = ["#64748b", "#2563eb", "#16a34a"]
    for index, (variant, label) in enumerate(LABELS.items()):
        values, lower, upper = [], [], []
        for case in results:
            record = case["variants"][variant]
            values.append(record["gflops"])
            rates = [2 * case["m"] * case["k"] * case["n"] / ns for ns in record["samples_ns"]]
            lower.append(values[-1] - min(rates))
            upper.append(max(rates) - values[-1])
        positions = [i + (index - 1) * width for i in range(len(results))]
        ax.bar(positions, values, width, label=label, color=colors[index],
               yerr=[lower, upper], capsize=3)
    ax.set_xticks(range(len(results)), [f'{c["m"]}×{c["k"]}×{c["n"]}' for c in results])
    ax.set_xlabel("M × K × N (A: M × K, B: K × N)")
    ax.set_ylabel("GFLOP/s (higher is better)")
    ax.set_title("CPU f32 matrix multiplication · one thread\nMedian throughput; whiskers show sample range")
    ax.legend(loc="upper center", ncol=3)
    ax.grid(axis="y", alpha=0.2)
    ax.set_axisbelow(True)
    peak = max(2 * c["m"] * c["k"] * c["n"] / min(v["samples_ns"])
               for c in results for v in c["variants"].values())
    ax.set_ylim(0, peak * 1.22)
    fig.tight_layout()
    fig.savefig(output / "results.png", dpi=180)
    fig.savefig(output / "results.svg")
    plt.close(fig)


def main():
    """Build audited variants, measure identical workloads, and save charts and metadata."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", nargs="+", type=shape, default=[shape(s) for s in ("64", "128", "256", "512", "127x129x131")])
    parser.add_argument("--iterations", type=int, default=20, help="products per timed sample")
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--cpu", type=int, help="pin every measured process to this logical CPU")
    parser.add_argument("--output", type=Path, default=ROOT / "tmp/cpu-matmul")
    args = parser.parse_args()
    if Path.cwd() != ROOT:
        parser.error(f"run from {ROOT}")
    if platform.system() != "Linux":
        parser.error("this benchmark currently supports Linux")
    if any(v < 1 or v > 1000000 for v in (args.iterations, args.samples, args.warmups)):
        parser.error("iterations, samples, and warmups must be in 1..1000000")
    if args.cpu is not None and args.cpu not in os.sched_getaffinity(0):
        parser.error("CPU is outside this process's affinity")
    out = args.output.resolve()
    if not out.is_relative_to(ROOT / "tmp"):
        parser.error("output must be inside the workspace tmp directory")
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(TMPDIR=str(out), MPLCONFIGDIR=str(out / "matplotlib"),
               OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1", GOTO_NUM_THREADS="1")
    os.environ["MPLCONFIGDIR"] = env["MPLCONFIGDIR"]
    try:
        import matplotlib  # Check dependencies before expensive compilation.
    except ImportError:
        parser.error("Matplotlib is required for charts; use the benchmark container or install python3-matplotlib")
    names = {"sun": os.environ.get("SUN", str(ROOT / "build/sun")),
             "cc": os.environ.get("CC", "clang-20"),
             "opt": os.environ.get("OPT", "opt-20"),
             "llc": os.environ.get("LLC", "llc-20")}
    tools = {}
    for name, command in names.items():
        tools[name] = shutil.which(command)
        if not tools[name]:
            parser.error(f"missing {command}; set {name.upper()} or use the benchmark container")
    commands = []

    def run(label, command, pin=False):
        """Log complete command output and preserve the command list even on failure."""
        if pin and args.cpu is not None:
            command = ["taskset", "-c", str(args.cpu), *command]
        commands.append(command)
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        print(f"{label}: {shlex.join(command)}", flush=True)
        result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / f"{label}.log").write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f"{label} failed; see {out / (label + '.log')}\n{result.stdout}")
        return result.stdout

    if "OPENBLAS_LIBS" in env:
        blas_libs = shlex.split(env["OPENBLAS_LIBS"])
        blas_cflags = shlex.split(env.get("OPENBLAS_CFLAGS", ""))
    else:
        if not shutil.which("pkg-config"):
            parser.error("install pkg-config and libopenblas-dev, or set OPENBLAS_CFLAGS and OPENBLAS_LIBS")
        blas_cflags = shlex.split(run("openblas-cflags", ["pkg-config", "--cflags", "openblas"]))
        blas_libs = shlex.split(run("openblas-libs", ["pkg-config", "--libs", "openblas"]))
    versions = {name: run(f"version-{name}", [tool, "--version"]).strip() for name, tool in tools.items()}
    harness = str(SOURCE / "harness.c")
    run("build-harness", [tools["cc"], "-O3", "-std=c11", "-Wall", "-Wextra", "-DSUN_DRIVER", "-c", harness, "-o", str(out / "harness.o")])
    run("build-openblas", [tools["cc"], "-O3", "-std=c11", "-Wall", "-Wextra", *blas_cflags, harness, "-o", str(out / "openblas"), *blas_libs, "-lm"])
    audits = {}
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in [*sorted((ROOT / "stdlib").rglob("*.sun")), SOURCE / "main.sun", SOURCE / "harness.c", Path(__file__)]}
    hashes["sun_binary"] = hashlib.sha256(Path(tools["sun"]).read_bytes()).hexdigest()
    for variant in ("scalar", "simd"):
        stage = out / f"sources-{variant}"
        stage.mkdir(exist_ok=True)
        for source in (ROOT / "stdlib").rglob("*.sun"):
            target = stage / source.relative_to(ROOT / "stdlib")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        if variant == "scalar":
            kernel = stage / "std/linalg.sun"
            kernel.write_text(scalar_source(kernel.read_text()))
        manifest = (stage / "stdlib.sun").read_text().replace('source_files: [', 'source_files: ["main.sun",', 1)
        (stage / "stdlib.sun").write_text(manifest)
        shutil.copyfile(SOURCE / "main.sun", stage / "main.sun")
        raw, optimized, obj = (out / f"{variant}{suffix}" for suffix in (".raw.ll", ".ll", ".o"))
        run(f"emit-{variant}", [tools["sun"], "--emit-llvm", "-O0", "--force-rebuild", "-o", str(raw), str(stage / "stdlib.sun")])
        run(f"opt-{variant}", [tools["opt"], "-passes=internalize,globaldce,default<O3>", "--internalize-public-api-list=main", "--vectorize-loops=false", "--vectorize-slp=false", "-S", str(raw), "-o", str(optimized)])
        audits[variant] = audit_ir(optimized.read_text(), variant)
        run(f"object-{variant}", [tools["llc"], "-O3", "-filetype=obj", "-relocation-model=pic", str(optimized), "-o", str(obj)])
        run(f"link-{variant}", [tools["cc"], str(obj), str(out / "harness.o"), "-lm", "-lpthread", "-ldl", "-o", str(out / variant)])
        if shutil.which("objdump"):
            run(f"assembly-{variant}", ["objdump", "-d", str(out / variant)])
    results = []
    for index, (m, k, n) in enumerate(args.sizes):
        case = {"m": m, "k": k, "n": n, "variants": {}}
        variants = list(LABELS)
        variants = variants[index % 3:] + variants[:index % 3]
        for variant in variants:
            output = run(f"measure-{index}-{variant}", [str(out / variant), *map(str, (m, k, n, args.iterations, args.samples, args.warmups))], pin=True)
            samples, checksum = parse_samples(output, args.samples, args.iterations)
            median = statistics.median(samples)
            case["variants"][variant] = {"samples_ns": samples, "median_ns": median,
                                          "gflops": 2 * m * k * n / median, "checksum": checksum,
                                          "backend": re.findall(r"^backend (.+)$", output, re.M)}
        if len({r["checksum"] for r in case["variants"].values()}) != 1:
            raise RuntimeError("backends disagree on checksum")
        results.append(case)
    metadata = {"timestamp_utc": datetime.now(timezone.utc).isoformat(), "platform": platform.platform(),
                "cpu_info": Path("/proc/cpuinfo").read_text(), "affinity": sorted(os.sched_getaffinity(0)),
                "pinned_cpu": args.cpu, "dtype": "f32", "threads": 1, "iterations": args.iterations,
                "samples": args.samples, "warmups": args.warmups, "versions": versions,
                "source_hashes": hashes, "vector_ir_audit": audits, "results": results}
    (out / "results.json").write_text(json.dumps(metadata, indent=2) + "\n")
    rows = ["| M × K × N | Sun scalar GFLOP/s | Sun SIMD GFLOP/s | OpenBLAS GFLOP/s | SIMD / scalar |",
            "| --- | ---: | ---: | ---: | ---: |"]
    for case in results:
        v = case["variants"]
        rates = " | ".join(f'{v[name]["gflops"]:.3f}' for name in LABELS)
        rows.append(f'| {case["m"]} × {case["k"]} × {case["n"]} | {rates} | {v["scalar"]["median_ns"] / v["simd"]["median_ns"]:.2f}× |')
    report = "\n".join(rows) + "\n\n![Median throughput](results.png)\n"
    (out / "results.md").write_text(report)
    plot_results(results, out)
    print("\n" + report + f"\nResults and charts: {out}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError) as error:
        sys.exit(str(error))
