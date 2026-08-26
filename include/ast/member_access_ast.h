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

  void forEachChildSlot(const ChildSlotFn& fn) override { fn(object); }
  std::unique_ptr<ExprAST> releaseObject() { return std::move(object); }
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

  // Resolved types of the actual variadic arguments for a generic method call
  // with an `args...` pack (set by semantic analyzer). They are part of the
  // specialization's identity, and so of its name.
  void setResolvedVariadicArgTypes(std::vector<sun::TypePtr> types) const {
    memberAnalysis().resolvedVariadicArgTypes = std::move(types);
  }
  const std::vector<sun::TypePtr>& getResolvedVariadicArgTypes() const {
    return memberAnalysis().resolvedVariadicArgTypes;
  }

  // Bound method reference: method used in value position (set by semantic
  // analyzer); the resolved type is then a LambdaType.
  void setIsBoundMethodRef(bool value) const {
    memberAnalysis().isBoundMethodRef = value;
  }
  bool isBoundMethodRef() const {
    return analysis_ &&
           static_cast<MemberAccessAnalysis&>(*analysis_).isBoundMethodRef;
  }

  // The symbol this access denotes — a module's function or variable, or the
  // specialization instantiated for a generic call (set by the semantic
  // analyzer). Codegen calls this name; it never spells one itself.
  void setQualifiedName(sun::QualifiedName name) const {
    memberAnalysis().qualifiedName = std::move(name);
  }
  const sun::QualifiedName& getQualifiedName() const {
    return memberAnalysis().qualifiedName;
  }
  bool hasQualifiedName() const {
    return analysis_ && !static_cast<MemberAccessAnalysis&>(*analysis_)
                             .qualifiedName.empty();
  }

  std::string dotLabel() const override {
    return "MemberAccess\n." + memberName;
  }
};
