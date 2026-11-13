/** Declares allocation-free termination shared by generated code and runtimes. */
#pragma once

#include <cstddef>

extern "C" {
/** Writes the supplied bytes to stderr and aborts without allocating or unwinding.
 * The message must remain readable for the duration of this call.
 */
[[noreturn]] void sun_panic(const char* message, std::size_t length) noexcept;
}
