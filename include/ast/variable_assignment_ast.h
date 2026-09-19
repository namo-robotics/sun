// variable_assignment_ast.h — VariableAssignmentAST class

#pragma once

#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "semantic_analysis/qualified_name.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** An assignment that replaces the value stored in a target expression. */
class VariableAssignmentAST : public ExprAST {
  std::string name;
  std::unique_ptr<ExprAST> value;

 protected:
  /**
   * Override to allocate VariableAnalysis instead of base ExprAnalysis
   */
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<VariableAnalysis>();
    }
  }

 private:
  /** Accesses the variable assignment annotations used by later passes. */
  VariableAnalysis& varAnalysis() const {
    ensureAnalysis();
    return static_cast<VariableAnalysis&>(*analysis_);
  }

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  explicit VariableAssignmentAST(std::string name,
                                 std::unique_ptr<ExprAST> value)
      : name(std::move(name)), value(std::move(value)) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override {
    return ASTNodeType::VARIABLE_ASSIGNMENT;
  }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override { fn(value); }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return name + " = " + value->toString();
  }
  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name; }
  /** Returns the value represented by this object. */
  const ExprAST* getValue() const { return value.get(); }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "VarAssign\n" + name; }

  /**
   * Qualified name (after semantic analysis qualifies it). A module-level
   * global is emitted using its declaration ID, so codegen looks the symbol up
   * by this rather than by the name written at the assignment.
   */
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return varAnalysis().qualifiedName;
  }
  /** Records the declaration name together with its enclosing scopes. */
  void setQualifiedName(sun::semantic_analysis::QualifiedName qname) {
    varAnalysis().qualifiedName = std::move(qname);
  }
};

}  // namespace sun::ast
