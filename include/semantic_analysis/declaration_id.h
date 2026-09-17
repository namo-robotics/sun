#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace sun {

/** Identifies a declaration within its owning analysis session. */
class DeclarationId {
  uint64_t value_ = 0;

 public:
  /** Construct an unassigned identity. */
  DeclarationId() = default;
  /** Construct an identity allocated by a declaration table. */
  explicit DeclarationId(uint64_t value) : value_(value) {}
  /** Return the table index; this value is never a portable identity. */
  uint64_t index() const { return value_; }
  /** Report whether this identity has been assigned. */
  explicit operator bool() const { return value_ != 0; }
  bool operator==(DeclarationId other) const { return value_ == other.value_; }
  bool operator!=(DeclarationId other) const { return !(*this == other); }
  bool operator<(DeclarationId other) const { return value_ < other.value_; }
};

/** Annotation data that survives recomputing types within one session. */
struct DeclarationIdentity {
  DeclarationId id;
  std::weak_ptr<const int> session;
  std::vector<DeclarationId> parameters;
  std::vector<DeclarationId> typeParameters;
  std::vector<DeclarationId> lifetimeParameters;
};

}  // namespace sun

template <>
struct std::hash<sun::DeclarationId> {
  size_t operator()(sun::DeclarationId id) const noexcept {
    return std::hash<uint64_t>{}(id.index());
  }
};
