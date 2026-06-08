// reference_creation_ast.h — ReferenceCreationAST class

#pragma once

#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "qualified_name.h"

// Reference creation: ref x = y (mutable) or ref const x = y (immutable)
// Creates a reference variable x that points to the address of y
class ReferenceCreationAST : public ExprAST {
  std::string name;
  std::unique_ptr<ExprAST> target;  // The expression being referenced
  bool mutable_;                    // true = mutable ref, false = immutable ref

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
  explicit ReferenceCreationAST(std::string name,
                                std::unique_ptr<ExprAST> target,
                                bool isMutable = true, Position loc = {})
      : ExprAST(loc),
        name(std::move(name)),
        target(std::move(target)),
        mutable_(isMutable) {}
  ASTNodeType getType() const override {
    return ASTNodeType::REFERENCE_CREATION;
  }
  std::string toString() const override {
    return std::string("ref ") + (mutable_ ? "" : "const ") + name + " = " +
           target->toString();
  }
  const std::string& getName() const { return name; }
  const ExprAST* getTarget() const { return target.get(); }
  bool isMutable() const { return mutable_; }
  std::string dotLabel() const override { return "RefCreate\n" + name; }

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
