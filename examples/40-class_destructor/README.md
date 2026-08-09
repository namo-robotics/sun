# Class Destructors

Sun runs a class's `deinit` method automatically when a value goes out of scope,
giving deterministic cleanup with no garbage collector. Here `foo` is destroyed
at the end of `main`.

```bash
./build.sh
./main
```
