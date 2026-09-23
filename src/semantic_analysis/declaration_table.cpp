#include "semantic_analysis/declaration_table.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Keeps import indexing details local to this translation unit. */
namespace {
/** Stores record positions and validated keys for one import batch. */
struct ImportIndex {
  std::unordered_map<std::string, size_t> keyToIndex;
  std::vector<PortableDeclarationKey> keys;
};

/** Validate imported keys and kinds, and index their record positions. */
ImportIndex indexImportRecords(
    const std::vector<LibraryDeclarationRecord>& records) {
  std::unordered_map<std::string, size_t> keyToIndex;
  keyToIndex.reserve(records.size());
  std::vector<PortableDeclarationKey> keys(records.size());
  for (size_t i = 0; i < records.size(); ++i) {
    keys[i] = PortableDeclarationKey::fromString(records[i].key);
    if (records[i].kind > static_cast<uint32_t>(DeclarationKind::Alias))
      logAndThrowError("Imported declaration has an unknown kind");
    keyToIndex.try_emplace(records[i].key, i);
  }
  return {std::move(keyToIndex), std::move(keys)};
}

}  // namespace

void DeclarationTable::importLibraryDeclarationRecords(
    const std::vector<LibraryDeclarationRecord>& records) {
  auto [keyToIndex, keys] = indexImportRecords(records);

  // Prepare data structures for tracking pending dependencies and
  // ready-to-process records.
  std::vector<size_t> pending(records.size());
  std::vector<std::vector<size_t>> dependents(records.size());
  std::deque<size_t> ready;
  for (size_t i = 0; i < records.size(); ++i) {
    for (const auto* link : {&records[i].owner, &records[i].module}) {
      if (link->empty()) continue;
      auto found = keyToIndex.find(*link);
      if (found != keyToIndex.end()) {
        ++pending[i];
        dependents[found->second].push_back(i);
      } else if (!findPortable(PortableDeclarationKey::fromString(*link))) {
        logAndThrowError("Imported declaration refers to a missing owner");
      }
    }
    if (!pending[i]) ready.push_back(i);
  }

  // Resolve an owner or module key, preserving absent references.
  auto reference = [&](const std::string& key) {
    return key.empty() ? DeclarationId{}
                       : findPortable(PortableDeclarationKey::fromString(key));
  };

  // Process the records in a topological order based on their dependencies.
  size_t restored = 0;
  while (!ready.empty()) {
    const auto i = ready.front();
    ready.pop_front();
    const auto& record = records[i];
    importLibraryDeclaration(keys[i], static_cast<DeclarationKind>(record.kind),
                             record.name, reference(record.owner),
                             reference(record.module), record.bundleHash);
    ++restored;
    for (auto dependent : dependents[i])
      if (--pending[dependent] == 0) ready.push_back(dependent);
  }

  if (restored != records.size())
    logAndThrowError("Imported declaration ownership contains a cycle");
}

}  // namespace sun::semantic_analysis
