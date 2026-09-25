// try_catch_expr_ast.h — TryCatchExprAST class

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Catch clause for try-catch expression
 * Represents: catch (name: ref Type) { body }
 */
struct CatchClause {
  std::string bindingName;                    // variable name for error binding
  std::optional<TypeAnnotation>
      bindingType;  // type annotation (e.g., ref IError)
  std::unique_ptr<BlockExprAST> body;         // the catch body

  // Filled in by semantic analysis, consumed by codegen for typed matching:
  bool isCatchAll = false;  // true for `catch (e: ref IError)` (matches any)
  sun::types::TypePtr resolvedType;

  /** Creates an instance with its default state. */
  CatchClause() = default;
  /** Creates an instance with its default state. */
  CatchClause(CatchClause&&) = default;
  /** Transfers the stored state from another instance during move assignment.
   */
  CatchClause& operator=(CatchClause&&) = default;
  mutable sun::semantic_analysis::DeclarationIdentity declaration{};
};

/**
 * Try-catch expression: try { ... } catch (e: ref A) { ... } catch (e: ref
 * IError) {
 * ... } Supports multiple typed catch handlers, tested in source order.
 */
class TryCatchExprAST : public ExprAST {
  std::unique_ptr<BlockExprAST> tryBlock;  // The try block
  std::vector<CatchClause> catchClauses;   // One or more catch handlers

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  TryCatchExprAST(std::unique_ptr<BlockExprAST> tryBlk,
                  std::vector<CatchClause> catchCls)
      : tryBlock(std::move(tryBlk)), catchClauses(std::move(catchCls)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::TRY_CATCH; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result = "try " + tryBlock->toString();
    for (const auto& c : catchClauses) {
      result += " catch (" + c.bindingName;
      if (c.bindingType) result += ": " + c.bindingType->toString();
      result += ") " + c.body->toString();
    }
    return result;
  }

  /** Returns the try block stored by this object. */
  const BlockExprAST& getTryBlock() const { return *tryBlock; }
  /** Returns the catch clauses stored by this object. */
  const std::vector<CatchClause>& getCatchClauses() const {
    return catchClauses;
  }
  /** Returns the catch clauses mutable stored by this object. */
  std::vector<CatchClause>& getCatchClausesMutable() { return catchClauses; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (tryBlock) tryBlock->forEachChildSlot(fn);
    for (auto& clause : catchClauses) {
      if (clause.body) clause.body->forEachChildSlot(fn);
    }
  }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "TryCatch"; }
};

}  // namespace sun::ast
