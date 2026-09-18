// variable_reference_ast.h — VariableReferenceAST class

#pragma once

#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "semantic_analysis/qualified_name.h"

namespace sun::ast {

class VariableReferenceAST : public ExprAST {
  std::string Name;

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
  explicit VariableReferenceAST(std::string Name) : Name(std::move(Name)) {}
  ASTNodeType getType() const override {
    return ASTNodeType::VARIABLE_REFERENCE;
  }
  std::string toString() const override { return Name; }
  std::string dotLabel() const override { return "VarRef\n" + Name; }
  const std::string& getName() const { return Name; }

  // Qualified name (after semantic analysis qualifies it)
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return varAnalysis().qualifiedName;
  }
  void setQualifiedName(sun::semantic_analysis::QualifiedName qname) {
    varAnalysis().qualifiedName = std::move(qname);
  }
  bool hasQualifiedName() const {
    return analysis_ &&
           !static_cast<VariableAnalysis&>(*analysis_).qualifiedName.empty();
  }
};

}  // namespace sun::ast
