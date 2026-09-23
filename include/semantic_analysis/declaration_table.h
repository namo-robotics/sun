#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "semantic_analysis/declaration_record.h"
#include "semantic_analysis/qualified_name.h"
#include "support/error.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::support::logAndThrowError;

/** Allocate and retain declarations independently of symbol spelling. */
class DeclarationTable {
  std::unordered_map<DeclarationId, DeclarationRecord> records_;
  std::vector<DeclarationId> order_;
  uint64_t nextLocalId_ = 1;
  std::shared_ptr<const int> session_ = std::make_shared<const int>(0);
  std::map<std::pair<DeclarationId, std::string>, DeclarationId> modules_;
  // Artifact-boundary names do not participate in session lookup.
  std::map<DeclarationId, DeclarationId> exportIds_;
  std::map<DeclarationId, DeclarationId> exportOwners_;
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
    records_.at(id).astNode = astNode;
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
      uint64_t generatedSlot = 0, DeclarationId importedId = {}) {
    if (owner) get(owner);
    if (module) get(module);
    if (origin) get(origin);
    DeclarationId id = importedId;
    if (!id) {
      do {
        id = DeclarationId(nextLocalId_++);
      } while (records_.contains(id));
    }
    if (records_.contains(id))
      logAndThrowError("Declaration identity already exists");
    records_.emplace(
        id, DeclarationRecord{kind, std::move(name), owner, module,
                              bool(importedId), std::move(specialization),
                              origin, std::move(generatedRole), generatedSlot});
    order_.push_back(id);
    return id;
  }

  /** Intern a source declaration from an artifact, validating its ownership. */
  DeclarationId importOriginal(const std::string& encoded, DeclarationKind kind,
                               const std::string& name, DeclarationId owner,
                               DeclarationId module,
                               const std::string& bundleHash = {}) {
    auto key = DeclarationId::fromString(encoded);
    if (auto existing = find(key)) {
      const auto& record = get(existing);
      if (!record.imported || record.kind != kind || record.name != name ||
          record.owner != owner || record.module != module ||
          record.bundleHash != bundleHash || record.specialization ||
          record.origin)
        logAndThrowError("Conflicting imported declaration identity");
      return existing;
    }
    if (kind == DeclarationKind::Module && modules_.contains({owner, name}))
      logAndThrowError("Conflicting imported module identity");
    auto id = add(kind, name, owner, module, {}, {}, {}, 0, key);
    records_.at(id).bundleHash = bundleHash;
    if (kind == DeclarationKind::Module)
      modules_.emplace(std::make_pair(owner, name), id);
    return id;
  }

  /** Restore an artifact's ownership graph before registering its syntax. */
  void importRecords(
      const std::vector<std::pair<DeclarationId, DeclarationRecord>>& records) {
    std::map<DeclarationId, size_t> indices;
    for (size_t i = 0; i < records.size(); ++i) {
      if (!records[i].first)
        logAndThrowError("Imported declaration has an empty identity");
      if (static_cast<uint32_t>(records[i].second.kind) >
          static_cast<uint32_t>(DeclarationKind::Alias))
        logAndThrowError("Imported declaration has an unknown kind");
      indices.try_emplace(records[i].first, i);
    }
    std::vector<size_t> pending(records.size());
    std::vector<std::vector<size_t>> dependents(records.size());
    std::deque<size_t> ready;
    for (size_t i = 0; i < records.size(); ++i) {
      for (const auto* link :
           {&records[i].second.owner, &records[i].second.module}) {
        if (link->empty()) continue;
        auto found = indices.find(*link);
        if (found != indices.end()) {
          ++pending[i];
          dependents[found->second].push_back(i);
        } else if (!find(*link)) {
          logAndThrowError("Imported declaration refers to a missing owner");
        }
      }
      if (!pending[i]) ready.push_back(i);
    }
    size_t restored = 0;
    while (!ready.empty()) {
      const auto i = ready.front();
      ready.pop_front();
      const auto& [id, record] = records[i];
      importOriginal(id.encoding(), record.kind, record.name, record.owner,
                     record.module, record.bundleHash);
      ++restored;
      for (auto dependent : dependents[i])
        if (--pending[dependent] == 0) ready.push_back(dependent);
    }
    if (restored != records.size())
      logAndThrowError("Imported declaration ownership contains a cycle");
  }

  /** Attach imported syntax to its previously interned original declaration. */
  DeclarationId importedSyntax(const std::string& encoded, DeclarationKind kind,
                               const std::string& name) const {
    auto id = find(DeclarationId::fromString(encoded));
    if (!id) logAndThrowError("Imported syntax has no declaration record");
    const auto& record = get(id);
    if (record.specialization || record.origin || record.kind != kind ||
        (kind != DeclarationKind::Module && record.name != name))
      logAndThrowError("Imported syntax does not match its declaration record");
    return id;
  }

  /** Find a declaration in this session; unassigned identities are errors. */
  const DeclarationRecord& get(const DeclarationId& id) const {
    auto it = records_.find(id);
    if (it == records_.end())
      logAndThrowError("Declaration identity does not belong to this analysis");
    return it->second;
  }

  /** Return an existing declaration ID unchanged, or an unset ID if absent. */
  DeclarationId find(const DeclarationId& id) const {
    return records_.contains(id) ? id : DeclarationId{};
  }

  /** Return a declaration in allocation order without interpreting its ID. */
  const DeclarationId& idAt(size_t index) const { return order_.at(index); }

  /** Return the stable name used when exporting this declaration. */
  std::optional<DeclarationId> exportedId(const DeclarationId& id) const {
    if (get(id).imported) return id;
    auto found = exportIds_.find(id);
    if (found == exportIds_.end()) return std::nullopt;
    return found->second;
  }

  /** Assign a stable artifact name without changing the declaration's session
   * ID. */
  void bindExportId(const DeclarationId& id, const DeclarationId& exported,
                    const std::string& bundleHash = {}) {
    const auto& record = get(id);
    if (!exported)
      logAndThrowError("Cannot export an unassigned declaration identity");
    if (auto existing = exportedId(id); existing && *existing != exported)
      logAndThrowError("Declaration already has another export identity");
    auto owner = exportOwners_.find(exported);
    if ((owner != exportOwners_.end() && owner->second != id) ||
        (find(exported) && exported != id))
      logAndThrowError(
          "Export identity already belongs to another declaration");
    if (!record.bundleHash.empty() && record.bundleHash != bundleHash)
      logAndThrowError("Declaration already belongs to another bundle");
    exportIds_.emplace(id, exported);
    exportOwners_.emplace(exported, id);
    records_.at(id).bundleHash = bundleHash;
  }

  /** Return how many declarations this session has allocated. */
  size_t size() const { return records_.size(); }
};

}  // namespace sun::semantic_analysis
