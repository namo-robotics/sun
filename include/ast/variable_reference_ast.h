// variable_reference_ast.h — VariableReferenceAST class

#pragma once

#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "semantic_analysis/qualified_name.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** A reference to a named variable resolved during semantic analysis. */
class VariableReferenceAST : public ExprAST {
  std::string Name;

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
  /**
   * Access as VariableAnalysis
   */
  VariableAnalysis& varAnalysis() const {
    ensureAnalysis();
    return static_cast<VariableAnalysis&>(*analysis_);
  }

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  explicit VariableReferenceAST(std::string Name) : Name(std::move(Name)) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override {
    return ASTNodeType::VARIABLE_REFERENCE;
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override { return Name; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "VarRef\n" + Name; }
  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return Name; }

  /**
   * Qualified name (after semantic analysis qualifies it)
   */
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return varAnalysis().qualifiedName;
  }
  /** Records the declaration name together with its enclosing scopes. */
  void setQualifiedName(sun::semantic_analysis::QualifiedName qname) {
    varAnalysis().qualifiedName = std::move(qname);
  }
  /** Reports whether a name including the enclosing scopes has been assigned.
   */
  bool hasQualifiedName() const {
    return analysis_ &&
           !static_cast<VariableAnalysis&>(*analysis_).qualifiedName.empty();
  }
};

}  // namespace sun::ast
