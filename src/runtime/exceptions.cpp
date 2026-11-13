/** Implements bounded exception transport without C++ exception allocation. */
#include "runtime/exceptions.h"
#include "runtime/panic.h"

#include <cstring>

/** Keeps runtime state and table decoding private to the transport implementation. */
namespace {
/** Refers to compiler-emitted immutable exception metadata. */
using sun::runtime::ExceptionTypeDescriptor;
/** Refers to an owned inline exception value. */
using sun::runtime::ExceptionValue;
/** Refers to storage that remains stable during native propagation. */
using sun::runtime::PropagationRecord;

constinit thread_local sun::runtime::ThreadExceptionState state{};

/** Returns the thread's initialized transport state. */
sun::runtime::ThreadExceptionState& threadState() noexcept {
  if (!state.throwTarget) sun_eh_terminate();
  return state;
}

/** Releases a record without moving or interpreting private native unwind fields. */
void releaseRecord(_Unwind_Reason_Code, _Unwind_Exception* exception) noexcept {
  reinterpret_cast<PropagationRecord*>(exception)->value.type = nullptr;
}

/** Checks the compiler/runtime payload contract before accessing inline storage. */
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
  source->type = nullptr;
}

/** Reads a potentially unaligned fixed-width field from trusted unwind metadata. */
template <typename T>
T read(const unsigned char*& cursor) noexcept {
  T result;
  std::memcpy(&result, cursor, sizeof(T));
  cursor += sizeof(T);
  return result;
}

/** Decodes an unsigned LEB128 field, rejecting integer overflow. */
std::uintptr_t uleb(const unsigned char*& cursor) noexcept {
  std::uintptr_t result = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    const auto byte = *cursor++;
    if (shift == 63 && (byte & 0x7e)) sun_eh_terminate();
    result |= std::uintptr_t(byte & 0x7f) << shift;
    if (!(byte & 0x80)) return result;
  }
  sun_eh_terminate();
}

