# Error Handling

Functions that can fail return an enum. The first variant holds success;
other variants own concrete errors. Use `try expression` to propagate a failure,
or `match` to handle it. One typed interface arm can handle several error
variants without erasing the owned payload types.

## Build and run

```bash
./build.sh
./main
```
