/** Reports unrecoverable failures without requiring a functioning allocator. */
#include "runtime/panic.h"

#include <cstdlib>
#include <unistd.h>

void sun_panic(const char* message, std::size_t length) noexcept {
  constexpr char prefix[] = "panic: ";
  (void)::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
  (void)::write(STDERR_FILENO, message, length);
  (void)::write(STDERR_FILENO, "\n", 1);
  std::abort();
}
