---
docs-page: abi/examples
docs-config: true
---
# Target compatibility

A library contains code for a particular target. The same tiny scalar call works
when the library and consumer target match. Importing the library built for the
other architecture must fail with a target diagnostic, before an executable is produced.

## Run

From the repository root:

```bash
bash examples/abi/target/build.sh
bash examples/abi/target/build-incompatible.sh
```

The build script builds `library.sun` twice: once for the host and once for a foreign
architecture. The consumers have the same call but select different libraries in their manifests. No cross linker is needed:
the foreign artifact is a `.moon` library and is never executed.

`sun-config.json` declares the successful build products and their library search
path. `build-incompatible.sh` intentionally fails and prints the compiler error. To check
both cases automatically, run `bash examples/abi/target/test.sh`.
