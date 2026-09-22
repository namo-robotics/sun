#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "semantic_analysis/declaration_id.h"
#include "semantic_analysis/portable_declaration_key.h"
#include "semantic_analysis/qualified_name.h"
#include "support/error.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
class ExprAST;
}

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::support::logAndThrowError;

struct SpecializationKey;

/** The source role of a declaration, independent of its resolved type. */
enum class DeclarationKind {
  Module = 0,
  Function = 1,
  Lambda = 2,
  Class = 3,
  Interface = 4,
  Enum = 5,
  Variable = 6,
  Reference = 7,
  Parameter = 8,
  TypeParameter = 9,
  LifetimeParameter = 10,
  Field = 11,
  Variant = 12,
  Binding = 13,
  Alias = 14
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
  std::string generatedRole;
  uint64_t generatedSlot = 0;
  // The syntax node that declares it, or null when no single node does: a
  // builtin, a field or variant inside a node, a module opened in several
  // places. Valid while the analyzed tree is alive, which the owner of the
  // analysis results keeps it for.
  const sun::ast::ExprAST* astNode = nullptr;
};

/** Allocate and retain declarations independently of symbol spelling. */
class DeclarationTable {
  std::deque<DeclarationRecord> records_;
  std::shared_ptr<const int> session_ = std::make_shared<const int>(0);
  std::map<std::pair<DeclarationId, std::string>, DeclarationId> modules_;
  std::map<PortableDeclarationKey, DeclarationId> portableDeclarations_;
  // The file-scope and module-scope variables this program's source
  // declares, by qualified name. The first declaration of a name wins.
  std::unordered_map<QualifiedName, DeclarationId> globals_;
  // The unqualified names in globals_, so that a name no global has can be
  // dismissed without building qualified names along the scope chain.
  std::unordered_set<std::string> globalBaseNames_;

 public:
  /** Start an independent table whose identities cannot be copied. */
  DeclarationTable() = default;
  /** Disallows copying so the owned state cannot be duplicated. */
  DeclarationTable(const DeclarationTable&) = delete;
  /** Disallows assignment so ownership and object identity cannot be
   * duplicated. */
  DeclarationTable& operator=(const DeclarationTable&) = delete;
  /** Creates an instance with its default state. */
  DeclarationTable(DeclarationTable&&) = default;
  /** Transfers the stored state from another instance during move assignment.
   */
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

  /** Links a declaration to the syntax node that declares it. */
  void bindAstNode(DeclarationId id, const sun::ast::ExprAST* astNode) {
    get(id);
    records_[id.index() - 1].astNode = astNode;
  }

  /**
   * Records a global variable under its qualified name, so a use that comes
   * before the declaration can find it. Only variables declared in this
   * program's source are recorded: those of a library and of C code have no
   * initializer to analyze.
   */
  void registerGlobal(const QualifiedName& name, DeclarationId id) {
    get(id);
    globals_.try_emplace(name, id);
    globalBaseNames_.insert(name.baseName);
  }

  /** Reports whether any registered global has this unqualified name. */
  bool hasGlobalNamed(const std::string& baseName) const {
    return globalBaseNames_.count(baseName) != 0;
  }

  /** The global variable with this qualified name, or an empty id. */
  DeclarationId findGlobal(const QualifiedName& name) const {
    auto found = globals_.find(name);
    return found == globals_.end() ? DeclarationId{} : found->second;
  }

  /** Allocate an identity without making the declaration visible in a scope. */
  DeclarationId add(
      DeclarationKind kind, std::string name, DeclarationId owner = {},
      DeclarationId module = {},
      std::shared_ptr<const SpecializationKey> specialization = {},
      DeclarationId origin = {}, std::string generatedRole = {},
      uint64_t generatedSlot = 0) {
    if (owner) get(owner);
    if (module) get(module);
    if (origin) get(origin);
    records_.push_back({kind, std::move(name), owner, module, std::nullopt,
                        std::move(specialization), origin,
                        std::move(generatedRole), generatedSlot});
    return DeclarationId(records_.size());
  }

  /** Intern a source declaration from an artifact, validating its ownership. */
  DeclarationId importOriginal(const std::string& encoded, DeclarationKind kind,
                               const std::string& name, DeclarationId owner,
                               DeclarationId module) {
    auto key = PortableDeclarationKey::parseOriginal(encoded);
    if (auto existing = findPortable(key)) {
      const auto& record = get(existing);
      if (record.kind != kind || record.name != name || record.owner != owner ||
          record.module != module)
        logAndThrowError("Conflicting imported declaration identity");
      return existing;
    }
    auto id = kind == DeclarationKind::Module ? this->module(name, owner)
                                              : add(kind, name, owner, module);
    bindPortable(id, key);
    return id;
  }

  /** Restore an artifact's ownership graph before registering its syntax. */
  void importRecords(const std::vector<ImportedDeclarationRecord>& records) {
    auto reference = [&](const std::string& encoded) {
      if (encoded.empty()) return DeclarationId{};
      auto id = findPortable(PortableDeclarationKey::parseOriginal(encoded));
      if (!id)
        logAndThrowError("Imported declaration refers to a missing owner");
      return id;
    };
    for (const auto& record : records) {
      if (record.kind > static_cast<uint32_t>(DeclarationKind::Alias))
        logAndThrowError("Imported declaration has an unknown kind");
      importOriginal(record.key, static_cast<DeclarationKind>(record.kind),
                     record.name, reference(record.owner),
                     reference(record.module));
    }
  }

  /** Attach imported syntax to its previously interned original declaration. */
  DeclarationId importedSyntax(const std::string& encoded, DeclarationKind kind,
                               const std::string& name) const {
    auto id = findPortable(PortableDeclarationKey::parseOriginal(encoded));
    if (!id) logAndThrowError("Imported syntax has no declaration record");
    const auto& record = get(id);
    if (record.kind != kind ||
        (kind != DeclarationKind::Module && record.name != name))
      logAndThrowError("Imported syntax does not match its declaration record");
    return id;
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

}  // namespace sun::semantic_analysis
