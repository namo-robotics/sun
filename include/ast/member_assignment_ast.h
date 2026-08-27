// member_assignment_ast.h — MemberAssignmentAST class

#pragma once

#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"

// Member assignment: object.field = value
class MemberAssignmentAST : public ExprAST {
  std::unique_ptr<ExprAST> object;  // The object (can be 'this' or any expr)
  std::string memberName;           // The field name
  std::unique_ptr<ExprAST> value;   // The value to assign

 protected:
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<MemberAccessAnalysis>();
    }
  }

 private:
  MemberAccessAnalysis& memberAnalysis() const {
    ensureAnalysis();
    return static_cast<MemberAccessAnalysis&>(*analysis_);
  }

 public:
  MemberAssignmentAST(std::unique_ptr<ExprAST> obj, std::string member,
                      std::unique_ptr<ExprAST> val)
      : object(std::move(obj)),
        memberName(std::move(member)),
        value(std::move(val)) {}

  ASTNodeType getType() const override {
    return ASTNodeType::MEMBER_ASSIGNMENT;
  }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(object);
    fn(value);
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

  // Resolved symbol name for a write to a module global (set by semantic
  // analysis); empty for an ordinary field write.
  void setQualifiedName(sun::QualifiedName name) const {
    memberAnalysis().qualifiedName = std::move(name);
  }
  const sun::QualifiedName& getQualifiedName() const {
    return memberAnalysis().qualifiedName;
  }

  // What becomes of the value the field held before this write: it is
  // dropped, dropped only if the storage is not all zero, or not dropped at
  // all because the field has never held a value. Set by semantic analysis.
  void setFieldWriteKind(sun::FieldWriteKind kind) const {
    memberAnalysis().fieldWrite = kind;
  }
  sun::FieldWriteKind fieldWriteKind() const {
    return memberAnalysis().fieldWrite;
  }
};