/** Decodes a signed LEB128 action field without signed-shift overflow. */
std::intptr_t sleb(const unsigned char*& cursor) noexcept {
  std::uintptr_t result = 0;
  unsigned shift = 0;
  unsigned char byte;
  do {
    if (shift >= 64) sun_eh_terminate();
    byte = *cursor++;
    if (shift == 63 && (byte & 0x7f) != 0 && (byte & 0x7f) != 0x7f)
      sun_eh_terminate();
    result |= std::uintptr_t(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  if (shift < 64 && (byte & 0x40)) result |= ~std::uintptr_t(0) << shift;
  return static_cast<std::intptr_t>(result);
}

/** Returns the fixed width required for reverse-indexed exception type entries. */
std::size_t encodedSize(unsigned encoding) noexcept {
  switch (encoding & 0x0f) {
    case 0: return sizeof(void*);
    case 2: case 0xa: return 2;
    case 3: case 0xb: return 4;
    case 4: case 0xc: return 8;
    default: sun_eh_terminate();
  }
}

/** Decodes the pointer encodings emitted by LLVM on the supported native targets. */
std::uintptr_t encoded(const unsigned char*& cursor, unsigned encoding,
                       _Unwind_Context* context) noexcept {
  if (encoding == 0xff) return 0;
  const auto address = reinterpret_cast<std::uintptr_t>(cursor);
  std::uintptr_t result;
  switch (encoding & 0x0f) {
    case 0: result = read<std::uintptr_t>(cursor); break;
    case 1: result = uleb(cursor); break;
    case 2: result = read<std::uint16_t>(cursor); break;
    case 3: result = read<std::uint32_t>(cursor); break;
    case 4: result = read<std::uint64_t>(cursor); break;
    case 9: result = sleb(cursor); break;
    case 0xa: result = read<std::int16_t>(cursor); break;
    case 0xb: result = read<std::int32_t>(cursor); break;
    case 0xc: result = read<std::int64_t>(cursor); break;
    default: sun_eh_terminate();
  }
  if (!result) return 0;
  switch (encoding & 0x70) {
    case 0: break;
    case 0x10: result += address; break;
    case 0x30:
#if defined(__APPLE__)
      // Darwin does not provide a data-relative base through the unwind API.
      sun_eh_terminate();
#else
      result += _Unwind_GetDataRelBase(context);
      break;
#endif
    case 0x40: result += _Unwind_GetRegionStart(context); break;
    default: sun_eh_terminate();
  }
  if (encoding & 0x80) {
    auto* pointer = reinterpret_cast<const unsigned char*>(result);
    result = read<std::uintptr_t>(pointer);
  }
  return result;
}
}  // namespace

void sun_eh_initialize() noexcept {
  if (!state.throwTarget) state.throwTarget = &state.base;
}

PropagationRecord* sun_eh_push(PropagationRecord* storage) noexcept {
  auto& current = threadState();
  if (!storage || storage == current.throwTarget) sun_eh_terminate();
  storage->value.type = nullptr;
  auto* previous = current.throwTarget;
  current.throwTarget = storage;
  return previous;
}

void sun_eh_pop(PropagationRecord* previous) noexcept {
  auto& current = threadState();
  if (!previous || previous == current.throwTarget ||
      current.throwTarget->value.type) sun_eh_terminate();
  current.throwTarget = previous;
}

void sun_eh_raise(ExceptionValue* value) {
  auto* record = threadState().throwTarget;
  if (!value || record->value.type || value == &record->value)
    sun_eh_terminate();
  moveValue(&record->value, value);
  record->unwind.exception_class = sun::runtime::exceptionClass;
  record->unwind.exception_cleanup = releaseRecord;
  (void)_Unwind_RaiseException(&record->unwind);
  sun_eh_terminate();
}

void sun_eh_take(_Unwind_Exception* exception,
                 ExceptionValue* destination) noexcept {
  if (!exception || !destination ||
      exception->exception_class != sun::runtime::exceptionClass)
    sun_eh_terminate();
  auto* record = reinterpret_cast<PropagationRecord*>(exception);
  if (destination == &record->value) sun_eh_terminate();
  moveValue(destination, &record->value);
  _Unwind_DeleteException(exception);
}

bool sun_eh_matches(const ExceptionTypeDescriptor* concrete,
                    const ExceptionTypeDescriptor* handler) noexcept {
  if (!handler) return true;
  for (auto* type = concrete; type; type = type->parent) {
    if (type == handler || std::strcmp(type->identity, handler->identity) == 0)
      return true;
  }
  return false;
}

void sun_eh_terminate() noexcept {
  constexpr char message[] = "Sun exception escaped its runtime boundary";
  sun_panic(message, sizeof(message) - 1);
}

_Unwind_Reason_Code sun_eh_personality(int version, _Unwind_Action actions,
                                      std::uint64_t exceptionClass,
                                      _Unwind_Exception* exception,
                                      _Unwind_Context* context) {
  if (version != 1) return _URC_FATAL_PHASE1_ERROR;
  if (exceptionClass != sun::runtime::exceptionClass ||
      (actions & _UA_FORCE_UNWIND)) sun_eh_terminate();
  // Darwin exposes the LSDA address as uintptr_t; other unwinders use void*.
  auto* cursor = reinterpret_cast<const unsigned char*>(
      _Unwind_GetLanguageSpecificData(context));
  if (!cursor) return _URC_CONTINUE_UNWIND;
  const auto region = _Unwind_GetRegionStart(context);
  const auto landingEncoding = *cursor++;
  const auto landingBase = landingEncoding == 0xff
                               ? region : encoded(cursor, landingEncoding, context);
  const auto typeEncoding = *cursor++;
  const unsigned char* typeTable = nullptr;
  if (typeEncoding != 0xff) {
    const auto offset = uleb(cursor);
    typeTable = cursor + offset;
  }
  const auto callEncoding = *cursor++;
  const auto callLength = uleb(cursor);
  const auto* actionTable = cursor + callLength;
  int beforeInstruction = 0;
  auto ip = _Unwind_GetIPInfo(context, &beforeInstruction);
  if (!beforeInstruction) --ip;
  const auto position = ip - region;
  while (cursor < actionTable) {
    const auto start = encoded(cursor, callEncoding, context);
    const auto length = encoded(cursor, callEncoding, context);
    const auto landing = encoded(cursor, callEncoding, context);
    const auto action = uleb(cursor);
    if (position < start || position - start >= length) continue;
    if (!landing) return _URC_CONTINUE_UNWIND;
    bool cleanup = !action;
    std::intptr_t selector = 0;
    if (action) {
      auto* entry = actionTable + action - 1;
      while (true) {
        const auto filter = sleb(entry);
        const auto* nextBase = entry;
        const auto next = sleb(entry);
        if (!filter) {
          cleanup = true;
        } else if (filter > 0) {
          if (!typeTable) sun_eh_terminate();
          auto* typeEntry = typeTable - filter * encodedSize(typeEncoding);
          auto* handler = reinterpret_cast<const ExceptionTypeDescriptor*>(
              encoded(typeEntry, typeEncoding, context));
          if (!selector && sun_eh_matches(
                  reinterpret_cast<PropagationRecord*>(exception)->value.type,
                  handler)) selector = filter;
        } else {
          sun_eh_terminate();
        }
        if (!next) break;
        entry = nextBase + next;
      }
    }
    if (actions & _UA_SEARCH_PHASE)
      return selector ? _URC_HANDLER_FOUND : _URC_CONTINUE_UNWIND;
    if (!(actions & _UA_HANDLER_FRAME)) selector = 0;
    if (!selector && !cleanup) return _URC_CONTINUE_UNWIND;
    _Unwind_SetGR(context, __builtin_eh_return_data_regno(0),
                  reinterpret_cast<std::uintptr_t>(exception));
    _Unwind_SetGR(context, __builtin_eh_return_data_regno(1), selector);
    _Unwind_SetIP(context, landingBase + landing);
    return _URC_INSTALL_CONTEXT;
  }
  return _URC_CONTINUE_UNWIND;
}
