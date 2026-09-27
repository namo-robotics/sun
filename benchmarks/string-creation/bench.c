/** Measure decimal string creation with separately owned C strings. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *buf[1024];

/** Format each integer into a new allocation and release the replaced string. */
static void from_int(uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) {
    char text[32];
    int length = snprintf(text, sizeof(text), "%llu", (unsigned long long)i);
    if (length < 0 || (size_t)length >= sizeof(text)) abort();
    char *value = malloc((size_t)length + 1);
    if (!value) abort();
    memcpy(value, text, (size_t)length + 1);
    free(buf[i & 1023]);
    buf[i & 1023] = value;
  }
}

/** Validate every retained decimal value and return the total string length. */
static size_t verify(uint64_t n) {
  size_t total = 0;
  for (uint64_t slot = 0; slot < 1024; ++slot) {
    uint64_t expected = n - 1 - ((n - 1 - slot) & 1023);
    char text[32];
    snprintf(text, sizeof(text), "%llu", (unsigned long long)expected);
    if (strcmp(buf[slot], text) != 0) abort();
    total += strlen(buf[slot]);
  }
  return total;
}

/** Read a monotonic clock in nanoseconds, failing if the clock is unavailable. */
static double now_ns(void) {
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) abort();
  return (double)time.tv_sec * 1e9 + (double)time.tv_nsec;
}

/** Warm up, measure repeated conversions, validate results, and release strings. */
int main(void) {
  const uint64_t n = 100000000;
  from_int(1000000);
  verify(1000000);
  double best = 1e300;
  for (int run = 0; run < 5; ++run) {
    double start = now_ns();
    from_int(n);
    double elapsed = now_ns() - start;
    if (elapsed < best) best = elapsed;
    verify(n);
  }
  double ns = best / (double)n;
  printf("C (%s)\n", __VERSION__);
  printf("%-24s %7.2f ns/string  %8.1f M/s   (check %zu)\n",
         "snprintf(i)", ns, 1e3 / ns, verify(n));
  for (size_t slot = 0; slot < 1024; ++slot) free(buf[slot]);
  return 0;
}
