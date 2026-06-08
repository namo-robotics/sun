// member_assignment_ast.h — MemberAssignmentAST class

#pragma once

#include <memory>
#include <string>

#include "ast/expr_ast.h"

// Member assignment: object.field = value
class MemberAssignmentAST : public ExprAST {
  std::unique_ptr<ExprAST> object;  // The object (can be 'this' or any expr)
  std::string memberName;           // The field name
  std::unique_ptr<ExprAST> value;   // The value to assign

 public:
  MemberAssignmentAST(std::unique_ptr<ExprAST> obj, std::string member,
                      std::unique_ptr<ExprAST> val)
      : object(std::move(obj)),
        memberName(std::move(member)),
        value(std::move(val)) {}

  ASTNodeType getType() const override {
    return ASTNodeType::MEMBER_ASSIGNMENT;
  }
  std::string toString() const override {
    return object->toString() + "." + memberName + " = " + value->toString();
  }

  const ExprAST* getObject() const { return object.get(); }
  const std::string& getMemberName() const { return memberName; }
  const ExprAST* getValue() const { return value.get(); }
  std::string dotLabel() const override {
    return "MemberAssign\n." + memberName;
  }
};
