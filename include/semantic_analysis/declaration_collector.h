// declaration_collector.h — The pre-pass that registers every declaration in
// a block before any body is analyzed.
//
// Sun does not require a declaration to appear before its use, so a block's
// types, class shapes and function signatures all have to be known before the
// first body is looked at. That is three sub-passes over the same block: the
// types (so signatures can name them), then the class shapes (so a
// specialization triggered from a signature can call any method in the
// block), then the signatures themselves.
//
// The pre-pass also owns what "already declared" means for the rest of the
// run: which symbols exist at module level, which class shapes are registered,
// and which partial-class extensions are waiting for their primary.

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "semantic_analysis/semantic_context.h"

class SemanticAnalyzer;

/**
 * Registers the declarations of a block ahead of its bodies, and keeps the
 * bookkeeping that tells later passes what has already been declared.
 */
class DeclarationCollector {
 public:
  DeclarationCollector(SemanticContext &ctx, SemanticAnalyzer &sema)
      : ctx_(ctx), sema_(sema) {}

  /**
   * Declaration pre-pass: register all functions, classes, interfaces, enums,
   * and modules in a block before analyzing bodies. This allows forward
   * references between declarations at the same scope level.
   */
  void collectDeclarations(BlockExprAST &block);

  /** Register one named function's signature in the current scope. */
  void collectFunctionSignature(FunctionAST &func);

  /**
   * Declaration-collection pre-pass: register a block's enums (and generic
   * enum templates) so function signatures collected afterwards can resolve
   * enum-typed parameters/returns.
   */
  void collectEnumDeclarations(const BlockExprAST &block);

  /**
   * Register a non-generic class's fields and method signatures on its
   * ClassType so that any body analyzed afterwards — including bodies of
   * generic specializations triggered from function signatures — can call
   * its methods regardless of declaration order.
   */
  void registerClassShape(ClassDefinitionAST &classDef,
                          const sun::QualifiedName &qualifiedClass,
                          std::shared_ptr<sun::ClassType> classType);

  /** True when this class's shape was already registered by the pre-pass. */
  bool hasClassShape(const std::string &mangledClassName) const {
    return preRegisteredClassShapes_.count(mangledClassName) > 0;
  }

  /**
   * Register a module-level variable imported from a .moon bundle. The stub
   * carries a type annotation and a content-hash-scoped qualified name, but
   * no initializer — the storage is in the bundle.
   */
  void registerPrecompiledModuleVariable(VariableCreationAST &varCreate);

  /** Bind a `using` declaration in the current scope (idempotent). */
  void registerUsing(UsingAST &usingDecl);

  // ---- Module-level redefinition -----------------------------------------
  //
  // A class, interface or enum may only be declared once per module. Names
  // are recorded as they are analyzed, and a second declaration is an error.

  /** True when this module-level name has already been declared. */
  bool isDeclared(const std::string &name) const {
    return definedSymbols_.count(name) > 0;
  }

  /** Record a module-level name so a later redeclaration is caught. */
  void noteDeclared(const std::string &name) { definedSymbols_.insert(name); }

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
  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;

  // Symbols defined at module level (depth 0) — used to detect redefinition
  // errors for classes, interfaces, and enums.
  std::unordered_set<std::string> definedSymbols_;

  // Pending class extensions collected during import processing.
  // Maps class name → list of extension ASTs to merge when primary is analyzed.
  std::unordered_map<std::string, std::vector<ClassDefinitionAST *>>
      pendingExtensions_;

  // Classes (by mangled name) whose fields and method signatures were
  // registered by the pre-pass. The sequential pass skips re-adding them and
  // only analyzes bodies.
  std::unordered_set<std::string> preRegisteredClassShapes_;

  // Depth of the pre-pass. Generic class specializations requested while > 0
  // register their type and method signatures immediately (so shapes and
  // signatures can refer to them) but defer method-body analysis to the end of
  // the outermost pre-pass, once every declaration in the program is
  // registered.
  int prepassDepth_ = 0;
};
