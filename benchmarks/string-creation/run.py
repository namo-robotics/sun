#!/usr/bin/env python3
"""Build the included string benchmarks with Sun and save local results."""

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
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).resolve().parent
UPSTREAM = "361c1f2ed0291573bcaccaab4ee2d5a96188fe5b"
FILES = ["bench.cpp", "bench.go", "bench.js", "bench.nim", "bench.py",
         "rust/Cargo.toml", "rust/Cargo.lock", "rust/src/main.rs"]


def main():
    """Run all languages sequentially on one CPU and record reproducible metadata."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpu", type=int, default=None,
                        help="CPU for every benchmark process (recommended on hybrid CPUs)")
    parser.add_argument("--output", type=Path, default=ROOT / "tmp/string-creation")
    args = parser.parse_args()
    if Path.cwd() != ROOT:
        parser.error(f"run this command from {ROOT}")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["TMPDIR"] = str(out)
    env["CARGO_HOME"] = str(out / "cargo-home")
    env["CARGO_TARGET_DIR"] = str(out / "rust-target")
    env["GOCACHE"] = str(out / "go-cache")
    env["GOPATH"] = str(out / "go-path")
    names = {"sun": os.environ.get("SUN", str(ROOT / "build/sun")),
             "cxx": os.environ.get("CXX", "clang++"),
             "python": os.environ.get("PYTHON", "python3"),
             "node": "node", "bun": "bun", "cargo": "cargo",
             "rustc": "rustc", "go": "go", "nim": "nim"}
    tools = {}
    for name, command in names.items():
        found = shutil.which(command)
        if not found:
            parser.error(f"missing {command}; add its toolchain to PATH")
        tools[name] = found
    if args.cpu is not None and args.cpu not in os.sched_getaffinity(0):
        parser.error("requested CPU is outside the process affinity")
    commands = []

    def run(label, command, pin=False):
        """Capture a command's complete output, failing immediately on errors."""
        if pin and args.cpu is not None:
            command = ["taskset", "-c", str(args.cpu), *command]
        commands.append(command)
        print(f"{label}: {shlex.join(command)}", flush=True)
        result = subprocess.run(command, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / f"{label}.log").write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f"{label} failed; see {out / (label + '.log')}\n{result.stdout}")
        return result.stdout

    hashes = {name: hashlib.sha256((SOURCE / name).read_bytes()).hexdigest() for name in FILES}
    manifest = SOURCE / "rust/Cargo.toml"
    versions = {}
    for name in names:
        flag = "version" if name == "go" else "--version"
        versions[name] = run(f"version-{name}", [tools[name], flag]).strip()
    run("build-stdlib", [tools["sun"], "--emit-moon", "-o", "build/stdlib.moon",
                         "stdlib/stdlib.sun"])
    sun = out / "bench_sun"
    cpp = out / "bench_cpp"
    go = out / "bench_go"
    nim = out / "bench_nim"
    run("build-sun", [tools["sun"], "-c", "--dynamic", "-o", str(sun), str(SOURCE / "bench.sun")])
    run("build-cpp", [tools["cxx"], "-O3", "-std=c++20", "-o", str(cpp), str(SOURCE / "bench.cpp")])
    run("build-rust", [tools["cargo"], "build", "--release", "--locked", "--manifest-path", str(manifest)])
    run("build-go", [tools["go"], "build", "-o", str(go), str(SOURCE / "bench.go")])
    run("build-nim", [tools["nim"], "c", "-d:danger", "--hints:off",
                      f"--nimcache:{out / 'nim-cache'}", f"-o:{nim}", str(SOURCE / "bench.nim")])
    cases = [
        ("python", [tools["python"], str(SOURCE / "bench.py")], [("Python str(i)", "str(i)")]),
        ("node", [tools["node"], str(SOURCE / "bench.js")], [("Node.js String(i)", "String(i)")]),
        ("bun", [tools["bun"], str(SOURCE / "bench.js")], [("Bun String(i)", "String(i)")]),
        ("cpp", [str(cpp)], [("C++ std::to_string", "to_string(i)")]),
        ("rust", [str(out / "rust-target/release/strbench")],
         [("Rust to_string()", "i.to_string()"), ("Rust itoa", "itoa + to_owned()")]),
        ("go", [str(go)], [("Go strconv.Itoa", "strconv.Itoa(i)")]),
        ("nim", [str(nim)], [("Nim $i", "$i")]),
        ("sun", [str(sun)], [("Sun interpolation", "Sun interpolation:")]),
    ]
    rows = []
    for label, command, entries in cases:
        output = run(f"run-{label}", command, pin=True)
        print(output, flush=True)
        for name, prefix in entries:
            pattern = rf"^{re.escape(prefix)}\s+([0-9.]+) ns/string\s+([0-9.]+) M/s"
            match = re.search(pattern, output, re.MULTILINE)
            if not match:
                raise RuntimeError(f"missing result for {name}")
            rows.append({"language": name, "ns_per_string": float(match[1]),
                         "million_strings_per_second": float(match[2])})
        if label in {"cpp", "rust", "go", "nim", "sun"}:
            checks = re.findall(r"\(check (\d+)\)", output)
            if len(checks) != len(entries) or any(int(x) != 8192 for x in checks):
                raise RuntimeError(f"incorrect retained string lengths for {label}: {checks}")
    rows.sort(key=lambda row: row["ns_per_string"])
    metadata = {"measured_at_utc": datetime.now(timezone.utc).isoformat(),
                "upstream_commit": UPSTREAM, "benchmark_sha256": hashes,
                "sun_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                "sun_source_sha256": hashlib.sha256((SOURCE / "bench.sun").read_bytes()).hexdigest(),
                "implementation_sha256": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                                          for name in ["stdlib/string.sun", "include/codegen/codegen.h",
                                                       "src/driver/driver.cpp"]},
                "worktree_modified": bool(subprocess.check_output(["git", "diff", "--name-only"], text=True).strip()),
                "platform": platform.platform(), "cpu": args.cpu, "versions": versions,
                "commands": commands, "results": rows}
    if Path("/proc/cpuinfo").exists():
        metadata["cpu_model"] = next(line.split(":", 1)[1].strip() for line in
                                     Path("/proc/cpuinfo").read_text().splitlines()
                                     if line.startswith("model name"))
    (out / "results.json").write_text(json.dumps(metadata, indent=2) + "\n")
    lines = ["| Language | ns/string | Million strings/s |",
             "|---|---:|---:|"]
    for row in rows:
        lines.append(f"| {row['language']} | {row['ns_per_string']:.2f} | {row['million_strings_per_second']:.1f} |")
    (out / "results.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
