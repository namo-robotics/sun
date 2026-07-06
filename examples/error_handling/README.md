# Error Handling

Functions that can fail declare an error type with the `, IError` suffix and
signal failure with `throw`. A `throw` unwinds to the nearest matching `catch`.

```bash
./build.sh
./main
```
