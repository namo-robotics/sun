# Debug artifacts for a library and executable

This example builds a tiny Moon library exposing `life.answer()` and an
executable that calls it. The executable exits with status zero when the
library returns the expected value. It needs no standard library.

The `sun-config.json` declares both build products, so one `sun -c` invocation
builds the library bundle and then the executable:

```json filename="sun-config.json"
{
  "root": true,
  "sun_path": ["."],
  "entrypoints": [
    { "path": "life.sun", "type": "library", "output_name": "life.moon" },
    { "path": "main.sun", "type": "binary", "output_name": "main" }
  ]
}
```

Run from the repository root:

```bash
SUN_BIN=./build/sun bash examples/65-debug/build.sh
./examples/65-debug/main
python3 -m json.tool examples/65-debug/life_debug/moon.json
```

If `sun` is on your PATH, omit `SUN_BIN=./build/sun`. The script runs
`sun -c --debug --no-test sun-config.json`; `--debug` and `--no-test` are
build-mode flags rather than project settings, so they stay on the command
line.

Both builds use `--debug` and produce `ast.dot`, `scope_tree.html`, and `ir.ll`
in `life_debug/` and `main_debug/`. Open the HTML files in a browser to
inspect scopes, or read `ir.ll` for the compiled code.

The library also produces `life_debug/moon.json`, containing its exported
metadata, format version, and locations of binary payloads. Find `life` in
`modules[].metadata.module_name` and `answer` in the module's `functions`.
Declaration keys are opaque strings. Source keys are generated as
`$<hash>$_<ordinal>`; specialized declarations use deterministic hashes.
Binary payload bytes are omitted.
The `.moon` file remains the binary bundle used by the executable.
