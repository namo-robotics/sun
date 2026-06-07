// member_access_ast.h — MemberAccessAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/analysis.h"
#include "ast/ast_utils.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"

// Member access expression: object.fieldName or object.methodName
// For method calls, this is wrapped in CallExprAST
// For generic method calls like object.method<T>(), typeArguments will be
// populated
class MemberAccessAST : public ExprAST {
  std::unique_ptr<ExprAST> object;  // The object being accessed
  std::string memberName;           // The field or method name
  std::vector<std::unique_ptr<TypeAnnotation>>
      typeArguments;  // Generic type arguments for methods

 protected:
  // Override to allocate MemberAccessAnalysis instead of base ExprAnalysis
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<MemberAccessAnalysis>();
    }
  }

 private:
  // Access as MemberAccessAnalysis
  MemberAccessAnalysis& memberAnalysis() const {
    ensureAnalysis();
    return static_cast<MemberAccessAnalysis&>(*analysis_);
  }

 public:
  MemberAccessAST(std::unique_ptr<ExprAST> obj, std::string member,
                  std::vector<std::unique_ptr<TypeAnnotation>> typeArgs = {})
      : object(std::move(obj)),
        memberName(std::move(member)),
        typeArguments(std::move(typeArgs)) {}

  ASTNodeType getType() const override { return ASTNodeType::MEMBER_ACCESS; }
  std::string toString() const override {
    std::string result = object->toString() + "." + memberName;
    if (!typeArguments.empty()) {
      result += "<";
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toString();
      }
      result += ">";
    }
    return result;
  }

  const ExprAST* getObject() const { return object.get(); }
  const std::string& getMemberName() const { return memberName; }
  bool hasTypeArguments() const { return !typeArguments.empty(); }
  const std::vector<std::unique_ptr<TypeAnnotation>>& getTypeArguments() const {
    return typeArguments;
  }

  // Resolved type arguments for generic method calls (set by semantic analyzer)
  void setResolvedTypeArgs(std::vector<sun::TypePtr> types) const {
    memberAnalysis().resolvedTypeArgs = std::move(types);
  }
  const std::vector<sun::TypePtr>& getResolvedTypeArgs() const {
    return memberAnalysis().resolvedTypeArgs;
  }
  bool hasResolvedTypeArgs() const {
    return analysis_ && !static_cast<MemberAccessAnalysis&>(*analysis_)
                             .resolvedTypeArgs.empty();
  }

  // Resolved qualified name for module member access (set by semantic analyzer)
  void setResolvedQualifiedName(std::string name) const {
    memberAnalysis().resolvedQualifiedName = std::move(name);
  }
  const std::string& getResolvedQualifiedName() const {
    return memberAnalysis().resolvedQualifiedName;
  }
  bool hasResolvedQualifiedName() const {
    return analysis_ && !static_cast<MemberAccessAnalysis&>(*analysis_)
                             .resolvedQualifiedName.empty();
  }

  std::string dotLabel() const override {
    return "MemberAccess\n." + memberName;
  }
  std::unique_ptr<ExprAST> clone() const override;
};
