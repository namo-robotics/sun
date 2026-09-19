// declaration_collection_pass.h — The pre-pass that registers every declaration
// in a block before any body is analyzed. Declaration naming runs first; this
// pass consumes the qualified names already attached to the AST.
//
// Sun does not require a declaration to appear before its use, so a block's
// types, class shapes and function signatures all have to be known before the
// first body is looked at. That is three sub-passes over the same block: the
// types (so signatures can name them), then the class shapes (so a
// specialization triggered from a signature can call any method in the
// block), then the signatures themselves.
//
// Declaration bookkeeping lives in the shared SemanticContext.

#pragma once

#include <string>
#include <vector>

#include "semantic_analysis/semantic_context.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::ast::BlockExprAST;

class SemanticAnalyzer;

/**
 * Register declarations before bodies, using bookkeeping in SemanticContext.
 */
class DeclarationCollectionPass {
 public:
  /** Share declaration state and signature-checking helpers with analysis. */
  DeclarationCollectionPass(SemanticContext &ctx, SemanticAnalyzer &sema)
      : ctx_(ctx), sema_(sema) {}

  /**
   * Declaration pre-pass: register all imports, functions, classes,
   * interfaces, enums, and modules in a block before analyzing bodies. This
   * allows forward references between declarations at the same scope level,
   * and lets a `using` anywhere in the block serve every declaration in it.
   */
  void run(BlockExprAST &block);

  /** Register one named function's signature in the current scope. */
  void collectFunctionSignature(sun::ast::FunctionAST &func);

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
  void registerClassShape(
      sun::ast::ClassDefinitionAST &classDef,
      const sun::semantic_analysis::QualifiedName &qualifiedClass,
      std::shared_ptr<sun::semantic_analysis::ClassType> classType);

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
  /**
   * Register type names through the whole module tree before any class shape
   * is resolved, so sibling modules may name each other's public types.
   */
  void collectTypeNames(BlockExprAST &block);

  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;

  // Depth of the pre-pass. Generic class specializations requested while > 0
  // register their type and method signatures immediately (so shapes and
  // signatures can refer to them) but defer method-body analysis to the end of
  // the outermost pre-pass, once every declaration in the program is
  // registered.
  int prepassDepth_ = 0;
};

}  // namespace sun::semantic_analysis
