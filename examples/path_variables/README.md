---
docs-page: modules
---

# Path Variables

Manifest entries can reference variables with `$NAME` — in `suns`, `moons`
(paths and urls) and `protos` entries alike. Here the manifest asks for
`$LIBS/mathlib.moon`:

```sun
manifest {
    moons: ["stdlib.moon", "$LIBS/mathlib.moon"]
}
```

This folder's `sun-config.json` supplies both the library search path (where
`stdlib.moon` is found) and the variable:

```json
{
    "sunPath": ["../../build"],
    "pathVariables": { "LIBS": "libs" }
}
```

The compiler uses the nearest `sun-config.json` in the entrypoint's folder or
any parent; relative entries resolve against the config file's folder. Its
definitions override variables supplied from outside the folder — `--path-var`
flags, the `sun.pathVariables` editor setting, and the environment:

```bash
sun --path-var LIBS=libs --compile -o main main.sun   # without a config file
LIBS=libs sun --compile -o main main.sun              # environment works too
```

Using a variable that is defined nowhere is a compile error. Variables are
expanded before path resolution, so a variable can hold a relative directory —
which keeps a manifest portable when libraries live in a different place on
each machine.

```bash
./build.sh
./main
```
