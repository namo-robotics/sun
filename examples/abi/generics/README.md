---
docs-page: abi/examples
docs-config: true
---
# Retained generic bodies

A `.moon` library retains the body of `read<T>`. The consumer can specialize it
for a type that did not exist when the library was built.

`compatible.sun` implements the imported `Value` interface and runs successfully.
`incompatible.sun` does not implement it and must fail the generic constraint
check, with a diagnostic naming `Value`.

This failure is a semantic incompatibility in the new type argument, not rejection
of the library's binary contract: the same library is supported in both attempts.

## Run

From the repository root:

```bash
bash examples/abi/generics/build.sh
bash examples/abi/generics/test.sh
```

The scripts build `reader.moon` once, then try both consumers. The successful
program verifies the imported generic body returns `42`.

`sun-config.json` declares the successful build products and their library search
path. The test script checks the deliberately failing consumer separately.
