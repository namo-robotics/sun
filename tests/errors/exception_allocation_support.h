/** Shares allocator denial and native fixtures between standalone and JIT
 * probes. */
#pragma once
#include "exception_fixture.h"

/** Tests exception operations while observing every intercepted allocation
 * attempt. Returns zero on success, one for an allocation attempt, and two
 * for an incorrect payload or trace. */
namespace sun::tests::exceptions {
/** Checks first-use and repeated native stack walking with allocation denied.
 */
int checkCapture();
/** Checks cold nested cleanup and rethrow after fixture loading. */
int checkTransport(const TransportFixture& fixture);
/** Checks complete and truncated capture, followed by both reporting
 * entrypoints. */
int checkNativeTraceBounds();
/** Audits worker propagation while excluding thread startup and teardown. */
int checkConcurrentTransport(const TransportFixture& fixture);
}  // namespace sun::tests::exceptions
