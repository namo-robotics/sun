/** Captures bounded traces starting at the requested caller. */
#include "runtime/exception_trace.h"

/** Keeps the native unwinder callback private to trace capture. */
namespace {
/** Tracks the capture boundary without counting platform-specific helper
 * frames. */
struct CaptureState {
  sun::runtime::StackTraceHeader& trace;
  std::uintptr_t callerReturnAddress;
  bool foundCaller = false;
};

/** Starts at the caller PC supplied by the reporting or throw entrypoint,
 * without a fixed skip count. */
_Unwind_Reason_Code captureFrame(_Unwind_Context* context,
                                 void* argument) noexcept {
  auto& capture = *static_cast<CaptureState*>(argument);
  int beforeInstruction = 0;
  auto address = _Unwind_GetIPInfo(context, &beforeInstruction);
  if (!capture.foundCaller) {
    if (address != capture.callerReturnAddress) {
      return _URC_NO_REASON;
    }
    capture.foundCaller = true;
  }
  if (!address) {
    return _URC_NO_REASON;
  }
  auto& trace = capture.trace;
  if (trace.count == sun_exception_trace_depth) {
    trace.status = sun::runtime::TraceStatus::Truncated;
    return _URC_NORMAL_STOP;
  }
  // Only return addresses need adjustment; signal instruction PCs already
  // identify code.
  trace.addresses()[trace.count++] = beforeInstruction ? address : address - 1;
  return _URC_NO_REASON;
}

}  // namespace

void sun::runtime::captureStackTrace(
    StackTraceHeader& trace, std::uintptr_t callerReturnAddress) noexcept {
  trace.count = 0;
  trace.status = sun::runtime::TraceStatus::Disabled;
  if (sun_exception_trace_depth > 0) {
    trace.status = sun::runtime::TraceStatus::Unavailable;
    CaptureState capture{trace, callerReturnAddress};
    const auto result = _Unwind_Backtrace(captureFrame, &capture);
    if (trace.status != sun::runtime::TraceStatus::Truncated && trace.count) {
      trace.status = result == _URC_END_OF_STACK
                         ? sun::runtime::TraceStatus::Complete
                         : sun::runtime::TraceStatus::Partial;
    }
  }
}
