#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "semantic_analysis/portable_type_key.h"

/** Defines syntax nodes whose declarations can be exported. */
namespace sun::ast {
class ExprAST;
}

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

class DeclarationTable;
class PortableTypeKey;

/**
 * One opaque string identity: decimal names for local declarations, retained
 * artifact identities for imports. Empty means no declaration is assigned.
 */
class DeclarationId {
  std::string value_;

 public:
  /** Construct an unassigned identity. */
  DeclarationId() = default;
  /** Construct a decimal identity for a session-local declaration. */
  explicit DeclarationId(uint64_t value)
      : value_(value ? std::to_string(value) : "") {}
  /** Retain an opaque declaration identity without interpreting its spelling.
   */
  explicit DeclarationId(std::string value) : value_(std::move(value)) {}
  /** Return the identifier's spelling for storage and serialization. */
  const std::string& encoding() const { return value_; }
  /** Report whether this identity is unset. */
  bool empty() const { return value_.empty(); }
  /** Report whether this identity has been assigned. */
  explicit operator bool() const { return !empty(); }
  /** Compare opaque declaration identifiers. */
  bool operator==(const DeclarationId& other) const {
    return value_ == other.value_;
  }
  /** Report whether the identifiers differ. */
  bool operator!=(const DeclarationId& other) const {
    return !(*this == other);
  }
  /** Order identifiers without interpreting their spelling. */
  bool operator<(const DeclarationId& other) const {
    return value_ < other.value_;
  }
  /** Derive a stable linker symbol for an emission role. */
  std::string symbol(const std::string& role) const;
  /** Assign source ordinals at the artifact boundary, excluding imported trees.
   */
  static void assignExportIds(const sun::ast::ExprAST& root,
                              DeclarationTable& table,
                              const std::string& artifactHash);
  /** Derive a key from assigned source keys and concrete specialization inputs.
   */
  static DeclarationId forExport(DeclarationId id,
                                 const DeclarationTable& table);
  /** Retain an artifact identifier unchanged; reject an unset identifier. */
  static DeclarationId fromString(const std::string& value);
  /** Identify an original declaration in a content-addressed artifact. */
  static DeclarationId original(const std::string& bundleHash,
                                uint64_t declarationNumber);
  /** Identify an instance by its template and concrete argument types. */
  static DeclarationId specialization(
      const DeclarationId& origin,
      const std::vector<PortableTypeKey>& arguments,
      const std::optional<std::vector<PortableTypeKey>>& variadicArguments =
          std::nullopt);
  /** Identify a declaration cloned within one specific enclosing instance. */
  static DeclarationId inInstance(const DeclarationId& origin,
                                  const DeclarationId& instance);
  /** Identify a generated declaration by a stable role within its origin. */
  static DeclarationId generated(const DeclarationId& origin,
                                 const std::string& role, uint64_t slot);
};

/** Annotation data that survives recomputing types within one session. */
struct DeclarationIdentity {
  bool imported = false;
  DeclarationId id;
  std::weak_ptr<const int> session;
  std::vector<DeclarationId> parameters;
  std::vector<DeclarationId> variadicParameters;
  std::vector<DeclarationId> typeParameters;
  std::vector<DeclarationId> lifetimeParameters;
  /** Report whether this annotation was bound, even if its session expired. */
  bool hasSession() const {
    const std::weak_ptr<const int> empty;
    return session.owner_before(empty) || empty.owner_before(session);
  }
  /** Discard session state while retaining the artifact's source identities. */
  void resetSession() {
    if (!imported) {
      *this = {};
      return;
    }
    session.reset();
    variadicParameters.clear();
  }
};

}  // namespace sun::semantic_analysis

/** Hashes declaration identities for use as keys in unordered containers. */
template <>
struct std::hash<sun::semantic_analysis::DeclarationId> {
  /** Computes a hash for the supplied value for use in unordered containers. */
  size_t operator()(
      const sun::semantic_analysis::DeclarationId& id) const noexcept {
    return std::hash<std::string>{}(id.encoding());
  }
};
