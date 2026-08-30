---
docs-page: modules
---

# Path Variables

Manifest entries can reference variables with `$NAME` — in `source_files`, `libraries`
(paths and urls) and `protos` entries alike. Here the manifest asks for
`$LIBS/mathlib.moon`:

```sun
manifest {
    libraries: ["stdlib.moon", "$LIBS/mathlib.moon"]
}
```

This folder's `sun-config.json` supplies both the library search path (where
`stdlib.moon` is found) and the variable:

```json
{
    "sunPath": ["../../build"],
    "pathVariables": { "LIBS": "libs" },
    "root": true
}
```

The compiler merges every `sun-config.json` from the entrypoint's folder up
to the filesystem root: the nearest definition of a variable wins, search
dirs are searched nearest-first, and relative entries resolve against their
own config file's folder. So a workspace root can define `sunPath` and shared
variables once, while each subfolder adds or overrides only what is local to
it. A config with `"root": true` stops the upward search — this example uses
it to stay self-contained. Config definitions override variables supplied
from outside the folders — `--path-var` flags, the `sun.pathVariables`
editor setting, and the environment:

```bash
sun --path-var LIBS=libs --compile -o main main.sun   # without a config file
LIBS=libs sun --compile -o main main.sun              # environment works too
```

Using a variable that is defined nowhere is a compile error. Variables are
expanded before path resolution, so a variable can hold a relative directory —
which keeps a manifest portable when libraries live in a different place on
each machine.

## Build and run

```bash
./build.sh
./main
```
