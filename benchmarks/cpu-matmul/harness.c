/** Shared input generation, timing, and validation for the CPU GEMM benchmark. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef SUN_DRIVER
#include <cblas.h>
#endif

static int64_t arguments[6];

/** Reads M, K, N, repetitions, samples, and warmups; bounds sizes before allocation. */
int32_t bench_configure(int32_t argc, char **argv) {
  if (argc != 7) return 1;
  for (int i = 0; i < 6; ++i) {
    char *end;
    errno = 0;
    arguments[i] = strtoll(argv[i + 1], &end, 10);
    if (errno || *end || end == argv[i + 1] || arguments[i] < 1 ||
        arguments[i] > (i < 3 ? 8192 : 1000000)) return 1;
  }
  return 0;
}

/** Returns one validated argument, for the Sun entrypoint. */
int64_t bench_arg(int32_t index) { return arguments[index]; }

/** Creates identical, exactly representable inputs with mixed signs in both drivers. */
void bench_fill(float *a, float *b) {
  int64_t m = arguments[0], k = arguments[1], n = arguments[2];
  for (int64_t i = 0; i < m; ++i)
    for (int64_t p = 0; p < k; ++p)
      a[i * k + p] = (float)((i % 7 - 3) * 0.25 + (p % 5 - 2) * 0.125);
  for (int64_t p = 0; p < k; ++p)
    for (int64_t j = 0; j < n; ++j)
      b[p * n + j] = (float)((p % 11 - 5) * 0.0625 + (j % 13 - 6) * 0.125);
}

/** Returns monotonic nanoseconds; clock failure aborts the benchmark. */
int64_t bench_now_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) { perror("clock_gettime"); exit(1); }
  return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

/** Keeps each product observable without reading the result inside the timed loop. */
__attribute__((noinline)) void bench_barrier(const float *c) {
  __asm__ __volatile__("" : : "r"(c) : "memory");
}

/** Checks every element against an independent double-precision algebraic reference. */
int32_t bench_validate(const float *c) {
  int64_t m = arguments[0], k = arguments[1], n = arguments[2];
  double sum_bp = 0, sum_ap = 0, sum_ap_bp = 0, checksum = 0;
  for (int64_t p = 0; p < k; ++p) {
    double ap = (p % 5 - 2) * 0.125, bp = (p % 11 - 5) * 0.0625;
    sum_ap += ap;
    sum_bp += bp;
    sum_ap_bp += ap * bp;
  }
  for (int64_t i = 0; i < m; ++i) {
    double ai = (i % 7 - 3) * 0.25;
    for (int64_t j = 0; j < n; ++j) {
      double bj = (j % 13 - 6) * 0.125;
      double expected = ai * sum_bp + ai * bj * k + sum_ap_bp + bj * sum_ap;
      double actual = c[i * n + j];
      if (!isfinite(actual) || fabs(actual - expected) > 1e-4 * (1 + fabs(expected))) {
        fprintf(stderr, "validation failed at (%" PRId64 ",%" PRId64 "): %.9g != %.9g\n",
                i, j, actual, expected);
        return 1;
      }
      checksum += actual;
    }
  }
  printf("checksum %.17g\n", checksum);
  return 0;
}

/** Prints elapsed nanoseconds for a batch; division by repetitions happens in Python. */
void bench_report(int64_t elapsed) { printf("sample %" PRId64 "\n", elapsed); }

#ifndef SUN_DRIVER
/** Runs single-threaded OpenBLAS with allocations and validation outside each timer. */
int main(int argc, char **argv) {
  if (bench_configure(argc, argv)) return 1;
  openblas_set_num_threads(1);
  if (openblas_get_num_threads() != 1) return 1;
  printf("backend %s; core=%s; threads=%d\n", openblas_get_config(),
         openblas_get_corename(), openblas_get_num_threads());
  int m = (int)arguments[0], k = (int)arguments[1], n = (int)arguments[2];
  float *a = malloc((size_t)m * k * sizeof(float));
  float *b = malloc((size_t)k * n * sizeof(float));
  float *c = malloc((size_t)m * n * sizeof(float));
  if (!a || !b || !c) { fprintf(stderr, "allocation failed\n"); return 1; }
  bench_fill(a, b);
  for (int64_t w = 0; w < arguments[5]; ++w)
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, m, n, k,
                1.0f, a, k, b, n, 0.0f, c, n);
  if (bench_validate(c)) return 1;
  for (int64_t s = 0; s < arguments[4]; ++s) {
    int64_t start = bench_now_ns();
    for (int64_t r = 0; r < arguments[3]; ++r) {
      cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, m, n, k,
                  1.0f, a, k, b, n, 0.0f, c, n);
      bench_barrier(c);
    }
    int64_t elapsed = bench_now_ns() - start;
    if (bench_validate(c)) return 1;
    bench_report(elapsed);
  }
  free(c); free(b); free(a);
  return 0;
}
#endif
