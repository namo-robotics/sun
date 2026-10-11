"""Test native C binding generation against real target layouts."""

from pathlib import Path
import os
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class BindgenTests(unittest.TestCase):
    """Check public generator behavior with isolated headers and output files."""

    def setUp(self):
        """Keep test artifacts inside the workspace temporary directory."""
        (ROOT / "tmp").mkdir(exist_ok=True)
        directory = tempfile.TemporaryDirectory(dir=ROOT / "tmp", prefix="bindgen-")
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name)
        self.compiler = Path(os.environ.get("SUN_BINDGEN_COMPILER", ROOT / "build/sun"))
        (self.path / "sun-config.json").write_text('{"root": true}')

    def test_darwin_target_versions_and_architectures(self):
        """Allow macOS version aliases while rejecting a different target ABI."""
        for arch in ("arm64", "x86_64"):
            source = self.path / "binding.sun"
            source.write_text((ROOT / f"stdlib/bindings/process_darwin_{arch}.sun").read_text())
            for target in (f"{arch}-apple-darwin25.0.0", f"{arch}-apple-macosx14.0"):
                result = self.compile(source, target)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            other = "x86_64" if arch == "arm64" else "arm64"
            for target in (f"{other}-apple-darwin", f"{arch}-unknown-linux-gnu",
                           f"{arch}-apple-ios17.0", f"{arch}-apple-macosx14.0-simulator",
                           f"{arch}-unknown-darwin",
                           "arm64e-apple-darwin" if arch == "arm64" else "x86_64h-apple-darwin"):
                result = self.compile(source, target)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("C binding target mismatch", result.stdout + result.stderr)
        source.write_text('_assert_c_target("arm64-apple-darwin");\n')
        result = self.compile(source, "aarch64-apple-macosx14.0")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def generate(self, header, names, target="x86_64-unknown-linux-gnu"):
        """Run the generator with an explicit target and declaration selection."""
        source = self.path / "input.h"
        source.write_text(header)
        output = self.path / "bindings.sun"
        command = ["bindgen", str(source), "--target", target,
                   "--module", "native", "--output", str(output)]
        for name in names:
            command += ["--allow", name]
        result = self.run_sun(*command)
        return result, output

    def compile(self, source, target):
        """Compile target bindings without requiring a target linker or runner."""
        return self.run_sun("--target", target, "--emit-obj",
                            "-o", self.path / "bindings.o", source)

    def test_supported_targets(self):
        """Check fixture declarations on supported OS and architecture pairs."""
        header = (ROOT / "tests/fixtures/ffi_alignment.h").read_text()
        for target in ("x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu",
                       "x86_64-apple-darwin", "arm64-apple-darwin"):
            with self.subTest(target=target):
                result, output = self.generate(header, ["SunFfiAlignedOuter", "SunFfiReading",
                    "sun_ffi_aligned_make", "sun_ffi_aligned_take"], target)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("alignment: 64", output.read_text())
                compiled = self.compile(output, target)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)

    def test_command_help_errors_and_stdout(self):
        """Expose help, argument errors, and generated stdout through sun bindgen."""
        result = self.run_sun("bindgen", "--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("usage: sun bindgen", result.stdout)
        self.assertIn("--allow", result.stdout)
        result = self.run_sun("bindgen")
        self.assertEqual(result.returncode, 2)
        self.assertIn("required", result.stderr)
        self.assertEqual(result.stdout, "")
        header = self.path / "header with spaces.h"
        header.write_text("struct Reading { int value; };\n")
        result = self.run_sun("bindgen", header, "--target", "aarch64-unknown-linux-gnu",
                              "--module", "native", "--allow", "Reading", "--output", "-")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("public class Reading", result.stdout)
        self.assertIn("_assert_c_layout<Reading>", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_typedefs_arrays_and_self_pointers(self):
        """Resolve canonical types and dependencies without Sun typedef syntax."""
        result, output = self.generate("""
            typedef unsigned long count_t;
            struct Item { count_t count; struct Item *next; int values[3]; };
            int consume(const struct Item *item, count_t count);
        """, ["consume"])
        self.assertEqual(result.returncode, 0, result.stderr)
        text = output.read_text()
        self.assertIn("count: u64", text)
        self.assertIn("raw_ptr<Item>", text)
        self.assertIn("array<i32, 3>", text)
        compiled = self.compile(output, "x86_64-unknown-linux-gnu")
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)

    def test_generation_without_external_programs(self):
        """Generate bindings with no interpreter or compiler executable on PATH."""
        header = self.path / "native.h"
        header.write_text("struct Reading { int value; };\n")
        result = self.run_sun(
            "bindgen", header, "--target=x86_64-unknown-linux-gnu",
            "--module=native", "--allow=Reading", "--output=-", env={"PATH": ""})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("public class Reading", result.stdout)
        self.assertIn("_assert_c_layout<Reading>(4, 4, 0)", result.stdout)

    def test_rejections(self):
        """Unsupported selected constructs fail with header locations and no output."""
        for header in (
            "union Bad { int a; float b; };",
            "struct Bad { int a: 3; };",
            "struct __attribute__((packed)) Bad { char a; int b; };",
            "struct Bad { _Alignas(32) int a; };",
            "int Bad(int count, ...);",
            "int Bad();",
            "int Bad(int (*callback)(int));",
            "struct Bad { long double value; };",
            "struct Bad { int size; int values[]; };",
            "struct Bad { int ptr; int c_ptr; };",
        ):
            with self.subTest(header=header):
                result, output = self.generate(header, ["Bad"])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("input.h", result.stderr)
                self.assertFalse(output.exists())

    def test_reserved_names(self):
        """Escape compiler keywords in records, fields, and foreign functions."""
        result, output = self.generate("""
            struct class {
                int ptr; int static_ptr; int test_function; int throws;
                int extends; int this; int def;
            };
            int test_function(struct class value);
        """, ["class", "test_function"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('as "test_function"', output.read_text())
        compiled = self.compile(output, "x86_64-unknown-linux-gnu")
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)

    def test_determinism_and_selection(self):
        """Emit formatted, stable output and ignore unrelated declarations."""
        header = "union Other { int x; float y; }; struct A { int value; }; int b(struct A value);"
        result, output = self.generate(header, ["A", "b"])
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = output.read_text()
        self.assertTrue(expected.startswith(
            "/** Auto-generated by sun bindgen. Do not edit manually. */\n\n"))
        self.assertNotIn("Other", expected)
        formatted = self.run_sun("fmt", str(output))
        self.assertEqual(formatted.returncode, 0, formatted.stdout + formatted.stderr)
        self.assertEqual(output.read_text(), expected)
        result, output = self.generate(header, ["b", "A"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_text(), expected)

    def test_layout_and_target_mismatches(self):
        """Compilation rejects edits to a binding and use with a different target."""
        result, output = self.generate("struct A { char tag; int value; };", ["A"])
        self.assertEqual(result.returncode, 0, result.stderr)
        compiled = self.compile(output, "aarch64-unknown-linux-gnu")
        self.assertNotEqual(compiled.returncode, 0)
        self.assertIn("C binding target mismatch", compiled.stdout + compiled.stderr)
        output.write_text(output.read_text().replace("value: i32", "value: i64"))
        compiled = self.compile(output, "x86_64-unknown-linux-gnu")
        self.assertNotEqual(compiled.returncode, 0)
        self.assertIn("C layout mismatch", compiled.stdout + compiled.stderr)

    def test_adjusted_parameters_and_link_names(self):
        """Use C's adjusted pointer parameter type and preserve assembler names."""
        result, output = self.generate(
            'int consume(int data[3]) __asm__("native_consume");', ["consume"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('arg0: raw_ptr<i32>', output.read_text())
        self.assertIn('as "native_consume"', output.read_text())

    def run_sun(self, *arguments, env=None):
        """Run the compiler with optional environment overrides and captured output."""
        environment = {**os.environ, "SUN_PATH": str(ROOT), **(env or {})}
        return subprocess.run(
            [str(self.compiler), *map(str, arguments)],
            cwd=ROOT, env=environment, capture_output=True, text=True)

    def configure_binding(self, binding):
        """Write an isolated project whose configured module is called native."""
        if "header" in binding:
            binding.setdefault("output", "generated/native.sun")
        for branch in binding.get("target", {}).values():
            branch.setdefault("output", "generated/native.sun")
        (self.path / "sun-config.json").write_text(json.dumps({
            "root": True, "c_bindings": {"native": binding}}))

    def test_config_compilation_and_nested_header_changes(self):
        """Write manifest inputs and invalidate object caching when an include changes."""
        includes = self.path / "include space"
        includes.mkdir()
        nested = includes / "reading.h"
        nested.write_text("struct Reading { int value; };\n")
        (self.path / "sensor.h").write_text("#include <reading.h>\n")
        self.configure_binding({"header": "sensor.h", "allow": ["Reading"],
                                "clang_args": ["-Iinclude space"]})
        source = self.path / "main.sun"
        source.write_text('manifest { source_files: ["generated/native.sun"] }\n'
                          "/** Reports the native record size. */\n"
                          "function main() i32 { return _convert<i32>(_sizeof<native.Reading>()); }\n")
        obj = self.path / "main.o"
        result = self.run_sun("--emit-obj", "-o", obj, source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        before = obj.read_bytes()
        stamp = obj.stat().st_mtime_ns
        generated = self.path / "generated/native.sun"
        generated_stamp = generated.stat().st_mtime_ns
        result = self.run_sun("--emit-obj", "-o", obj, source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(obj.stat().st_mtime_ns, stamp)
        self.assertEqual(generated.stat().st_mtime_ns, generated_stamp)
        nested.write_text("struct Reading { long long value[3]; };\n")
        result = self.run_sun("--emit-obj", "-o", obj, source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotEqual(obj.read_bytes(), before)
        result = self.run_sun(source)
        self.assertEqual(result.returncode, 24, result.stdout + result.stderr)
        self.assertEqual(list(self.path.glob("*.sun")), [source])
        (self.path / "sun-config.json").write_text('{"root": true}')
        (self.path / "sensor.h").unlink()
        result = self.run_sun("--emit-obj", "--force-rebuild", "-o", obj, source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_config_bundle_and_target_selection(self):
        """Bundle explicitly listed generated files selected for the compilation target."""
        (self.path / "native.h").write_text("struct Reading { long value; };\n")
        self.configure_binding({"target": {
            "aarch64-unknown-linux-gnu": {"header": "native.h", "allow": ["Reading"]}}})
        source = self.path / "library.sun"
        source.write_text('manifest { source_files: ["generated/native.sun"] }\n'
                          "/** Exposes a native size. */\npublic module wrapper {\n"
                          "/** Returns the record size. */\n"
                          "public function size() i64 { return _sizeof<native.Reading>(); }\n}\n")
        result = self.run_sun("--target", "aarch64-unknown-linux-gnu", "--emit-moon",
                              "-o", self.path / "native.moon", source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.compile(source, "x86_64-unknown-linux-gnu")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("C binding generation failed", result.stdout + result.stderr)
        consumer = self.path / "consumer"
        consumer.mkdir()
        (consumer / "sun-config.json").write_text('{"root": true}')
        (self.path / "native.h").unlink()
        imported = consumer / "main.sun"
        imported.write_text('manifest { libraries: ["../native.moon"] }\n'
                            '/** Uses bindings bundled without their header. */\n'
                            'function main() i32 { return _convert<i32>(wrapper.size()); }\n')
        result = self.run_sun("--target", "aarch64-unknown-linux-gnu", "--emit-obj",
                              "-o", consumer / "main.o", imported)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_config_generator_diagnostics(self):
        """Surface header errors and reject malformed config without an artifact."""
        source = self.path / "main.sun"
        source.write_text("/** Exits successfully. */\nfunction main() i32 { return 0; }\n")
        header = self.path / "native.h"
        header.write_text("union Bad { int x; float y; };\n")
        self.configure_binding({"header": "native.h", "allow": ["Bad"]})
        result = self.run_sun("--emit-obj", "-o", self.path / "bad.o", source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("native.h", result.stdout + result.stderr)
        self.assertFalse((self.path / "bad.o").exists())
        for binding in ({"header": "native.h", "allow": []},
                        {"header": "native.h", "allow": ["Bad"], "typo": True}):
            self.configure_binding(binding)
            result = self.run_sun("--emit-obj", "-o", self.path / "bad.o", source)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("sun-config.json", result.stdout + result.stderr)

    def test_config_inheritance_and_test_command(self):
        """Resolve config paths at their origin and use bindings in Sun tests."""
        (self.path / "native.h").write_text("struct Reading { int value; };\n")
        self.configure_binding({"header": "native.h", "allow": ["Reading"]})
        child = self.path / "src"
        child.mkdir()
        source = child / "tests.sun"
        source.write_text('manifest { source_files: ["../generated/native.sun"] }\n'
                          "/** Checks generated bindings in the test runner. */\n"
                          "test_function reading_size() { var size = _sizeof<native.Reading>(); }\n")
        result = self.run_sun("test", "--moon", self.compiler.parent / "stdlib.moon", source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (child / "native.h").write_text("struct Other { int value; };\n")
        (child / "sun-config.json").write_text(json.dumps({"c_bindings": {
            "native": {"header": "native.h", "output": "../generated/native.sun", "allow": ["Other"]}}}))
        source.write_text('manifest { source_files: ["../generated/native.sun"] }\n'
                          "/** Uses the nearest config's module definition. */\n"
                          "function main() i32 { return _convert<i32>(_sizeof<native.Other>()); }\n")
        result = self.run_sun(source)
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)

    def test_generated_file_requires_manifest_entry(self):
        """Generation alone does not import a module; failures preserve prior output."""
        header = self.path / "native.h"
        header.write_text("struct Reading { int value; };\n")
        self.configure_binding({"header": "native.h", "allow": ["Reading"]})
        source = self.path / "main.sun"
        source.write_text("/** Refers to a module not listed in the manifest. */\n"
                          "function main() i32 { return _convert<i32>(_sizeof<native.Reading>()); }\n")
        result = self.run_sun("--emit-obj", "-o", self.path / "main.o", source)
        self.assertNotEqual(result.returncode, 0)
        generated = self.path / "generated/native.sun"
        self.assertTrue(generated.exists())
        original = generated.read_bytes()
        header.write_text("union Reading { int value; float other; };\n")
        result = self.run_sun("--emit-obj", "-o", self.path / "main.o", source)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(generated.read_bytes(), original)

    def test_config_requires_unique_explicit_outputs(self):
        """Reject missing outputs and output collisions before writing bindings."""
        (self.path / "native.h").write_text("struct Reading { int value; };\n")
        source = self.path / "main.sun"
        source.write_text("/** Exits successfully. */\nfunction main() i32 { return 0; }\n")
        binding = {"header": "native.h", "allow": ["Reading"]}
        config = self.path / "sun-config.json"
        config.write_text(json.dumps({"root": True, "c_bindings": {"a": binding}}))
        result = self.run_sun("--emit-obj", "-o", self.path / "main.o", source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("require header, output and allow", result.stdout + result.stderr)
        binding["output"] = "generated/shared.sun"
        config.write_text(json.dumps({"root": True, "c_bindings": {"a": binding, "b": binding}}))
        result = self.run_sun("--emit-obj", "-o", self.path / "main.o", source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("share the same output", result.stdout + result.stderr)
        self.assertFalse((self.path / "generated/shared.sun").exists())

if __name__ == "__main__":
    unittest.main()
