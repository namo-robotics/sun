// member_access_ast.h — MemberAccessAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/analysis.h"
#include "ast/ast_utils.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::semantic_analysis::TypePtr;

/**
 * Member access expression: object.fieldName or object.methodName
 * For method calls, this is wrapped in CallExprAST
 * For generic method calls like object.method&lt;T&gt;(), typeArguments will be
 * populated
 */
class MemberAccessAST : public ExprAST {
  std::unique_ptr<ExprAST> object;  // The object being accessed
  std::string memberName;           // The field or method name
  std::vector<std::unique_ptr<TypeAnnotation>>
      typeArguments;  // Generic type arguments for methods

 protected:
  /**
   * Override to allocate MemberAccessAnalysis instead of base ExprAnalysis
   */
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<MemberAccessAnalysis>();
    }
  }

 private:
  /**
   * Access as MemberAccessAnalysis
   */
  MemberAccessAnalysis& memberAnalysis() const {
    ensureAnalysis();
    return static_cast<MemberAccessAnalysis&>(*analysis_);
  }

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  MemberAccessAST(std::unique_ptr<ExprAST> obj, std::string member,
                  std::vector<std::unique_ptr<TypeAnnotation>> typeArgs = {})
      : object(std::move(obj)),
        memberName(std::move(member)),
        typeArguments(std::move(typeArgs)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::MEMBER_ACCESS; }
  /** Returns a readable representation for diagnostics and debugging. */
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

  /** Provides the receiver expression used for member access. */
  const ExprAST* getObject() const { return object.get(); }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override { fn(object); }
  /** Transfers ownership of the receiver expression to the caller. */
  std::unique_ptr<ExprAST> releaseObject() { return std::move(object); }
  /** Returns the member name to resolve on the receiver. */
  const std::string& getMemberName() const { return memberName; }
  /** Reports whether this object has type arguments. */
  bool hasTypeArguments() const { return !typeArguments.empty(); }
  /** Provides the concrete types supplied for generic specialization. */
  const std::vector<std::unique_ptr<TypeAnnotation>>& getTypeArguments() const {
    return typeArguments;
  }

  /**
   * Resolved type arguments for generic method calls (set by semantic analyzer)
   */
  void setResolvedTypeArgs(std::vector<TypePtr> types) const {
    memberAnalysis().resolvedTypeArgs = std::move(types);
  }
  /** Returns the concrete generic type arguments. */
  const std::vector<TypePtr>& getResolvedTypeArgs() const {
    return memberAnalysis().resolvedTypeArgs;
  }
  /** Reports whether this object has resolved type args. */
  bool hasResolvedTypeArgs() const {
    return analysis_ && !static_cast<MemberAccessAnalysis&>(*analysis_)
                             .resolvedTypeArgs.empty();
  }

  /**
   * Resolved types of the actual variadic arguments for a generic method call
   * with an `args...` pack (set by semantic analyzer). They are part of the
   * specialization's identity, and so of its name.
   */
  void setResolvedVariadicArgTypes(std::vector<TypePtr> types) const {
    memberAnalysis().resolvedVariadicArgTypes = std::move(types);
  }
  /** Returns the concrete variadic argument types. */
  const std::vector<TypePtr>& getResolvedVariadicArgTypes() const {
    return memberAnalysis().resolvedVariadicArgTypes;
  }

  /**
   * Bound method reference: method used in value position (set by semantic
   * analyzer); the resolved type is then a LambdaType.
   */
  void setIsBoundMethodRef(bool value) const {
    memberAnalysis().isBoundMethodRef = value;
  }
  /** Reports whether this syntax node represents a bound method reference. */
  bool isBoundMethodRef() const {
    return analysis_ &&
           static_cast<MemberAccessAnalysis&>(*analysis_).isBoundMethodRef;
  }

  /**
   * The symbol this access denotes — a module's function or variable, or the
   * specialization instantiated for a generic call (set by the semantic
   * analyzer). Codegen calls this name; it never spells one itself.
   */
  void setQualifiedName(sun::semantic_analysis::QualifiedName name) const {
    memberAnalysis().qualifiedName = std::move(name);
  }
  /** Returns the declaration name together with its enclosing scopes. */
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return memberAnalysis().qualifiedName;
  }
  /** Reports whether a name including the enclosing scopes has been assigned. */
  bool hasQualifiedName() const {
    return analysis_ && !static_cast<MemberAccessAnalysis&>(*analysis_)
                             .qualifiedName.empty();
  }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override {
    return "MemberAccess\n." + memberName;
  }
};

}  // namespace sun::ast
