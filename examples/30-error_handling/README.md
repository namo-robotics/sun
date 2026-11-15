# Error Handling

`foo()` throws A while owning `bar`. During unwinding, `bar`'s destructor
throws and handles B before `main()` catches A.

See [Error Handling](../../docs/pages/errors.mdx#nested-exceptions-and-stack-traces)
for the walkthrough and stack-trace memory layout.

## Build and run

From this example directory:

```bash
./build.sh
./main
```

The program prints nothing and exits with status 1 because `main()` catches A.
This is the expected result.
