// declare_type_ast.h — DeclareTypeAST class

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"

// Declare type statement: declare [Alias =] Type<Args>;
// Used to explicitly instantiate generic types and optionally create aliases
class DeclareTypeAST : public ExprAST {
  std::optional<std::string> aliasName;  // Optional alias name
  TypeAnnotation typeAnnotation;         // The type to instantiate

 protected:
  // Override to allocate DeclareTypeAnalysis instead of base ExprAnalysis
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<DeclareTypeAnalysis>();
    }
  }

 private:
  // Access as DeclareTypeAnalysis
  DeclareTypeAnalysis& declAnalysis() const {
    ensureAnalysis();
    return static_cast<DeclareTypeAnalysis&>(*analysis_);
  }

 public:
  DeclareTypeAST(TypeAnnotation type,
                 std::optional<std::string> alias = std::nullopt)
      : aliasName(std::move(alias)), typeAnnotation(std::move(type)) {}

  ASTNodeType getType() const override { return ASTNodeType::DECLARE_TYPE; }
  std::string toString() const override {
    if (aliasName) {
      return "declare " + *aliasName + " = " + typeAnnotation.toString();
    }
    return "declare " + typeAnnotation.toString();
  }

  bool hasAlias() const { return aliasName.has_value(); }
  const std::string& getAliasName() const { return *aliasName; }
  const TypeAnnotation& getTypeAnnotation() const { return typeAnnotation; }

  // Resolved declared type (set by semantic analysis)
  void setResolvedDeclaredType(sun::TypePtr type) const {
    declAnalysis().resolvedDeclaredType = std::move(type);
  }
  sun::TypePtr getResolvedDeclaredType() const {
    return analysis_ ? static_cast<DeclareTypeAnalysis&>(*analysis_)
                           .resolvedDeclaredType
                     : nullptr;
  }
  bool hasResolvedDeclaredType() const {
    return analysis_ &&
           static_cast<DeclareTypeAnalysis&>(*analysis_).resolvedDeclaredType !=
               nullptr;
  }

  std::string dotLabel() const override {
    std::string label = "DeclareType";
    if (aliasName) label += "\n" + *aliasName;
    return label;
  }
  std::unique_ptr<ExprAST> clone() const override;
};
