// variable_creation_ast.h — VariableCreationAST class

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"
#include "semantic_analysis/qualified_name.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/** A variable declaration with its type, initializer, and binding properties. */
class VariableCreationAST : public ExprAST {
  std::string name;
  std::unique_ptr<ExprAST> value;
  std::optional<TypeAnnotation> typeAnnotation;
  bool isConst_;  // `const x = ...`: the binding and its value never change
  bool isCExtern_ = false;  // C extern storage is provided by a native global
  bool explicitCAbi_ = false;            // Source spelled the optional "C" ABI
  std::optional<std::string> linkName_;  // Optional native symbol override
  std::string doc_;  // Comment written above the declaration

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
  explicit VariableCreationAST(
      std::string name, std::unique_ptr<ExprAST> value,
      std::optional<TypeAnnotation> type = std::nullopt, bool isConst = false)
      : name(std::move(name)),
        value(std::move(value)),
        typeAnnotation(std::move(type)),
        isConst_(isConst) {}
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override {
    return ASTNodeType::VARIABLE_CREATION;
  }
  /** Reports whether this syntax node represents const. */
  bool isConst() const { return isConst_; }
  /** Reports whether this syntax node represents c extern. */
  bool isCExtern() const { return isCExtern_; }
  /** Marks whether the function uses the C calling convention. */
  void setCExtern(bool value) { isCExtern_ = value; }
  /** Reports whether this object has explicit c ABI. */
  bool hasExplicitCAbi() const { return explicitCAbi_; }
  /** Updates the explicit c ABI stored by this object. */
  void setExplicitCAbi(bool value) { explicitCAbi_ = value; }
  /** Reports whether this object has link name. */
  bool hasLinkName() const { return linkName_.has_value(); }
  /** Updates the link name stored by this object. */
  void setLinkName(std::string name) { linkName_ = std::move(name); }
  /** Returns the link name stored by this object. */
  const std::string& getLinkName() const {
    return linkName_.has_value() ? *linkName_ : name;
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string externPrefix =
        explicitCAbi_ ? "extern \"C\" var " : "extern var ";
    std::string result =
        std::string(isPublic() ? "public " : "") +
        (isCExtern_ ? externPrefix : (isConst_ ? "const " : "var ")) + name;
    if (typeAnnotation) result += ": " + typeAnnotation->toString();
    // A global imported from a .moon carries its type but no initializer.
    if (value) result += " = " + value->toString();
    if (linkName_) result += " as \"" + *linkName_ + "\"";
    return result;
  }
  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name; }
  /** Returns the value represented by this object. */
  const ExprAST* getValue() const { return value.get(); }
  /** Reports whether this object has value. */
  bool hasValue() const { return value != nullptr; }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (value) fn(value);
  }
  /** Returns the type annotation stored by this object. */
  const std::optional<TypeAnnotation>& getTypeAnnotation() const {
    return typeAnnotation;
  }
  /** Reports whether this object has type annotation. */
  bool hasTypeAnnotation() const { return typeAnnotation.has_value(); }

  /**
   * Comment written above the declaration (see doc_comments.h)
   */
  const std::string& getDoc() const { return doc_; }
  /** Stores the source documentation comment for this declaration. */
  void setDoc(std::string doc) { doc_ = std::move(doc); }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override {
    std::string label = "VarCreate\n" + name;
    if (typeAnnotation) label += ": " + typeAnnotation->toString();
    return label;
  }

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
