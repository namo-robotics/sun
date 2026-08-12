// try_catch_expr_ast.h — TryCatchExprAST class

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"

// Catch clause for try-catch expression
// Represents: catch (name: Type) { body }
struct CatchClause {
  std::string bindingName;                    // variable name for error binding
  std::optional<TypeAnnotation> bindingType;  // type annotation (e.g., IError)
  std::unique_ptr<BlockExprAST> body;         // the catch body

  // Filled in by semantic analysis, consumed by codegen for typed matching:
  bool isCatchAll = false;           // true for `catch (e: IError)` (matches any)
  std::string resolvedMangledName;   // concrete error class's mangled name
                                     // (empty when isCatchAll)

  CatchClause() = default;
  CatchClause(CatchClause&&) = default;
  CatchClause& operator=(CatchClause&&) = default;
};

// Try-catch expression: try { ... } catch (e: A) { ... } catch (e: IError) { ... }
// Supports multiple typed catch handlers, tested in source order.
class TryCatchExprAST : public ExprAST {
  std::unique_ptr<BlockExprAST> tryBlock;    // The try block
  std::vector<CatchClause> catchClauses;     // One or more catch handlers

 public:
  TryCatchExprAST(std::unique_ptr<BlockExprAST> tryBlk,
                  std::vector<CatchClause> catchCls)
      : tryBlock(std::move(tryBlk)), catchClauses(std::move(catchCls)) {}

  ASTNodeType getType() const override { return ASTNodeType::TRY_CATCH; }
  std::string toString() const override {
    std::string result = "try " + tryBlock->toString();
    for (const auto& c : catchClauses) {
      result += " catch (" + c.bindingName;
      if (c.bindingType) result += ": " + c.bindingType->toString();
      result += ") " + c.body->toString();
    }
    return result;
  }

  const BlockExprAST& getTryBlock() const { return *tryBlock; }
  const std::vector<CatchClause>& getCatchClauses() const {
    return catchClauses;
  }
  std::vector<CatchClause>& getCatchClausesMutable() { return catchClauses; }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (tryBlock) tryBlock->forEachChildSlot(fn);
    for (auto& clause : catchClauses) {
      if (clause.body) clause.body->forEachChildSlot(fn);
    }
  }
  std::string dotLabel() const override { return "TryCatch"; }
};
