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

struct SunFfiTS { long long sec; long long nsec; };

void sun_ffi_fill(struct SunFfiTS* t) {
  t->sec = 7;
  t->nsec = 9;
}

// By-value struct cases, one per System V classification outcome.
struct SunFfiPair { int a, b; };                  // 8B  -> one INTEGER reg
struct SunFfiMixed { int a; double b; };          // 16B -> INTEGER + SSE
struct SunFfiBig { int a, b, c, d, e; };          // 20B -> MEMORY (byval/sret)

int sun_ffi_take_pair(struct SunFfiPair p) { return p.a * 100 + p.b; }
int sun_ffi_take_mixed(struct SunFfiMixed m) { return m.a + (int)m.b; }
int sun_ffi_take_big(struct SunFfiBig b) {
  return b.a + b.b + b.c + b.d + b.e;
}
struct SunFfiPair sun_ffi_make_pair(int a, int b) {
  struct SunFfiPair p = {a, b};
  return p;
}
struct SunFfiBig sun_ffi_make_big(int base) {
  struct SunFfiBig b = {base, base + 1, base + 2, base + 3, base + 4};
  return b;
}
int sun_ffi_pair_then_int(struct SunFfiPair p, int extra) {
  return p.a + p.b + extra;
}
