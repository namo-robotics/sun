/** Allocation-free exception transport shared by generated code. */
#pragma once

#include <unwind.h>

#include <cstddef>
#include <cstdint>

#include "abi/exception_layout.h"

/** Provides fixed-field views of the complete structs generated for Sun code.
 *
 * Thread storage: [target + padding] [unwind header] [value header] [frames]
 * Catch storage:                                   [value header] [frames]
 *
 * Generated structs include inline frame arrays. These C++ views omit those
 * arrays so one precompiled runtime supports every configured capacity.
 * Frame capacity comes from the linked program, not from sizeof these views.
 * The base record lives for the thread's lifetime. Destructor records live on
 * the stack until their nested propagation finishes. Taking an exception moves
 * its payload and populated frames into catch storage and clears the source
 * type.
 */
namespace sun::runtime {

inline constexpr std::size_t exceptionPayloadCapacity =
    sun::abi::exceptions::payloadCapacity;
inline constexpr std::uint64_t exceptionClass = 0x53554e0045584300ULL;

/** Immutable metadata whose identity includes the declaration and layout
 * contract. Descriptors and their strings must outlive every value referring to
 * them.
 */
struct ExceptionTypeDescriptor {
  const char* identity;
  const ExceptionTypeDescriptor* parent;
  std::uint32_t payloadSize;
  std::uint32_t payloadAlignment;
  const char* displayName = nullptr;
};

/** Distinguishes a depth limit from an incomplete or intentionally disabled
 * walk. */
enum class TraceStatus : std::uint32_t {
  Complete = 0,
  Truncated = 1,
  Partial = 2,
  Unavailable = 3,
  Disabled = 4
};

/** Describes inline frames placed immediately after this fixed-size header. */
struct StackTrace {
  std::uint64_t count;
  TraceStatus status;
  std::uint32_t reserved;

  /** Borrows trailing address storage supplied by the generated program. */
  std::uintptr_t* addresses() noexcept {
    return reinterpret_cast<std::uintptr_t*>(this + 1);
  }

  /** Borrows initialized trailing addresses without changing the trace. */
  const std::uintptr_t* addresses() const noexcept {
    return reinterpret_cast<const std::uintptr_t*>(this + 1);
  }
};

/** Matches the borrowed array descriptor passed by Sun's ordinary C ABI
 * lowering. */
struct TraceFrameView {
  const std::uintptr_t* frames;
  std::uint32_t rank;
  const std::uint64_t* dimensions;
};

/** Contains the payload and trace header; the caller also reserves trailing
 * frames. A null type marks consumed storage. Only sun_eh_take and sun_eh_raise
 * transfer ownership; copying this header alone does not copy the trace.
 */
struct alignas(sun::abi::exceptions::alignment) ExceptionValue {
  const ExceptionTypeDescriptor* type;
  alignas(sun::abi::exceptions::alignment) unsigned char payload
      [exceptionPayloadCapacity];
  StackTrace trace;
};

/** Holds the fixed fields of an exception during native propagation. The caller
 * reserves trailing stack-trace addresses beyond this structure and keeps the
 * entire allocation at one address until propagation completes.
 */
struct alignas(sun::abi::exceptions::alignment) PropagationRecord {
  _Unwind_Exception unwind;
  ExceptionValue value;
};

/** Selects the throw target; a depth-sized base record follows this header. */
struct alignas(sun::abi::exceptions::alignment) ThreadExceptionState {
  PropagationRecord* throwTarget;

