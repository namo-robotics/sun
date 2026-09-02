# Testing

Tests are declared with `test_function` and live either next to the code they
exercise — where module-scoped privacy lets them call private helpers — or in
test-only files listed under `test_files:` in the manifest. Assertions come
from `std.test`; a failure throws, so a fixture class's `deinit` (the
teardown) still runs when a test fails.

Compiling with `-c` produces two binaries: `main`, with every test stripped,
and `main_test`, which runs the tests — in parallel by default, or one after
another with `--test-sequential`. During development, `sun test main.sun`
runs them under the JIT without building anything.

## Build and run

```bash
./build.sh
./main            # the program; contains no test code
./main_test       # runs the 4 tests, exit 0 when all pass
sun test main.sun # same tests, JIT
```
