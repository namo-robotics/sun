// reference_creation_ast.h — ReferenceCreationAST class

#pragma once

#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "semantic_analysis/qualified_name.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Reference creation: ref x = y (mutable) or const ref x = y (immutable)
 * Creates a reference variable x that points to the address of y
 */
class ReferenceCreationAST : public ExprAST {
  std::string name;
  std::unique_ptr<ExprAST> target;  // The expression being referenced
  bool mutable_;                    // true = mutable ref, false = immutable ref

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
  /** Creates this syntax node from its operands and declaration information. */
  explicit ReferenceCreationAST(std::string name,
                                std::unique_ptr<ExprAST> target,
                                bool isMutable = true,
                                sun::support::Position loc = {})
      : ExprAST(loc),
        name(std::move(name)),
        target(std::move(target)),
        mutable_(isMutable) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override {
    return ASTNodeType::REFERENCE_CREATION;
  }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override { fn(target); }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return std::string(mutable_ ? "ref " : "const ref ") + name + " = " +
           target->toString();
  }
  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name; }
  /** Returns the target stored by this object. */
  const ExprAST* getTarget() const { return target.get(); }
  /** Reports whether the loan grants exclusive write access to its target. */
  bool isMutable() const { return mutable_; }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "RefCreate\n" + name; }

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
  /** Reports whether a name including the enclosing scopes has been assigned. */
  bool hasQualifiedName() const {
    return analysis_ &&
           !static_cast<VariableAnalysis&>(*analysis_).qualifiedName.empty();
  }
};

}  // namespace sun::ast
