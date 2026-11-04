# Compiler and tooling tests

The C++ suites use GoogleTest in `sun_tests`. Suite names follow the directory
structure so a filter such as `Classes*` selects related tests. Standard-library
and TLS tests are separate Sun suites.

## Choosing what to exercise

Use the earliest stage that actually proves the behavior:

- Call a pure helper directly for type rules and other isolated computations.
- Use the lexer or parser for token and syntax behavior.
- Use `sun::driver::analyzeString` from `driver/execution_utils.h` for acceptance
  and diagnostics that require semantic analysis. It throws reported analysis
  errors and creates a fresh driver for each call. It does **not** run borrow
  checking, code generation, linking, or execution.
- Use `compileString` for borrow checking and code generation.
- Use `executeString` when the result or runtime side effects are the assertion.
- Use file-based or command-line fixtures when imports, artifacts, linking,
  caching, or command-line behavior are part of the test.

Only preload the standard library when the source needs it. Do not replace an
execution test with analysis simply because its program is accepted. Generic
declarations and instantiated generic bodies also exercise different paths.

## Writing readable cases

Give each test one clear purpose. Keep related assertions together, but split
independent failure scenarios into separately named tests. Prefer short,
complete Sun examples and direct assertions over generated source or a custom
test language. Format embedded Sun source like ordinary source and document
definitions as required by `AGENTS.md`.

Keep setup local unless a small helper removes distracting repetition. Helpers
should describe the operation they perform and preserve useful failure output.
For simple input/output tables, name each case and use `SCOPED_TRACE` so a failure
identifies the input. Do not share mutable drivers or compilation sessions
between tests to save time.

Before removing a test, identify the surviving case and compare its source,
assertions, compiler stage, and setup. Preserve boundary values, historical
regression triggers, and distinct import or compilation paths. Similar-looking
programs are not necessarily redundant. Record removals in `cleanup.md`.

## Building and running

Run commands from the repository root, using at most half the available cores:

```sh
cmake --build build -j$(($(nproc)/2))
SUN_PATH=$PWD ctest --test-dir build -j$(($(nproc)/2)) --output-on-failure
```

For a selected group or a single test:

```sh
SUN_PATH=$PWD ctest --test-dir build -j$(($(nproc)/2)) -R '^Classes' --output-on-failure
SUN_PATH=$PWD build/tests/sun_tests --gtest_filter=Classes.simple_class_definition
```

Never run `sun_tests` without a filter: use CTest for the complete parallel run.
The default `SUN_TEST_SHARDS=1` discovers individual tests for CTest and IDEs.
Larger shard counts retain separate processes for the exclusions defined in
`tests/CMakeLists.txt`; preserve those exclusions when changing suite names.
For local throughput, the existing sharding option reduces process startup
overhead while running every enabled test:

```sh
cmake -S . -B build -DSUN_TEST_SHARDS=$(($(nproc)/2))
```

Set `-DSUN_TEST_SHARDS=1` again to restore individual CTest entries. GoogleTest
filters on `build/tests/sun_tests` remain available in either configuration.

For performance comparisons, use the same build configuration, compiler-cache
setting, and parallelism. Finish building before running tests. Measure test
object rebuilding separately from compiler and dependency building, and compare
both suite runtime and affected test timings. Store local logs under `tmp`.
