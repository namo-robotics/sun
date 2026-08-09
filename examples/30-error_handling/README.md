# Error Handling

Functions that can fail declare an error type with the `, IError` suffix and
signal failure with `throw`. Errors are real exceptions: a `throw` unwinds the
stack to the nearest matching `catch`. Any class implementing `IError` can be
thrown — here the standard library's `DivisionByZeroError`.

```bash
./build.sh
./main
```
