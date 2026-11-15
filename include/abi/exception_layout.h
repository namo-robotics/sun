/** Defines the bounded exception ABI shared by LLVM lowering and native
 * helpers. */
#pragma once
#include <cstddef>

/** Describes the initial exception ABI on supported 64-bit targets. */
namespace sun::abi::exceptions {
inline constexpr std::size_t alignment = 16;
inline constexpr std::size_t payloadCapacity = 128;
inline constexpr std::size_t payloadOffset = 16;
inline constexpr unsigned defaultTraceDepth = 16;
inline constexpr unsigned maximumTraceDepth = 256;
inline constexpr std::size_t traceOffset = payloadOffset + payloadCapacity;
inline constexpr std::size_t traceStatusOffset = traceOffset + 8;
inline constexpr std::size_t traceFramesOffset = traceOffset + 16;

/** Computes the aligned value size after the caller validates the depth. */
constexpr std::size_t valueSizeForDepth(std::size_t depth) {
  return (traceFramesOffset + depth * sizeof(void*) + alignment - 1) /
         alignment * alignment;
}

inline constexpr std::size_t valueSize = valueSizeForDepth(defaultTraceDepth);
inline constexpr std::size_t unwindHeaderSize = 32;
inline constexpr std::size_t recordSize = unwindHeaderSize + valueSize;

/** Reserves the thread-state header followed by its aligned base record. */
constexpr std::size_t threadStateSizeForDepth(std::size_t depth) {
  return alignment + unwindHeaderSize + valueSizeForDepth(depth);
}
}  // namespace sun::abi::exceptions
