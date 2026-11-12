---
docs-page: abi/examples
docs-config: true
---
# Independent library revisions

`shapes_user.moon` embeds the `Point` type and implementation from `shapes_v1.sun`.
An application can list another shapes revision without replacing that embedded
copy. The build script compiles each revision directly with
`--id sun.examples/shapes`, using distinct output paths. This identifies the
library but does not make its revisions interchangeable. The script then builds
the configured bridge and consumer, whose IDs come from their entrypoint names.

`main_v1.1.sun` constructs a v1.1 point containing `43` and passes it to
`shapes_user.read`, compiled against v1. Their recorded ABI contracts match, so
the borrow is accepted. The reader retains its original declarations and code;
its `local()` function still constructs a v1 point containing `42`.

`main_v2.sun` attempts the same call with a layout-changing v2 point. Type checking
rejects that argument. Both versions may still be used independently.

## Run

From the repository root:

```bash
bash examples/abi/identity/build.sh
bash examples/abi/identity/test.sh
```

The scripts build libraries and executables under the example's `.sun/build/`
directory and use `sun describe --config` to resolve their paths. `test.sh`
runs the compatible-borrow example and checks the cross-revision rejection.
Use `build-incompatible.sh` to inspect the rejected call separately, and
`clean.sh` to remove generated outputs.
