---
docs-page: abi/examples
docs-config: true
---
# Compatibility across library revisions

`shapes_user.sun` is built against the `Point` declaration in `shapes_v1.sun`.
All three shapes revisions declare the persistent `library_id: "sun.examples/shapes"`.
The compiled shapes user accepts v1.1 after the compiler checks its nominal
identity and ABI contract, without rebuilding the shapes user.

| Library | Change | Accepted by the existing shapes user? |
| --- | --- | --- |
| `shapes_v1.sun` | Original single-field `Point` | Yes |
| `shapes_v1.1.sun` | Constructor initializes `x` to `43` instead of `42`; unchanged ABI contract | Yes |
| `shapes_v2.sun` | Adds a second field; changes the ABI contract | No |

Changing an ordinary constructor body changes behavior, but does not change the
ABI contract in this example. The shapes user reads `43` from a v1.1 point.
Compiling `main_v1.1.sun` warns that different artifacts of the same library are
in use. The main program uses v1.1, while `shapes_user.moon` keeps its embedded
v1 implementation; both remain independent.

The version names do not grant compatibility. Each artifact must declare the
same library identity and preserve the exported declarations, layouts, call and
ownership contracts, retained generic code, target, and compiler/runtime rules.
Matching names or layouts in an unrelated library are insufficient. Bundles
without revision requirements continue to require exact dependencies.

Nominal correspondence does not rewrite embedded implementations or linker
symbols. Code already compiled into the shapes user retains its original
implementation dependencies. Missing dependencies and ambiguous artifact selections
are rejected before semantic import.

## Run

From the repository root:

```bash
bash examples/abi/identity/build.sh
bash examples/abi/identity/test.sh
```

Build scripts compile from the example folder and write libraries and executables
into that same folder.

`sun-config.json` builds all three shapes libraries and `shapes_user.moon` once,
then builds `main_v1.1.sun` against v1.1. The executable returns success.
`test.sh` runs it and checks that
`main_v2.sun`, which substitutes v2, fails with a library ABI mismatch.

Run `bash examples/abi/identity/build-incompatible.sh` separately to inspect the
v2 rejection.

Build progress is hidden at the default `warn` log level. Use
`SUN_LOG_LEVEL=info bash examples/abi/identity/build.sh` to show it. Building the
v2 library alone is valid; the ABI error occurs when `build-incompatible.sh`
tries to use it with the existing shapes user.

Remove the generated libraries, executables, and test log with:

```bash
bash examples/abi/identity/clean.sh
```
