// declare_type_ast.h — DeclareTypeAST class

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Declare type statement: declare [Alias =] Type<Args>;
 * Used to explicitly instantiate generic types and optionally create aliases
 */
class DeclareTypeAST : public ExprAST {
  std::optional<std::string> aliasName;  // Optional alias name
  TypeAnnotation typeAnnotation;         // The type to instantiate

 protected:
  /**
   * Override to allocate DeclareTypeAnalysis instead of base ExprAnalysis
   */
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<DeclareTypeAnalysis>();
    }
  }

 private:
  /**
   * Access as DeclareTypeAnalysis
   */
  DeclareTypeAnalysis& declAnalysis() const {
    ensureAnalysis();
    return static_cast<DeclareTypeAnalysis&>(*analysis_);
  }

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  DeclareTypeAST(TypeAnnotation type,
                 std::optional<std::string> alias = std::nullopt)
      : aliasName(std::move(alias)), typeAnnotation(std::move(type)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::DECLARE_TYPE; }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string prefix = isPublic() ? "public " : "";
    if (aliasName) {
      return prefix + "declare " + *aliasName + " = " +
             typeAnnotation.toString();
    }
    return prefix + "declare " + typeAnnotation.toString();
  }

  /** Reports whether this object has alias. */
  bool hasAlias() const { return aliasName.has_value(); }
  /** Returns the alias name stored by this object. */
  const std::string& getAliasName() const { return *aliasName; }
  /** Returns the type annotation stored by this object. */
  const TypeAnnotation& getTypeAnnotation() const { return typeAnnotation; }

  /**
   * Resolved declared type (set by semantic analysis)
   */
  void setResolvedDeclaredType(sun::types::TypePtr type) const {
    declAnalysis().resolvedDeclaredType = std::move(type);
  }
  /** Returns the resolved declared type stored by this object. */
  sun::types::TypePtr getResolvedDeclaredType() const {
    return analysis_ ? static_cast<DeclareTypeAnalysis&>(*analysis_)
                           .resolvedDeclaredType
                     : nullptr;
  }
  /** Reports whether this object has resolved declared type. */
  bool hasResolvedDeclaredType() const {
    return analysis_ &&
           static_cast<DeclareTypeAnalysis&>(*analysis_).resolvedDeclaredType !=
               nullptr;
  }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override {
    std::string label = "DeclareType";
    if (aliasName) label += "\n" + *aliasName;
    return label;
  }
};

}  // namespace sun::ast
