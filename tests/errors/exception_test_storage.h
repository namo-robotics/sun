/** Owns the trailing frames required by the default-depth native fixtures. */
#pragma once
#include <array>

#include "runtime/exceptions.h"

/** Supplies complete storage while production runtime types describe only
 * headers. */
namespace sun::tests::exceptions {
/** Owns one catch value and all of its reserved frame addresses. */
struct OwnedExceptionStorage {
  runtime::ExceptionValueHeader header;
  std::array<std::uintptr_t, abi::exceptions::defaultTraceDepth> frames;
};

/** Owns one stable unwind record and its reserved frame addresses. */
struct OwnedPropagationStorage {
  runtime::PropagationRecordHeader header;
  std::array<std::uintptr_t, abi::exceptions::defaultTraceDepth> frames;
};

static_assert(offsetof(OwnedExceptionStorage, frames) ==
              abi::exceptions::traceFramesOffset);
static_assert(offsetof(OwnedPropagationStorage, frames) ==
              abi::exceptions::unwindHeaderSize +
                  abi::exceptions::traceFramesOffset);
static_assert(sizeof(OwnedExceptionStorage) == abi::exceptions::valueSize);
static_assert(sizeof(OwnedPropagationStorage) == abi::exceptions::recordSize);
}  // namespace sun::tests::exceptions
