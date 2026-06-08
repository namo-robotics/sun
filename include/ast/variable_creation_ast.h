// variable_creation_ast.h — VariableCreationAST class

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"
#include "qualified_name.h"

class VariableCreationAST : public ExprAST {
  std::string name;
  std::unique_ptr<ExprAST> value;
  std::optional<TypeAnnotation> typeAnnotation;

 protected:
  // Override to allocate VariableAnalysis instead of base ExprAnalysis
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<VariableAnalysis>();
    }
  }

 private:
  // Access as VariableAnalysis
  VariableAnalysis& varAnalysis() const {
    ensureAnalysis();
    return static_cast<VariableAnalysis&>(*analysis_);
  }

 public:
  explicit VariableCreationAST(
      std::string name, std::unique_ptr<ExprAST> value,
      std::optional<TypeAnnotation> type = std::nullopt)
      : name(std::move(name)),
        value(std::move(value)),
        typeAnnotation(std::move(type)) {}
  ASTNodeType getType() const override {
    return ASTNodeType::VARIABLE_CREATION;
  }
  std::string toString() const override {
    std::string result = "var " + name;
    if (typeAnnotation) result += ": " + typeAnnotation->toString();
    result += " = " + value->toString();
    return result;
  }
  const std::string& getName() const { return name; }
  const ExprAST* getValue() const { return value.get(); }
  const std::optional<TypeAnnotation>& getTypeAnnotation() const {
    return typeAnnotation;
  }
  bool hasTypeAnnotation() const { return typeAnnotation.has_value(); }
  std::string dotLabel() const override {
    std::string label = "VarCreate\n" + name;
    if (typeAnnotation) label += ": " + typeAnnotation->toString();
    return label;
  }
  std::unique_ptr<ExprAST> clone() const override;

  // Qualified name (after semantic analysis qualifies it)
  const sun::QualifiedName& getQualifiedName() const {
    return varAnalysis().qualifiedName;
  }
  // Returns mangled form for codegen symbol lookup
  std::string getMangledName() const {
    auto& qn = varAnalysis().qualifiedName;
    return qn.empty() ? name : qn.mangled();
  }
  void setQualifiedName(sun::QualifiedName qname) {
    varAnalysis().qualifiedName = std::move(qname);
  }
  bool hasQualifiedName() const {
    return analysis_ &&
           !static_cast<VariableAnalysis&>(*analysis_).qualifiedName.empty();
  }
};
