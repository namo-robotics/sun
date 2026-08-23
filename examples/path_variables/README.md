---
docs-page: modules
---

# Path Variables

Manifest entries can reference variables with `$NAME` — in `suns`, `moons`
(paths and urls) and `protos` entries alike. Here the manifest asks for
`$LIBS/mathlib.moon`, and the build supplies the directory:

```sun
manifest {
    moons: ["stdlib.moon", "$LIBS/mathlib.moon"]
}
```

```bash
sun --path-var LIBS=libs --compile -o main main.sun
```

A variable not defined with `--path-var` falls back to the environment, so
this works too:

```bash
LIBS=libs sun --compile -o main main.sun
```

Using a variable that is defined in neither place is a compile error.
Variables are expanded before path resolution, so a variable can hold a
relative directory — which keeps a manifest portable when libraries live
in a different place on each machine.

```bash
./build.sh
./main
```