  /** Returns the base record reserved immediately after this header. */
  PropagationRecord* base() noexcept {
    return reinterpret_cast<PropagationRecord*>(this + 1);
  }
};

static_assert(sizeof(ThreadExceptionState) == sun::abi::exceptions::alignment);
static_assert(sizeof(StackTrace) == sun::abi::exceptions::traceFramesOffset -
                                        sun::abi::exceptions::traceOffset);
static_assert(sizeof(void*) == 8);
static_assert(sizeof(_Unwind_Exception) ==
              sun::abi::exceptions::unwindHeaderSize);
static_assert(alignof(_Unwind_Exception) <= sun::abi::exceptions::alignment);
static_assert(offsetof(ExceptionValue, payload) ==
              sun::abi::exceptions::payloadOffset);
static_assert(offsetof(ExceptionValue, trace) ==
              sun::abi::exceptions::traceOffset);
static_assert(offsetof(ExceptionValue, trace) + offsetof(StackTrace, status) ==
              sun::abi::exceptions::traceStatusOffset);
static_assert(offsetof(ExceptionValue, trace) + sizeof(StackTrace) ==
              sun::abi::exceptions::traceFramesOffset);
static_assert(sizeof(ExceptionValue) ==
              sun::abi::exceptions::valueSizeForDepth(0));
static_assert(offsetof(PropagationRecord, value) ==
              sun::abi::exceptions::unwindHeaderSize);
static_assert(sizeof(PropagationRecord) ==
              sun::abi::exceptions::unwindHeaderSize +
                  sun::abi::exceptions::valueSizeForDepth(0));

}  // namespace sun::runtime

extern "C" {
/** Defines the capture capacity selected by the linked Sun program. */
extern const std::uint64_t sun_exception_trace_depth;
/** Reserves a thread-state header and a depth-sized base propagation record. */
alignas(
    sun::abi::exceptions::
        alignment) extern thread_local constinit unsigned char sun_exception_state
    [];

/** Selects the thread's reserved base record on first use; repeated calls are
 * safe. */
void sun_eh_initialize() noexcept;

/** Selects fresh stack storage before cleanup, returning the previous target.
 * The caller must catch all escaping exceptions before this storage expires.
 */
sun::runtime::PropagationRecord* sun_eh_push(
    sun::runtime::PropagationRecord* storage) noexcept;

/** Restores the previous throw target after all nested propagation completes.
 */
void sun_eh_pop(sun::runtime::PropagationRecord* previous) noexcept;

/** Copies a fully evaluated payload into reserved storage and captures its
 * original trace. */
[[noreturn]] void sun_eh_throw(
    const sun::runtime::ExceptionTypeDescriptor* type, const void* payload);

/** Consumes a retained value, preserving its original trace, and starts
 * propagation. */
[[noreturn]] void sun_eh_raise(sun::runtime::ExceptionValue* value);

/** Reports the exception type and source call chain without consuming the
 * value. */
void sun_eh_report(const sun::runtime::ExceptionValue* value,
                   std::uint32_t unhandled = 0) noexcept;

/** Captures and reports the current stack without needing an active exception.
 */
void sun_report_stack_trace() noexcept;

/** Prints a safe borrowed array view without consuming its originating
 * exception. */
void sun_eh_print_trace(sun::runtime::TraceFrameView view,
                        std::uint32_t status) noexcept;

/** Reports the actual escaping propagation value before terminating. */
[[noreturn]] void sun_eh_terminate_exception(
    _Unwind_Exception* exception) noexcept;

/** Consumes a handled record into independent catch storage and releases it. */
void sun_eh_take(_Unwind_Exception* exception,
                 sun::runtime::ExceptionValue* destination) noexcept;

/** Matches a concrete exception or any ancestor by its full nominal identity.
 */
bool sun_eh_matches(
    const sun::runtime::ExceptionTypeDescriptor* concrete,
    const sun::runtime::ExceptionTypeDescriptor* handler) noexcept;

/** Terminates without invoking user cleanup when a runtime boundary is
 * violated. */
[[noreturn]] void sun_eh_terminate() noexcept;

/** Interprets LLVM's exception tables using Sun descriptor-based family
 * matching. */
_Unwind_Reason_Code sun_eh_personality(int version, _Unwind_Action actions,
                                       std::uint64_t exceptionClass,
                                       _Unwind_Exception* exception,
                                       _Unwind_Context* context);
}
