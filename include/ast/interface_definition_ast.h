// interface_definition_ast.h — InterfaceDefinitionAST class

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "ast/function_ast.h"
#include "ast/type_annotation.h"
#include "parsing/lexer.h"
#include "semantic_analysis/qualified_name.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::semantic_analysis::Visibility;

/**
 * Field declaration in an interface: var name: type;
 */
struct InterfaceFieldDecl {
  std::string name;
  TypeAnnotation type;
  sun::support::Position location;  // Source location of field declaration
  Visibility visibility = Visibility::Private;
  std::string doc;  // Comment written above the field
  mutable sun::semantic_analysis::DeclarationIdentity declaration{};
};

/**
 * Method declaration in an interface (uses FunctionAST internally)
 * Methods can have default implementations
 */
struct InterfaceMethodDecl {
  std::unique_ptr<FunctionAST> function;
  bool hasDefaultImpl;   // true if method has a body (default implementation)
  bool isConst = false;  // `const function`: does not mutate `this`
  /** Returns the access level recorded on this member declaration. */
  Visibility visibility() const { return function->getVisibility(); }
};

/**
 * Interface definition: interface Name<T, U> { fields and methods }
 */
class InterfaceDefinitionAST : public ExprAST {
  std::string name;  // Source name as written by user (for error messages)
  std::vector<TypeParameter>
      typeParameters;  // Generic type parameters: <T, U: Trait>
  // Lifetime parameters: interface ISink<'a>. Kept apart from
  // typeParameters; see ClassDefinitionAST.
  std::vector<LifetimeParameter> lifetimeParameters;
  std::vector<InterfaceFieldDecl> fields;
  std::vector<InterfaceMethodDecl> methods;
  std::string doc_;  // Comment written above the interface

 protected:
  /**
   * Override to allocate InterfaceAnalysis instead of base ExprAnalysis
   */
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<InterfaceAnalysis>();
    }
  }

 private:
  /**
   * Access as InterfaceAnalysis
   */
  InterfaceAnalysis& ifaceAnalysis() const {
    ensureAnalysis();
    return static_cast<InterfaceAnalysis&>(*analysis_);
  }

 public:
  /** Creates this syntax node and takes ownership of any supplied child expressions. */
  InterfaceDefinitionAST(std::string name,
                         std::vector<TypeParameter> typeParams,
                         std::vector<InterfaceFieldDecl> fields,
                         std::vector<InterfaceMethodDecl> methods,
                         bool precompiled = false)
      : name(std::move(name)),
        typeParameters(std::move(typeParams)),
        fields(std::move(fields)),
        methods(std::move(methods)) {
    precompiled_ = precompiled;
  }

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override {
    return ASTNodeType::INTERFACE_DEFINITION;
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result =
        std::string(isPublic() ? "public " : "") + "interface " + name;
    if (!typeParameters.empty()) {
      result += "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i].toString();
      }
      result += ">";
    }
    result += " { ... }";
    return result;
  }

  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return name; }
  /**
   * Qualified name (after semantic analysis qualifies it)
   */
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return ifaceAnalysis().qualifiedName;
  }
  /** Records the declaration name together with its enclosing scopes. */
  void setQualifiedName(sun::semantic_analysis::QualifiedName qname) {
    ifaceAnalysis().qualifiedName = std::move(qname);
  }
  /** Reports whether a name including the enclosing scopes has been assigned. */
  bool hasQualifiedName() const {
    return analysis_ &&
           !static_cast<InterfaceAnalysis&>(*analysis_).qualifiedName.empty();
  }
  /** Stores the declared lifetime parameters used to check borrowed values. */
  void setLifetimeParameters(std::vector<LifetimeParameter> params) {
    lifetimeParameters = std::move(params);
  }
  /** Provides the declared lifetime parameters used to check borrowed values. */
  const std::vector<LifetimeParameter>& getLifetimeParameters() const {
    return lifetimeParameters;
  }
  /** Provides the generic parameters declared by this type or function. */
  const std::vector<TypeParameter>& getTypeParameters() const {
    return typeParameters;
  }
  /** Returns the names used to bind generic arguments during specialization. */
  std::vector<std::string> getTypeParameterNames() const {
    return typeParameterNames(typeParameters);
  }
  /** Reports whether this declaration still has unbound type parameters. */
  bool isGeneric() const { return !typeParameters.empty(); }
  /** Provides the field declarations belonging to this type. */
  const std::vector<InterfaceFieldDecl>& getFields() const { return fields; }
  /** Provides the method declarations belonging to this type. */
  const std::vector<InterfaceMethodDecl>& getMethods() const { return methods; }
  /** Provides mutable access to field declarations for later compiler passes. */
  std::vector<InterfaceFieldDecl>& getMutableFields() { return fields; }
  /** Provides mutable access to method declarations for later compiler passes. */
  std::vector<InterfaceMethodDecl>& getMutableMethods() { return methods; }

  /**
   * Comment written above the interface (see doc_comments.h)
   */
  const std::string& getDoc() const { return doc_; }
  /** Stores the source documentation comment for this declaration. */
  void setDoc(std::string doc) { doc_ = std::move(doc); }

  /** Visits replaceable child expressions so tree passes can rewrite them in place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    for (auto& method : methods) {
      if (method.function) method.function->forEachChildSlot(fn);
    }
  }

  /**
   * Get methods with default implementations
   */
  std::vector<const InterfaceMethodDecl*> getDefaultMethods() const {
    std::vector<const InterfaceMethodDecl*> defaults;
    for (const auto& method : methods) {
      if (method.hasDefaultImpl) defaults.push_back(&method);
    }
    return defaults;
  }

  /**
   * Get methods without default implementations (must be implemented by class)
   */
  std::vector<const InterfaceMethodDecl*> getRequiredMethods() const {
    std::vector<const InterfaceMethodDecl*> required;
    for (const auto& method : methods) {
      if (!method.hasDefaultImpl) required.push_back(&method);
    }
    return required;
  }
  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override { return "Interface\n" + name; }
};

}  // namespace sun::ast
