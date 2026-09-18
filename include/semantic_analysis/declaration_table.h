#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "semantic_analysis/declaration_id.h"
#include "semantic_analysis/portable_declaration_key.h"
#include "support/error.h"

namespace sun {

struct SpecializationKey;

/** The source role of a declaration, independent of its resolved type. */
enum class DeclarationKind {
  Module,
  Function,
  Lambda,
  Class,
  Interface,
  Enum,
  Variable,
  Reference,
  Parameter,
  TypeParameter,
  LifetimeParameter,
  Field,
  Variant,
  Binding,
  Alias
};

/** The identity and ownership of a declaration in one analysis session. */
struct DeclarationRecord {
  DeclarationKind kind;
  std::string name;
  DeclarationId owner;
  DeclarationId module;
  std::optional<PortableDeclarationKey> portableKey;
  std::shared_ptr<const SpecializationKey> specialization;
  DeclarationId origin;
};

/** Allocate and retain declarations independently of symbol spelling. */
class DeclarationTable {
  std::deque<DeclarationRecord> records_;
  std::shared_ptr<const int> session_ = std::make_shared<const int>(0);
  std::map<std::pair<DeclarationId, std::string>, DeclarationId> modules_;
  std::map<PortableDeclarationKey, DeclarationId> portableDeclarations_;

 public:
  /** Start an independent table whose identities cannot be copied. */
  DeclarationTable() = default;
  DeclarationTable(const DeclarationTable&) = delete;
  DeclarationTable& operator=(const DeclarationTable&) = delete;
  DeclarationTable(DeclarationTable&&) = default;
  DeclarationTable& operator=(DeclarationTable&&) = default;

  /** Identify this session even after a previous table has been destroyed. */
  const std::shared_ptr<const int>& session() const { return session_; }

  /** Intern reopened module fragments under their existing module entity. */
  DeclarationId module(const std::string& name, DeclarationId owner = {}) {
    auto key = std::make_pair(owner, name);
    auto found = modules_.find(key);
    if (found != modules_.end()) return found->second;
    auto id = add(DeclarationKind::Module, name, owner, owner);
    modules_.emplace(std::move(key), id);
    return id;
  }

  /** Allocate an identity without making the declaration visible in a scope. */
  DeclarationId add(
      DeclarationKind kind, std::string name, DeclarationId owner = {},
      DeclarationId module = {},
      std::shared_ptr<const SpecializationKey> specialization = {},
      DeclarationId origin = {}) {
    if (owner) get(owner);
    if (module) get(module);
    if (origin) get(origin);
    records_.push_back({kind, std::move(name), owner, module, std::nullopt,
                        std::move(specialization), origin});
    return DeclarationId(records_.size());
  }

  /** Find a declaration in this session; unassigned identities are errors. */
  const DeclarationRecord& get(DeclarationId id) const {
    if (!id || id.index() > records_.size())
      logAndThrowError("Declaration identity does not belong to this analysis");
    return records_[id.index() - 1];
  }

  /** Find the session identity already assigned to an imported declaration. */
  DeclarationId findPortable(const PortableDeclarationKey& key) const {
    auto it = portableDeclarations_.find(key);
    return it == portableDeclarations_.end() ? DeclarationId{} : it->second;
  }

  /** Bind a portable identity exactly once, rejecting conflicting declarations.
   */
  void bindPortable(DeclarationId id, const PortableDeclarationKey& key) {
    get(id);
    if (key.empty()) logAndThrowError("Cannot bind an unassigned portable key");
    auto& record = records_[id.index() - 1];
    if (record.portableKey && !(*record.portableKey == key))
      logAndThrowError("Declaration already has another portable identity");
    auto existing = findPortable(key);
    if (existing && existing != id)
      logAndThrowError(
          "Portable identity already belongs to another declaration");
    portableDeclarations_.emplace(key, id);
    record.portableKey = key;
  }

  /** Return how many declarations this session has allocated. */
  size_t size() const { return records_.size(); }
};

}  // namespace sun
