---
docs-page: abi/examples
docs-config: true
---
# Exact dependency identity

`shapes_user.sun` is built against the `Point` declaration in `shapes_v1.sun`.
Its compiled library requires that exact dependency.

| Library | Change | Accepted by the existing shapes user? |
| --- | --- | --- |
| `shapes_v1.sun` | Original single-field `Point` | Yes |
| `shapes_v1.1.sun` | Constructor returns `43` instead of `42`; same layout | No |
| `shapes_v2.sun` | Adds a second field | No |

The version names describe the examples; they do not grant compatibility.
The current exact-dependency check rejects both replacements. Even matching
layouts do not make separately built Sun declarations interchangeable.

## Run

From the repository root:

```bash
bash examples/abi/identity/build.sh
bash examples/abi/identity/build-incompatible.sh
```

`build-incompatible.sh` compiles `main_incompatible.sun` and prints the expected error.

`sun-config.json` builds all three shapes libraries, `shapes_user.moon`, and
`main.sun`, which uses v1 and returns success. `main_incompatible.sun` tries
v2 with the same compiled shapes user. The v1.1 library is included for comparison.

Run `bash examples/abi/identity/test.sh` to check both consumers automatically.
