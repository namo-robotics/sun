#pragma once

#include <atomic>
#include <cstdint>

namespace sun {

/** Identifies a source unit independently of its diagnostic path. Zero is
 * unset. */
using SourceFileId = uint64_t;

/** Allocate a distinct identity for a parsed or loaded source unit. */
inline SourceFileId nextSourceFileId() {
  static std::atomic<SourceFileId> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace sun
