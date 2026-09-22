// member_assignment_ast.h — MemberAssignmentAST class

#pragma once

#include <memory>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Member assignment: object.field = value
 */
class MemberAssignmentAST : public ExprAST {
  std::unique_ptr<ExprAST> object;  // The object (can be 'this' or any expr)
  std::string memberName;           // The field name
  std::unique_ptr<ExprAST> value;   // The value to assign

 protected:
  /** Creates the analysis annotations required by this node when first needed.
   */
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<MemberAccessAnalysis>();
    }
  }

 private:
  /** Accesses the resolved member and field-write annotations. */
  MemberAccessAnalysis& memberAnalysis() const {
    ensureAnalysis();
    return static_cast<MemberAccessAnalysis&>(*analysis_);
  }

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  MemberAssignmentAST(std::unique_ptr<ExprAST> obj, std::string member,
                      std::unique_ptr<ExprAST> val)
      : object(std::move(obj)),
        memberName(std::move(member)),
        value(std::move(val)) {}

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override {
    return ASTNodeType::MEMBER_ASSIGNMENT;
  }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    fn(object);
    fn(value);
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    return object->toString() + "." + memberName + " = " + value->toString();
  }

  /** Provides the receiver expression used for member access. */
  const ExprAST* getObject() const { return object.get(); }
  /** Returns the member name to resolve on the receiver. */
  const std::string& getMemberName() const { return memberName; }
  /** Returns the value represented by this object. */
  const ExprAST* getValue() const { return value.get(); }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override {
    return "MemberAssign\n." + memberName;
  }

  /**
   * Resolved symbol name for a write to a module global (set by semantic
   * analysis); empty for an ordinary field write.
   */
  void setQualifiedName(sun::semantic_analysis::QualifiedName name) const {
    memberAnalysis().qualifiedName = std::move(name);
  }
  /** Returns the declaration name together with its enclosing scopes. */
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return memberAnalysis().qualifiedName;
  }

  /**
   * What becomes of the value the field held before this write: it is
   * dropped, dropped only if the storage is not all zero, or not dropped at
   * all because the field has never held a value. Set by semantic analysis.
   */
  void setFieldWriteKind(sun::ast::FieldWriteKind kind) const {
    memberAnalysis().fieldWrite = kind;
  }
  /** Returns whether this field write initializes or replaces its value. */
  sun::ast::FieldWriteKind fieldWriteKind() const {
    return memberAnalysis().fieldWrite;
  }
};

}  // namespace sun::ast
