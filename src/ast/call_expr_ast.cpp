// call_expr_ast.cpp — CallExprAST clone and dotLabel implementations

#include "ast/call_expr_ast.h"

#include "ast/ast_utils.h"
#include "ast/member_access_ast.h"
#include "ast/variable_reference_ast.h"

std::unique_ptr<ExprAST> CallExprAST::clone() const {
  auto copy =
      std::make_unique<CallExprAST>(Callee->clone(), cloneExprVector(Args));
  cloneBase(*copy);
  return copy;
}

std::string CallExprAST::dotLabel() const {
  // Try to get a readable name from the callee
  if (Callee->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto* ref = static_cast<const VariableReferenceAST*>(Callee.get());
    return "Call\n" + ref->getName() + "()";
  }
  if (Callee->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto* ma = static_cast<const MemberAccessAST*>(Callee.get());
    return "Call\n." + ma->getMemberName() + "()";
  }
  return "Call";
}
