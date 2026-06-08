// try_catch_expr_ast.h — TryCatchExprAST class

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"

// Catch clause for try-catch expression
// Represents: catch (name: Type) { body }
struct CatchClause {
  std::string bindingName;                    // variable name for error binding
  std::optional<TypeAnnotation> bindingType;  // type annotation (e.g., IError)
  std::unique_ptr<BlockExprAST> body;         // the catch body

  CatchClause() = default;
  CatchClause(CatchClause&&) = default;
  CatchClause& operator=(CatchClause&&) = default;
};

// Try-catch expression: try { ... } catch (e: IError) { ... }
// Modern exception handling syntax
class TryCatchExprAST : public ExprAST {
  std::unique_ptr<BlockExprAST> tryBlock;  // The try block
  CatchClause catchClause;                 // The catch handler

 public:
  TryCatchExprAST(std::unique_ptr<BlockExprAST> tryBlk, CatchClause catchCls)
      : tryBlock(std::move(tryBlk)), catchClause(std::move(catchCls)) {}

  ASTNodeType getType() const override { return ASTNodeType::TRY_CATCH; }
  std::string toString() const override {
    std::string result =
        "try " + tryBlock->toString() + " catch (" + catchClause.bindingName;
    if (catchClause.bindingType)
      result += ": " + catchClause.bindingType->toString();
    result += ") " + catchClause.body->toString();
    return result;
  }

  const BlockExprAST& getTryBlock() const { return *tryBlock; }
  const CatchClause& getCatchClause() const { return catchClause; }
  std::string dotLabel() const override { return "TryCatch"; }
};
