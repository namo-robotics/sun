# Error Handling

Functions that can fail declare `throws` and return ordinary values on success.
Use `exception` to define a failure type and `extends` to group related failures.
A `try` block with a family catch handles every descendant of that family.

## Build and run

```bash
./build.sh
./main
```
