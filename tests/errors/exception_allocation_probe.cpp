/** Runs native exception allocation checks without linking the compiler or
 * LLVM. */
#include <cstdio>
#include <cstring>

#include "exception_allocation_support.h"

/** Enables allocator denial before the first exception or stack walk. */
int main(int argc, char** argv) {
  using namespace sun::tests::exceptions;
  if (argc == 2 && std::strcmp(argv[1], "--capture") == 0) {
    return checkCapture();
  }
  const bool concurrent = argc == 2 && std::strcmp(argv[1], "--threads") == 0;
  if (argc != 1 && !concurrent) {
    std::fputs("usage: sun_exception_allocation_probe [--capture|--threads]\n",
               stderr);
    return 64;
  }
  if (concurrent) {
    return checkConcurrentTransport(nativeFixture);
  }
  if (int result = checkTransport(nativeFixture)) {
    return result;
  }
  return checkNativeTraceBounds();
}
