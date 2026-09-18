#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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

/** Portable source identities retained across importing analysis sessions.
 */
struct ImportedDeclarationIdentity {
  std::string declaration;
  std::vector<std::string> parameters;
  std::vector<std::string> typeParameters;
  std::vector<std::string> lifetimeParameters;
};

/** One original declaration exported with its portable ownership links. */
struct ImportedDeclarationRecord {
  std::string key;
  uint32_t kind = 0;
  std::string name;
  std::string owner;
  std::string module;
};

/** Annotation data that survives recomputing types within one session. */
struct DeclarationIdentity {
  std::optional<ImportedDeclarationIdentity> imported;
  DeclarationId id;
  std::weak_ptr<const int> session;
  std::vector<DeclarationId> parameters;
  std::vector<DeclarationId> variadicParameters;
  std::vector<DeclarationId> typeParameters;
  std::vector<DeclarationId> lifetimeParameters;
  /** Discard session state while retaining the artifact's source identities. */
  void resetSession() {
    auto source = std::move(imported);
    *this = {};
    imported = std::move(source);
  }
};

}  // namespace sun

template <>
struct std::hash<sun::DeclarationId> {
  size_t operator()(sun::DeclarationId id) const noexcept {
    return std::hash<uint64_t>{}(id.index());
  }
};
