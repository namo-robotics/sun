/** Declares the allocation-free trace capture shared by current-stack and
 * exception reporting. */
#pragma once
#include "runtime/exceptions.h"

/** Captures bounded traces into caller-owned storage. */
namespace sun::runtime {
/** Captures frames from the requested caller into the trailing address
 * storage. */
void captureStackTrace(StackTraceHeader& trace,
                       std::uintptr_t callerReturnAddress) noexcept;
}  // namespace sun::runtime
