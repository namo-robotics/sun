/** Implements bounded exception transport without C++ exception allocation. */
#include "runtime/exceptions.h"

#include <cstring>

#include "runtime/exception_trace.h"
#include "runtime/panic.h"

/** Keeps storage access and ownership transfers private to exception transport.
 */
namespace {
/** Refers to compiler-emitted immutable exception metadata. */
using sun::runtime::ExceptionTypeDescriptor;
/** Refers to an owned inline exception value. */
using sun::runtime::ExceptionValue;
/** Refers to storage that remains stable during native propagation. */
using sun::runtime::PropagationRecord;

/** Interprets storage supplied by the linked program without allocating memory.
 */
sun::runtime::ThreadExceptionState& reservedState() noexcept {
  return *reinterpret_cast<sun::runtime::ThreadExceptionState*>(
      sun_exception_state);
}

/** Returns the thread's initialized transport state. */
sun::runtime::ThreadExceptionState& threadState() noexcept {
  auto& state = reservedState();
  if (!state.throwTarget) {
    sun_eh_terminate();
  }
  return state;
}

/** Releases a record without moving or interpreting private native unwind
 * fields. */
void releaseRecord(_Unwind_Reason_Code, _Unwind_Exception* exception) noexcept {
  reinterpret_cast<PropagationRecord*>(exception)->value.type = nullptr;
}

/** Checks the compiler/runtime payload contract before accessing inline
 * storage. */
void validateType(const ExceptionTypeDescriptor* type) noexcept {
  if (!type || !type->identity || !type->payloadAlignment ||
      type->payloadAlignment > sun::abi::exceptions::alignment ||
      (type->payloadAlignment & (type->payloadAlignment - 1)) ||
      type->payloadSize > sun::runtime::exceptionPayloadCapacity) {
    sun_eh_terminate();
  }
}

/** Moves the bounded payload without invoking user code. */
void moveValue(ExceptionValue* destination, ExceptionValue* source) noexcept {
  validateType(source->type);
  destination->type = source->type;
  std::memcpy(destination->payload, source->payload, source->type->payloadSize);
  if (source->trace.count > sun_exception_trace_depth) {
    sun_eh_terminate();
  }
  destination->trace.count = source->trace.count;
  destination->trace.status = source->trace.status;
  if (source->trace.count) {
    std::memcpy(destination->trace.addresses(), source->trace.addresses(),
                source->trace.count * sizeof(std::uintptr_t));
  }
  source->type = nullptr;
}

/** Initializes the stable native header only after the value is ready to
 * propagate. */
[[noreturn]] void propagate(PropagationRecord* record) {
  record->unwind.exception_class = sun::runtime::exceptionClass;
  record->unwind.exception_cleanup = releaseRecord;
  (void)_Unwind_RaiseException(&record->unwind);
  sun_eh_terminate_exception(&record->unwind);
}

}  // namespace

void sun_eh_initialize() noexcept {
  if (sun_exception_trace_depth > sun::abi::exceptions::maximumTraceDepth) {
    sun_eh_terminate();
  }
  auto& state = reservedState();
  if (!state.throwTarget) {
    state.throwTarget = state.base();
  }
}

PropagationRecord* sun_eh_push(PropagationRecord* storage) noexcept {
  sun_eh_initialize();
  auto& current = threadState();
  if (!storage || storage == current.throwTarget) {
    sun_eh_terminate();
  }
  storage->value.type = nullptr;
  auto* previous = current.throwTarget;
  current.throwTarget = storage;
  return previous;
}

void sun_eh_pop(PropagationRecord* previous) noexcept {
  auto& current = threadState();
  if (!previous || previous == current.throwTarget ||
      current.throwTarget->value.type) {
    sun_eh_terminate();
  }
  current.throwTarget = previous;
}

__attribute__((noinline)) void sun_eh_throw(const ExceptionTypeDescriptor* type,
                                            const void* payload) {
  sun_eh_initialize();
  auto* record = threadState().throwTarget;
  validateType(type);
  if (!payload || record->value.type) {
    sun_eh_terminate();
  }
  record->value.type = type;
  std::memcpy(record->value.payload, payload, type->payloadSize);
  const auto throwReturnAddress = reinterpret_cast<std::uintptr_t>(
      __builtin_extract_return_addr(__builtin_return_address(0)));
  sun::runtime::captureStackTrace(record->value.trace, throwReturnAddress);
  propagate(record);
}

void sun_eh_raise(ExceptionValue* value) {
  auto* record = threadState().throwTarget;
  if (!value || record->value.type || value == &record->value) {
    sun_eh_terminate();
  }
  moveValue(&record->value, value);
  propagate(record);
}

void sun_eh_terminate_exception(_Unwind_Exception* exception) noexcept {
  if (exception && exception->exception_class == sun::runtime::exceptionClass) {
    sun_eh_report(&reinterpret_cast<PropagationRecord*>(exception)->value, 1);
  }
  sun_eh_terminate();
}

void sun_eh_take(_Unwind_Exception* exception,
                 ExceptionValue* destination) noexcept {
  if (!exception || !destination ||
      exception->exception_class != sun::runtime::exceptionClass) {
    sun_eh_terminate();
  }
  auto* record = reinterpret_cast<PropagationRecord*>(exception);
  if (destination == &record->value) {
    sun_eh_terminate();
  }
  moveValue(destination, &record->value);
  _Unwind_DeleteException(exception);
}

bool sun_eh_matches(const ExceptionTypeDescriptor* concrete,
                    const ExceptionTypeDescriptor* handler) noexcept {
  if (!handler) {
    return true;
  }
  for (auto* type = concrete; type; type = type->parent) {
    if (type == handler ||
        std::strcmp(type->identity, handler->identity) == 0) {
      return true;
    }
  }
  return false;
}

void sun_eh_terminate() noexcept {
  constexpr char message[] = "Sun exception escaped its runtime boundary";
  sun_panic(message, sizeof(message) - 1);
}
