// variable_creation_ast.h — VariableCreationAST class

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"
#include "semantic_analysis/qualified_name.h"

class VariableCreationAST : public ExprAST {
  std::string name;
  std::unique_ptr<ExprAST> value;
  std::optional<TypeAnnotation> typeAnnotation;
  bool isConst_;     // `const x = ...`: the binding and its value never change
  bool isCExtern_ = false;  // C extern storage is provided by a native global
  bool explicitCAbi_ = false;  // Source spelled the optional "C" ABI
  std::optional<std::string> linkName_;  // Optional native symbol override
  std::string doc_;  // Comment written above the declaration

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
      std::optional<TypeAnnotation> type = std::nullopt, bool isConst = false)
      : name(std::move(name)),
        value(std::move(value)),
        typeAnnotation(std::move(type)),
        isConst_(isConst) {}
  ASTNodeType getType() const override {
    return ASTNodeType::VARIABLE_CREATION;
  }
  bool isConst() const { return isConst_; }
  bool isCExtern() const { return isCExtern_; }
  void setCExtern(bool value) { isCExtern_ = value; }
  bool hasExplicitCAbi() const { return explicitCAbi_; }
  void setExplicitCAbi(bool value) { explicitCAbi_ = value; }
  bool hasLinkName() const { return linkName_.has_value(); }
  void setLinkName(std::string name) { linkName_ = std::move(name); }
  const std::string& getLinkName() const {
    return linkName_.has_value() ? *linkName_ : name;
  }
  std::string toString() const override {
    std::string externPrefix =
        explicitCAbi_ ? "extern \"C\" var " : "extern var ";
    std::string result = std::string(isPublic() ? "public " : "") +
                         (isCExtern_ ? externPrefix
                                     : (isConst_ ? "const " : "var ")) +
                         name;
    if (typeAnnotation) result += ": " + typeAnnotation->toString();
    // A global imported from a .moon carries its type but no initializer.
    if (value) result += " = " + value->toString();
    if (linkName_) result += " as \"" + *linkName_ + "\"";
    return result;
  }
  const std::string& getName() const { return name; }
  const ExprAST* getValue() const { return value.get(); }
  bool hasValue() const { return value != nullptr; }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    if (value) fn(value);
  }
  const std::optional<TypeAnnotation>& getTypeAnnotation() const {
    return typeAnnotation;
  }
  bool hasTypeAnnotation() const { return typeAnnotation.has_value(); }

  // Comment written above the declaration (see doc_comments.h)
  const std::string& getDoc() const { return doc_; }
  void setDoc(std::string doc) { doc_ = std::move(doc); }

  std::string dotLabel() const override {
    std::string label = "VarCreate\n" + name;
    if (typeAnnotation) label += ": " + typeAnnotation->toString();
    return label;
  }

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
