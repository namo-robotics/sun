# GPU matrix example

Build with `-DSUN_ENABLE_CUDA=ON`, then run from the repository root:

```sh
build/sun --lib-path build -lcublas -lcudart -lpthread examples/110-gpu-matrices/main.sun
```

For toolkits outside the system library search path, add `-L` with the toolkit's
library directory and include that directory in `LD_LIBRARY_PATH`.

The example uploads `[[1, 2], [3, 4]]` twice, multiplies the GPU matrices with
`try (a * b)`, and downloads the product. The result is `[[7, 10], [15, 22]]`,
printed one element per line. CUDA errors produce a nonzero exit status.
