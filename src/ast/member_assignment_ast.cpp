// member_assignment_ast.cpp — MemberAssignmentAST clone implementation

#include "ast/member_assignment_ast.h"

std::unique_ptr<ExprAST> MemberAssignmentAST::clone() const {
  auto copy = std::make_unique<MemberAssignmentAST>(object->clone(), memberName,
                                                    value->clone());
  cloneBase(*copy);
  return copy;
}
