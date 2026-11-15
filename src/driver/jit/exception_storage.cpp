/** Supplies default exception storage for the compiler and native runtime
 * tests. */
#include "runtime/exceptions.h"

extern "C" {
extern const std::uint64_t sun_exception_trace_depth =
    sun::abi::exceptions::defaultTraceDepth;
alignas(sun::abi::exceptions::
            alignment) constinit thread_local unsigned char sun_exception_state
    [sun::abi::exceptions::threadStateSizeForDepth(
        sun::abi::exceptions::defaultTraceDepth)]{};
}
