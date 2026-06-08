// interface_definition_ast.h — InterfaceDefinitionAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "ast/function_ast.h"
#include "ast/type_annotation.h"
#include "lexer.h"
#include "qualified_name.h"

// Field declaration in an interface: var name: type;
struct InterfaceFieldDecl {
  std::string name;
  TypeAnnotation type;
  Position location;  // Source location of field declaration
};

// Method declaration in an interface (uses FunctionAST internally)
// Methods can have default implementations
struct InterfaceMethodDecl {
  std::unique_ptr<FunctionAST> function;
  bool hasDefaultImpl;  // true if method has a body (default implementation)
};

// Interface definition: interface Name<T, U> { fields and methods }
class InterfaceDefinitionAST : public ExprAST {
  std::string name;  // Source name as written by user (for error messages)
  std::vector<std::string>
      typeParameters;  // Generic type parameters: T, U, etc.
  std::vector<InterfaceFieldDecl> fields;
  std::vector<InterfaceMethodDecl> methods;

 protected:
  // Override to allocate InterfaceAnalysis instead of base ExprAnalysis
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<InterfaceAnalysis>();
    }
  }

 private:
  // Access as InterfaceAnalysis
  InterfaceAnalysis& ifaceAnalysis() const {
    ensureAnalysis();
    return static_cast<InterfaceAnalysis&>(*analysis_);
  }

 public:
  InterfaceDefinitionAST(std::string name, std::vector<std::string> typeParams,
                         std::vector<InterfaceFieldDecl> fields,
                         std::vector<InterfaceMethodDecl> methods,
                         bool precompiled = false)
      : name(std::move(name)),
        typeParameters(std::move(typeParams)),
        fields(std::move(fields)),
        methods(std::move(methods)) {
    precompiled_ = precompiled;
  }

  ASTNodeType getType() const override {
    return ASTNodeType::INTERFACE_DEFINITION;
  }
  std::string toString() const override {
    std::string result = "interface " + name;
    if (!typeParameters.empty()) {
      result += "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i];
      }
      result += ">";
    }
    result += " { ... }";
    return result;
  }

  const std::string& getName() const { return name; }
  // Qualified name (after semantic analysis qualifies it)
  const sun::QualifiedName& getQualifiedName() const {
    return ifaceAnalysis().qualifiedName;
  }
  // Returns mangled form for codegen symbol lookup
  std::string getMangledName() const {
    auto& qn = ifaceAnalysis().qualifiedName;
    return qn.empty() ? name : qn.mangled();
  }
  void setQualifiedName(sun::QualifiedName qname) {
    ifaceAnalysis().qualifiedName = std::move(qname);
  }
  bool hasQualifiedName() const {
    return analysis_ &&
           !static_cast<InterfaceAnalysis&>(*analysis_).qualifiedName.empty();
  }
  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  bool isGeneric() const { return !typeParameters.empty(); }
  const std::vector<InterfaceFieldDecl>& getFields() const { return fields; }
  const std::vector<InterfaceMethodDecl>& getMethods() const { return methods; }

  // Get methods with default implementations
  std::vector<const InterfaceMethodDecl*> getDefaultMethods() const {
    std::vector<const InterfaceMethodDecl*> defaults;
    for (const auto& method : methods) {
      if (method.hasDefaultImpl) defaults.push_back(&method);
    }
    return defaults;
  }

  // Get methods without default implementations (must be implemented by class)
  std::vector<const InterfaceMethodDecl*> getRequiredMethods() const {
    std::vector<const InterfaceMethodDecl*> required;
    for (const auto& method : methods) {
      if (!method.hasDefaultImpl) required.push_back(&method);
    }
    return required;
  }
  std::string dotLabel() const override { return "Interface\n" + name; }
  std::unique_ptr<ExprAST> clone() const override;
};
