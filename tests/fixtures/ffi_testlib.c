// tests/fixtures/ffi_testlib.c
//
// Built as a shared library (sun_ffi_testlib) so the FFI tests can link
// against symbols that are NOT already present in the test binary. Testing
// against libc alone would pass even if -l/-L did nothing, because libc is
// already resident in the process.

int sun_ffi_triple(int x) { return x * 3; }

long long sun_ffi_sum(long long a, long long b) { return a + b; }

int sun_ffi_count_bytes(const unsigned char* s) {
  int n = 0;
  while (s && s[n]) n++;
  return n;
}
