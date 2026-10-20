---
docs-page: abi/examples
docs-config: true
---
# Exact dependency identity

`bridge.moon` was compiled against the original `shapes.Point`. Supplying that
exact library works. Replacing it with another library must fail, even though
both versions have one `i32` field and the same public names.

Only the constructor changes from `42` to `43`. This creates a different bundle
and declaration identity; it does not change the storage layout. Sun reports a
missing exact dependency rather than silently substituting the new type.

## Run

From the repository root:

```bash
bash examples/abi/identity/build.sh
bash examples/abi/identity/test.sh
```

The build script builds the bridge with `original.moon`. `main.sun` selects the bridge plus `original.moon` (passes);
`incompatible.sun` selects the bridge plus `replacement.moon` (fails).
The successful program verifies the imported result is `42`.

`sun-config.json` declares the successful build products and their library search
path. The test script checks the deliberately failing consumer separately.
