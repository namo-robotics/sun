/**
 * Resolves class shapes and concrete signatures after type registration.
 * Declaration identities, qualified names, module scopes, nominal types, and
 * generic templates must already be registered before this pass runs.
 */

#pragma once

#include <string>
#include <vector>

#include "semantic_analysis/semantic_context.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
class SemanticAnalyzer;

}  // namespace sun::semantic_analysis

/** Provides the ordered preparation and registration passes for analysis. */
namespace sun::semantic_analysis::passes {
using sun::ast::BlockExprAST;

/**
 * Resolve declarations before bodies, using bookkeeping in SemanticContext.
 *
 * TypeRegistrationPass supplies types and templates first. This pass binds
 * imports, fills class shapes, and resolves concrete signatures. While it runs (see
 * GenericSpecializer::isInDeclarationPrepass), a global reached through a
 * type annotation may not call a function, because its callee has no
 * analyzed body yet, and a class specialization registers its signatures
 * but holds its method bodies until the pass ends.
 */
class DeclarationCollectionPass {
 public:
  /** Share declaration state and signature-checking helpers with analysis. */
  DeclarationCollectionPass(SemanticContext &ctx, SemanticAnalyzer &sema)
      : ctx_(ctx), sema_(sema) {}

  /**
   * Bind imports and resolve class shapes and concrete function signatures
   * in the scopes prepared by TypeRegistrationPass before checking bodies. This
   * allows forward references between declarations at the same scope level,
   * and lets a `using` anywhere in the block serve every declaration in it.
   */
  void run(BlockExprAST &block);

  /** Register one named function's signature in the current scope. */
  void collectFunctionSignature(sun::ast::FunctionAST &func);

  /**
   * Register a non-generic class's fields and method signatures on its
   * ClassType so that any body analyzed afterwards — including bodies of
   * generic specializations triggered from function signatures — can call
   * its methods regardless of declaration order.
   */
  void registerClassShape(
      sun::ast::ClassDefinitionAST &classDef,
      const sun::semantic_analysis::QualifiedName &qualifiedClass,
      std::shared_ptr<sun::types::ClassType> classType);

  /**
   * Register a module-level variable imported from a .moon bundle. The stub
   * carries a type annotation and a content-hash-scoped qualified name, but
   * no initializer — the storage is in the bundle.
   */
  void registerPrecompiledModuleVariable(
      sun::ast::VariableCreationAST &varCreate);

  /** Register one C extern global before any function body is analyzed. */
  void collectExternVariable(sun::ast::VariableCreationAST &varCreate);

  /** Bind a `using` declaration in the current scope (idempotent). */
  void registerUsing(sun::ast::UsingAST &usingDecl);

 private:
  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;

  // Depth of the pre-pass. Generic class specializations requested while > 0
  // register their type and method signatures immediately (so shapes and
  // signatures can refer to them) but defer method-body analysis to the end of
  // the outermost pre-pass, once every declaration in the program is
  // registered.
  int prepassDepth_ = 0;
};

}  // namespace sun::semantic_analysis::passes
