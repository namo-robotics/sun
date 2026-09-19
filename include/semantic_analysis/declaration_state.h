#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast_fwd.h"
#include "semantic_analysis/declaration_id.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::ast::ClassDefinitionAST;

class SemanticScopeBase;

/** Declaration bookkeeping shared by collection and body checking. */
class DeclarationState {
 public:
  /** True when this class's shape was already registered by the pre-pass. */
  bool hasClassShape(sun::semantic_analysis::DeclarationId declaration) const {
    return preRegisteredClassShapes_.count(declaration) > 0;
  }

  /** Record a class shape and report whether it is new. */
  bool noteClassShape(sun::semantic_analysis::DeclarationId declaration) {
    return preRegisteredClassShapes_.insert(declaration).second;
  }

  /** Check whether a source name has already been declared in this scope.
   */
  bool isDeclared(const std::string &name,
                  const SemanticScopeBase *scopeIdentity) const {
    auto scope = definedSymbols_.find(scopeIdentity);
    return scope != definedSymbols_.end() && scope->second.count(name) > 0;
  }

  /** Record a source name in its scope; reopened modules reuse their scope. */
  void noteDeclared(const std::string &name,
                    const SemanticScopeBase *scopeIdentity) {
    definedSymbols_[scopeIdentity].insert(name);
  }

  // ---- Partial classes ---------------------------------------------------
  //
  // A partial class adds methods to a primary declared elsewhere. When the
  // primary has not been analyzed yet, the extension waits here for it.

  /** Hold an extension until its primary class is analyzed. */
  void deferExtension(const std::string &className,
                      ClassDefinitionAST *extension) {
    pendingExtensions_[className].push_back(extension);
  }

  /** The extensions waiting for this class, or nullptr when there are none. */
  const std::vector<ClassDefinitionAST *> *pendingExtensions(
      const std::string &className) const {
    auto it = pendingExtensions_.find(className);
    return it == pendingExtensions_.end() ? nullptr : &it->second;
  }

  /** Drop the extensions for a class once they have been merged into it. */
  void clearPendingExtensions(const std::string &className) {
    pendingExtensions_.erase(className);
  }

 private:
  std::unordered_map<const SemanticScopeBase *, std::unordered_set<std::string>>
      definedSymbols_;

  // Pending class extensions collected during import processing.
  // Maps class name → list of extension ASTs to merge when primary is analyzed.
  std::unordered_map<std::string, std::vector<ClassDefinitionAST *>>
      pendingExtensions_;

  // Classes (by declaration identity) whose fields and method signatures were
  // registered by the pre-pass. The sequential pass skips re-adding them and
  // only analyzes bodies.
  std::unordered_set<sun::semantic_analysis::DeclarationId>
      preRegisteredClassShapes_;
};

}  // namespace sun::semantic_analysis
