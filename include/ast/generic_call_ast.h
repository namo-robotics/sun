// generic_call_ast.h — GenericCallAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/analysis.h"
#include "ast/ast_fwd.h"
#include "ast/ast_utils.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"

// Generic function call: create<Type>(args...) or create<Type1, Type2>(args...)
// Used for generic free functions like create<T>, destroy, etc.
class GenericCallAST : public ExprAST {
  std::string functionName;  // e.g., "create", "destroy"
  std::vector<std::unique_ptr<TypeAnnotation>>
      typeArguments;                           // All type parameters
  std::vector<std::unique_ptr<ExprAST>> args;  // Function arguments

 protected:
  // Override to allocate GenericCallAnalysis instead of base ExprAnalysis
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<GenericCallAnalysis>();
    }
  }

 private:
  // Access as GenericCallAnalysis
  GenericCallAnalysis& gcAnalysis() const {
    ensureAnalysis();
    return static_cast<GenericCallAnalysis&>(*analysis_);
  }

 public:
  GenericCallAST(std::string name,
                 std::vector<std::unique_ptr<TypeAnnotation>> typeArgs,
                 std::vector<std::unique_ptr<ExprAST>> arguments)
      : functionName(std::move(name)), args(std::move(arguments)) {
    typeArguments = std::move(typeArgs);
  }

  ASTNodeType getType() const override { return ASTNodeType::GENERIC_CALL; }
  std::string toString() const override {
    std::string result = functionName + "<";
    if (!typeArguments.empty()) {
      for (size_t i = 0; i < typeArguments.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeArguments[i]->toString();
      }
    }
    result += ">(";
    for (size_t i = 0; i < args.size(); ++i) {
      if (i > 0) result += ", ";
      result += args[i]->toString();
    }
    return result + ")";
  }

  const std::string& getFunctionName() const { return functionName; }
  const std::vector<std::unique_ptr<TypeAnnotation>>& getTypeArguments() const {
    return typeArguments;
  }
  const std::vector<std::unique_ptr<ExprAST>>& getArgs() const { return args; }

  // Resolved type arguments (set by semantic analyzer after type param
  // substitution)
  void setResolvedTypeArgs(std::vector<sun::TypePtr> types) const {
    gcAnalysis().resolvedTypeArgs = std::move(types);
  }
  const std::vector<sun::TypePtr>& getResolvedTypeArgs() const {
    return gcAnalysis().resolvedTypeArgs;
  }
  bool hasResolvedTypeArgs() const {
    return analysis_ && !static_cast<GenericCallAnalysis&>(*analysis_)
                             .resolvedTypeArgs.empty();
  }

  // Generic function AST (set by semantic analyzer)
  void setGenericFunctionAST(const FunctionAST* ast) const {
    gcAnalysis().genericFunctionAST = ast;
  }
  const FunctionAST* getGenericFunctionAST() const {
    return analysis_ ? static_cast<GenericCallAnalysis&>(*analysis_)
                           .genericFunctionAST
                     : nullptr;
  }
  bool hasGenericFunctionAST() const {
    return analysis_ &&
           static_cast<GenericCallAnalysis&>(*analysis_).genericFunctionAST !=
               nullptr;
  }

  std::string dotLabel() const override {
    return "GenericCall\n" + functionName + "<...>()";
  }
  std::unique_ptr<ExprAST> clone() const override;
};
