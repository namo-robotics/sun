// variable_assignment_ast.h — VariableAssignmentAST class

#pragma once

#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "semantic_analysis/qualified_name.h"

class VariableAssignmentAST : public ExprAST {
  std::string name;
  std::unique_ptr<ExprAST> value;

 protected:
  // Override to allocate VariableAnalysis instead of base ExprAnalysis
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<VariableAnalysis>();
    }
  }

 private:
  VariableAnalysis& varAnalysis() const {
    ensureAnalysis();
    return static_cast<VariableAnalysis&>(*analysis_);
  }

 public:
  explicit VariableAssignmentAST(std::string name,
                                 std::unique_ptr<ExprAST> value)
      : name(std::move(name)), value(std::move(value)) {}
  ASTNodeType getType() const override {
    return ASTNodeType::VARIABLE_ASSIGNMENT;
  }

  void forEachChildSlot(const ChildSlotFn& fn) override { fn(value); }
  std::string toString() const override {
    return name + " = " + value->toString();
  }
  const std::string& getName() const { return name; }
  const ExprAST* getValue() const { return value.get(); }
  std::string dotLabel() const override { return "VarAssign\n" + name; }

  // Qualified name (after semantic analysis qualifies it). A module-level
  // global is emitted under its mangled name, so codegen looks the symbol up
  // by this rather than by the name written at the assignment.
  const sun::QualifiedName& getQualifiedName() const {
    return varAnalysis().qualifiedName;
  }
  std::string getMangledName() const {
    auto& qn = varAnalysis().qualifiedName;
    return qn.empty() ? name : qn.mangled();
  }
  void setQualifiedName(sun::QualifiedName qname) {
    varAnalysis().qualifiedName = std::move(qname);
  }
};
