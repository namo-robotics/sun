/** Checks validation paths that do not require a GPU or a functioning driver. */
#include <stdint.h>
#include <stdio.h>

/** Validates rank, extents, and precision before CUDA allocation. */
int sun_cuda_validate_shape(int rank, int64_t rows, int64_t columns, int width);
/** Checks boundary values without invoking CUDA. */
int main(void) {
  if (sun_cuda_validate_shape(2, 3, 7, 4) != 0 ||
      sun_cuda_validate_shape(1, 7, 1, 8) != 0 ||
      sun_cuda_validate_shape(0, 1, 1, 4) != -1 ||
      sun_cuda_validate_shape(3, 1, 1, 4) != -1 ||
      sun_cuda_validate_shape(1, 1, 2, 4) != -1 ||
      sun_cuda_validate_shape(2, -1, 2, 4) != -1 ||
      sun_cuda_validate_shape(2, 0, 2, 4) != -1 ||
      sun_cuda_validate_shape(2, 1, 2, 2) != -1 ||
      sun_cuda_validate_shape(2, INT64_MAX, INT64_MAX, 8) != -2 ||
      sun_cuda_validate_shape(2, 65536, 65536, 8) != -2) {
    fputs("CUDA shape validation failed\n", stderr);
    return 1;
  }
  return 0;
}
