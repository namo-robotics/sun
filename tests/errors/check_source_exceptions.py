"""Check native exception code generation and source formatting through the CLI."""
import pathlib
import subprocess
import sys
import tempfile


def check(command, expected=0):
    """Require the expected exit status and retain diagnostics on failure."""
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != expected:
        raise AssertionError(
            f"{command}: expected {expected}, got {result.returncode}\n"
            f"{result.stdout}{result.stderr}"
        )


def main():
    """Build independent native programs and inspect their generated IR."""
    compiler = str(pathlib.Path(sys.argv[1]).resolve())
    source = pathlib.Path(sys.argv[2]).resolve()
    pathlib.Path("tmp").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="source_exception_", dir="tmp") as temporary:
        directory = pathlib.Path(temporary).resolve()
        formatted = directory / "fixture.sun"
        formatted.write_text(source.read_text())
        check([compiler, "fmt", str(formatted)])
        check([compiler, "fmt", "--check", str(formatted)])
        for optimization in ([], ["-O0"]):
            check([compiler, *optimization, str(formatted)], 42)
            output = directory / "native"
            check([compiler, "--dynamic", "-c", *optimization, str(formatted), "-o", str(output)])
            check([str(output)], 42)
        ir_path = directory / "fixture.ll"
        check([compiler, "--emit-llvm", "-O0", str(formatted), "-o", str(ir_path)])
        ir = ir_path.read_text()
        for instruction in ("invoke ", "landingpad ", "@sun_eh_personality", "@sun_eh_push", "@sun_eh_pop"):
            if instruction not in ir:
                raise AssertionError(f"Missing native exception operation: {instruction}")
        if "__cxa_" in ir:
            raise AssertionError("Source exceptions must not use C++ exception allocation")


if __name__ == "__main__":
    main()
