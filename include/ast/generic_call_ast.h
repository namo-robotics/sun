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

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::semantic_analysis::TypePtr;

/**
 * Generic function call: create<Type>(args...) or create<Type1, Type2>(args...)
 * Used for generic free functions like create<T>, destroy, etc.
 */
class GenericCallAST : public ExprAST {
  std::string functionName;  // e.g., "create", "destroy"
  std::vector<std::unique_ptr<TypeAnnotation>>
      typeArguments;                           // All type parameters
  std::vector<std::unique_ptr<ExprAST>> args;  // Function arguments

 protected:
  /**
   * Override to allocate GenericCallAnalysis instead of base ExprAnalysis
   */
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<GenericCallAnalysis>();
    }
  }

 private:
  /**
   * Access as GenericCallAnalysis
   */
  GenericCallAnalysis& gcAnalysis() const {
    ensureAnalysis();
    return static_cast<GenericCallAnalysis&>(*analysis_);
  }

 public:
  /** Creates this syntax node from its operands and declaration information. */
  GenericCallAST(std::string name,
                 std::vector<std::unique_ptr<TypeAnnotation>> typeArgs,
                 std::vector<std::unique_ptr<ExprAST>> arguments)
      : functionName(std::move(name)), args(std::move(arguments)) {
    typeArguments = std::move(typeArgs);
  }

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::GENERIC_CALL; }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    for (auto& arg : args) fn(arg);
  }
  /** Returns a readable representation for diagnostics and debugging. */
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

  /** Returns the function name stored by this object. */
  const std::string& getFunctionName() const { return functionName; }
  /** Provides the concrete types supplied for generic specialization. */
  const std::vector<std::unique_ptr<TypeAnnotation>>& getTypeArguments() const {
    return typeArguments;
  }
  /** Provides the ordered arguments associated with this expression. */
  const std::vector<std::unique_ptr<ExprAST>>& getArgs() const { return args; }

  /**
   * Mutable access to the argument list (used to expand variadic packs into
   * concrete args during semantic analysis).
   */
  std::vector<std::unique_ptr<ExprAST>>& getArgsMutable() { return args; }

  /**
   * Resolved type arguments (set by semantic analyzer after type param
   * substitution)
   * How each argument reaches its parameter (set by the semantic analyzer
   * once the specialization is known; one entry per argument)
   */
  void setArgConversions(
      std::vector<sun::semantic_analysis::ArgConversion> conversions) const {
    gcAnalysis().argConversions = std::move(conversions);
  }
  /** Returns the arg conversions stored by this object. */
  const std::vector<sun::semantic_analysis::ArgConversion>& getArgConversions()
      const {
    return gcAnalysis().argConversions;
  }

  /** Updates the resolved type args stored by this object. */
  void setResolvedTypeArgs(std::vector<TypePtr> types) const {
    gcAnalysis().resolvedTypeArgs = std::move(types);
  }
  /** Returns the resolved type args stored by this object. */
  const std::vector<TypePtr>& getResolvedTypeArgs() const {
    return gcAnalysis().resolvedTypeArgs;
  }
  /** Reports whether this object has resolved type args. */
  bool hasResolvedTypeArgs() const {
    return analysis_ && !static_cast<GenericCallAnalysis&>(*analysis_)
                             .resolvedTypeArgs.empty();
  }

  /**
   * Generic function AST (set by semantic analyzer)
   */
  void setGenericFunctionAST(const FunctionAST* ast) const {
    gcAnalysis().genericFunctionAST = ast;
  }
  /** Returns the generic function syntax tree stored by this object. */
  const FunctionAST* getGenericFunctionAST() const {
    return analysis_ ? static_cast<GenericCallAnalysis&>(*analysis_)
                           .genericFunctionAST
                     : nullptr;
  }
  /** Reports whether this object has generic function syntax tree. */
  bool hasGenericFunctionAST() const {
    return analysis_ &&
           static_cast<GenericCallAnalysis&>(*analysis_).genericFunctionAST !=
               nullptr;
  }

  /** Record the instantiated callable signature used to check this call. */
  void setResolvedCalleeType(TypePtr type) const {
    gcAnalysis().resolvedCalleeType = std::move(type);
  }

  /** Return the concrete callable signature, or null in an abstract template.
   */
  TypePtr getResolvedCalleeType() const {
    return analysis_ ? static_cast<const GenericCallAnalysis&>(*analysis_)
                           .resolvedCalleeType
                     : nullptr;
  }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override {
    return "GenericCall\n" + functionName + "<...>()";
  }
};

}  // namespace sun::ast
