/** Stores validated implementation inputs for cross-revision comparisons. */
#pragma once

#include <map>
#include <set>
#include <string>
#include <tuple>

#include "abi/correspondence_checker.h"
#include "moon.pb.h"

/** Checks correspondence between independently compiled declarations. */
namespace sun::abi {
/** Keeps producer evidence separate from executable imported declarations. */
class CorrespondenceEvidence {
 public:
  /** Registers metadata only after its contract and payload have been checked.
   */
  void add(const moon::ModuleMetadata& metadata,
           const std::string& libraryIdentity);
  /** Compares a function, method, or class and its resolved dependencies. */
  CorrespondenceResult compare(const std::string& source,
                               const std::string& target, bool plain) const;

 private:
  std::map<std::string, moon::DeclarationRecord> declarations_;
  std::map<std::string, sun::proto::ast::ASTNode> definitions_;
  std::map<std::string, sun::proto::ast::VariableCreation> globals_;
  std::set<std::string> unsupported_;
  mutable std::map<std::tuple<std::string, std::string, bool>,
                   CorrespondenceResult>
      comparisons_;
  std::map<std::string, std::string> libraryIdentities_;
  /** Retains methods as individually addressable proof definitions. */
  void addDefinition(const sun::proto::ast::ASTNode& definition);
  /** Names a declaration structurally without changing its executable identity.
   */
  std::string correspondenceName(const std::string& key) const;
  /** Compares a dependency graph transactionally; cycles are not cached proofs.
   */
  CorrespondenceResult compareImpl(
      const std::string& source, const std::string& target, bool plain,
      std::set<std::pair<std::string, std::string>>& active) const;
};
}  // namespace sun::abi
