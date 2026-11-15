/** Checks cold unwinder lookup independently of Sun exception propagation. */
#include <unwind.h>

#include <cstdint>
#include <iterator>
#include <utility>

#include "allocation_audit.h"
#include "exception_allocation_support.h"

/** Supplies distinct physical frames so unwinder caches cannot hide allocation.
 */
namespace {
/** Stores a bounded native walk without allocating diagnostic scratch space. */
struct CaptureProbe {
  std::uintptr_t frames[16];
  unsigned count = 0;
  bool truncated = false;
};

/** Stops only after observing one more frame than the reserved storage holds.
 */
_Unwind_Reason_Code captureFrame(_Unwind_Context* context, void* argument) {
  auto& capture = *static_cast<CaptureProbe*>(argument);
  const auto address = _Unwind_GetIP(context);
  if (!address) {
    return _URC_NO_REASON;
  }
  if (capture.count == std::size(capture.frames)) {
    capture.truncated = true;
    return _URC_NORMAL_STOP;
  }
  capture.frames[capture.count++] = address;
  return _URC_NO_REASON;
}

/** Gives each walk a distinct native frame so lookup caches cannot hide later
 * allocation. */
template <std::size_t Index>
__attribute__((noinline)) unsigned captureDistinctFrame() {
  CaptureProbe capture;
  _Unwind_Backtrace(captureFrame, &capture);
  return capture.count + Index;
}

/** Visits enough different frames to exceed the initial cache in common
 * unwinders. */
template <std::size_t... Index>
bool captureDistinctFrames(std::index_sequence<Index...>) {
  return ((captureDistinctFrame<Index>() > Index) && ...);
}

}  // namespace

/** Exercises first-use and repeated native capture while allocator denial is
 * active. */
int sun::tests::exceptions::checkCapture() {
  AllocationAudit audit("Native trace capture");
  CaptureProbe first, repeated;
  const auto firstResult = _Unwind_Backtrace(captureFrame, &first);
  const auto repeatedResult = _Unwind_Backtrace(captureFrame, &repeated);
  const bool distinctFrames =
      captureDistinctFrames(std::make_index_sequence<128>{});
  if (!audit.finish()) {
    return 1;
  }
  if (!distinctFrames || !first.count || !repeated.count ||
      (firstResult != _URC_END_OF_STACK && !first.truncated) ||
      (repeatedResult != _URC_END_OF_STACK && !repeated.truncated)) {
    return 2;
  }
  return 0;
}
