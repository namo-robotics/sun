/** Allocation-free exception transport shared by generated code. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <unwind.h>
#include "abi/exception_layout.h"

/** Stores Sun-owned exception values separately from native unwind state. */
namespace sun::runtime {

inline constexpr std::size_t exceptionPayloadCapacity = sun::abi::exceptions::payloadCapacity;
inline constexpr std::uint64_t exceptionClass = 0x53554e0045584300ULL;

/** Immutable metadata whose identity includes the declaration and layout contract.
 * Descriptors and their strings must outlive every value referring to them.
 */
struct ExceptionTypeDescriptor {
  const char* identity;
  const ExceptionTypeDescriptor* parent;
  std::uint32_t payloadSize;
  std::uint32_t payloadAlignment;
};

/** Owns a complete concrete payload; a null type denotes a consumed value. */
struct alignas(sun::abi::exceptions::alignment) ExceptionValue {
  const ExceptionTypeDescriptor* type;
  alignas(sun::abi::exceptions::alignment) unsigned char payload[exceptionPayloadCapacity];
};

/** Remains at one address until its native propagation has completed. */
struct alignas(sun::abi::exceptions::alignment) PropagationRecord {
  _Unwind_Exception unwind;
  ExceptionValue value;
};

/** Retains base storage and selects storage for subsequent throws on one thread. */
struct ThreadExceptionState {
  PropagationRecord base;
  PropagationRecord* throwTarget;
};

static_assert(sizeof(void*) == 8);
static_assert(sizeof(_Unwind_Exception) == sun::abi::exceptions::unwindHeaderSize);
static_assert(alignof(_Unwind_Exception) <= sun::abi::exceptions::alignment);
static_assert(offsetof(ExceptionValue, payload) == sun::abi::exceptions::payloadOffset);
static_assert(sizeof(ExceptionValue) == sun::abi::exceptions::valueSize);
static_assert(offsetof(PropagationRecord, value) == sun::abi::exceptions::unwindHeaderSize);
static_assert(sizeof(PropagationRecord) == sun::abi::exceptions::recordSize);

}  // namespace sun::runtime

extern "C" {
/** Selects the thread's reserved base record on first use; repeated calls are safe. */
void sun_eh_initialize() noexcept;

/** Selects fresh stack storage before cleanup, returning the previous target.
 * The caller must catch all escaping exceptions before this storage expires.
 */
sun::runtime::PropagationRecord* sun_eh_push(
    sun::runtime::PropagationRecord* storage) noexcept;

/** Restores the previous throw target after all nested propagation completes. */
void sun_eh_pop(sun::runtime::PropagationRecord* previous) noexcept;

/** Consumes a value into the selected record and starts native propagation. */
[[noreturn]] void sun_eh_raise(sun::runtime::ExceptionValue* value);

/** Consumes a handled record into independent catch storage and releases it. */
void sun_eh_take(_Unwind_Exception* exception,
                 sun::runtime::ExceptionValue* destination) noexcept;

/** Matches a concrete exception or any ancestor by its full nominal identity. */
bool sun_eh_matches(const sun::runtime::ExceptionTypeDescriptor* concrete,
                    const sun::runtime::ExceptionTypeDescriptor* handler) noexcept;

/** Terminates without invoking user cleanup when a runtime boundary is violated. */
[[noreturn]] void sun_eh_terminate() noexcept;

/** Interprets LLVM's exception tables using Sun descriptor-based family matching. */
_Unwind_Reason_Code sun_eh_personality(int version, _Unwind_Action actions,
                                      std::uint64_t exceptionClass,
                                      _Unwind_Exception* exception,
                                      _Unwind_Context* context);
}
