# GPU matrix example

Build with `-DSUN_ENABLE_CUDA=ON`, then run from the repository root:

```sh
build/sun --lib-path build -lcublas -lcudart -lpthread examples/110-gpu-matrices/main.sun
```

For toolkits outside the system library search path, add `-L` with the toolkit's
library directory and include that directory in `LD_LIBRARY_PATH`.

The example uploads two host matrices, uses allocating arithmetic operators,
reuses an output, and explicitly downloads the result. Expected output is `7`
and `22`. CUDA errors produce a nonzero exit status.
