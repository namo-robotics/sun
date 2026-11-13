/** Defines the bounded exception ABI shared by LLVM lowering and native helpers. */
#pragma once
#include <cstddef>

/** Describes the initial exception ABI on supported 64-bit targets. */
namespace sun::abi::exceptions {
inline constexpr std::size_t alignment = 16;
inline constexpr std::size_t payloadCapacity = 128;
inline constexpr std::size_t payloadOffset = 16;
inline constexpr std::size_t valueSize = payloadOffset + payloadCapacity;
inline constexpr std::size_t unwindHeaderSize = 32;
inline constexpr std::size_t recordSize = unwindHeaderSize + valueSize;
}  // namespace sun::abi::exceptions
