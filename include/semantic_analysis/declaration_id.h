#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

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
  /** Compares the stored values for equality. */
  bool operator==(DeclarationId other) const { return value_ == other.value_; }
  /** Reports whether the stored values differ. */
  bool operator!=(DeclarationId other) const { return !(*this == other); }
  /** Orders values for use in sorted containers. */
  bool operator<(DeclarationId other) const { return value_ < other.value_; }
};

/** Portable source identities retained across importing analysis sessions.
 */
struct LibraryDeclarationIdentity {
  std::string key;
  std::vector<std::string> parameters;
  std::vector<std::string> typeParameters;
  std::vector<std::string> lifetimeParameters;
};

/** One original declaration exported with its portable ownership links. */
struct LibraryDeclarationRecord {
  std::string key;
  uint32_t kind = 0;
  std::string name;
  std::string owner;
  std::string module;
  std::string bundleHash;
};

/** Annotation data that survives recomputing types within one session. */
struct DeclarationIdentity {
  std::optional<LibraryDeclarationIdentity> imported;
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

}  // namespace sun::semantic_analysis

/** Hashes declaration identities for use as keys in unordered containers. */
template <>
struct std::hash<sun::semantic_analysis::DeclarationId> {
  /** Computes a hash for the supplied value for use in unordered containers. */
  size_t operator()(sun::semantic_analysis::DeclarationId id) const noexcept {
    return std::hash<uint64_t>{}(id.index());
  }
};
