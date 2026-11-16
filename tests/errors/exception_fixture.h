/** Declares the hand-written LLVM scenarios used by native and JIT runtime
 * tests. */
#pragma once
#include "runtime/exceptions.h"

extern "C" {
/** Returns the sum of payloads preserved through nested native cleanup. */
int sun_fixture_nested();
/** Returns A's payload after catching its parent, handling C, and rethrowing A.
 */
int sun_fixture_rethrow();
/** Attempts to escape the boundary protecting stack-based exception storage. */
void sun_fixture_escape();
/** Propagates through recursive frames and skips an unrelated catch handler. */
int sun_fixture_deep(int depth);
/** Returns an independently owned payload and trace captured through recursive
 * frames. */
void sun_fixture_capture(sun::runtime::ExceptionValue* output, int depth);
/** Handles a nested exception and then transfers the original trace through
 * rethrow. */
void sun_fixture_trace_rethrow(sun::runtime::ExceptionValue* original,
                               sun::runtime::ExceptionValue* output);
}

/** Borrows test entrypoints without owning their native or JIT code. */
namespace sun::tests::exceptions {
/** Exercises nested cleanup and trace-preserving rethrow under allocator
 * denial. */
struct TransportFixture {
  int (*nested)();
  int (*rethrow)();
};

inline constexpr TransportFixture nativeFixture{sun_fixture_nested,
                                                sun_fixture_rethrow};
}  // namespace sun::tests::exceptions
