// class_definition_ast.h — ClassDefinitionAST class

#pragma once

#include <map>
#include <memory>
#include <set>
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
using sun::semantic_analysis::DeclarationId;
using sun::semantic_analysis::Visibility;

/** Declares a class field with its type and optional default expression. */
struct ClassFieldDecl {
  std::string name;
  TypeAnnotation type;
  sun::support::Position location;  // Source location of field qualifiedName
  Visibility visibility = Visibility::Private;
  std::string doc;  // Comment written above the field
  std::unique_ptr<ExprAST> initializer;
  mutable sun::semantic_analysis::DeclarationIdentity declaration{};
};

/**
 * Method qualifiedName in a class (uses FunctionAST internally)
 */
struct ClassMethodDecl {
  std::unique_ptr<FunctionAST> function;
  bool isConstructor;    // true if method name is "init"
  bool isConst = false;  // `const method`: does not mutate `this`
  /** Returns the access level recorded on this member declaration. */
  Visibility visibility() const { return function->getVisibility(); }
};

/**
 * Implemented interface with optional type arguments
 * e.g., IIterator&lt;T&gt; or IComparable<i32>
 */
struct ImplementedInterfaceAST {
  /** Represent an implemented interface using ordinary type syntax. */
  TypeAnnotation toAnnotation() const {
    TypeAnnotation result(name);
    result.declarationKey = declarationKey;
    for (const auto& argument : typeArguments)
      result.typeArguments.push_back(
          std::make_unique<TypeAnnotation>(argument));
    return result;
  }
  std::string name;
  std::vector<TypeAnnotation> typeArguments;
  std::optional<sun::semantic_analysis::DeclarationId> declarationKey;
};

/**
 * Class definition: class Name<T, U> implements Interface1&lt;T&gt;, Interface2
 * { fields and methods }
 */
class ClassDefinitionAST : public ExprAST {
  std::string name;  // Source name as written by user (for error messages)
  std::vector<TypeParameter>
      typeParameters;  // Generic type parameters: <T, U: Trait>
  // Lifetime parameters: class Bus<'a, 'b>. Kept apart from typeParameters
  // so generic machinery never sees lifetimes (they are erased, and never
  // mint a specialization).
  std::vector<LifetimeParameter> lifetimeParameters;
  std::vector<ImplementedInterfaceAST>
      implementedInterfaces;  // Interfaces with type args
  std::vector<ClassFieldDecl> fields;
  std::vector<ClassMethodDecl> methods;
  std::set<std::string> compiledSpecializations_;
  bool isPartial_ = false;  // True for "partial class X {}" (methods only)
  bool isPacked_ = false;   // True for "packed class X {}" (no field padding)
  std::string doc_;         // Comment written above the class

 protected:
  /**
   * Override to allocate ClassAnalysis instead of base ExprAnalysis
   */
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<ClassAnalysis>();
    }
  }

 private:
  /**
   * Access as ClassAnalysis
   */
  ClassAnalysis& classAnalysis() const {
    ensureAnalysis();
    return static_cast<ClassAnalysis&>(*analysis_);
  }

 public:
  /** Creates this syntax node and takes ownership of any supplied child
   * expressions. */
  ClassDefinitionAST(std::string name, std::vector<TypeParameter> typeParams,
                     std::vector<ImplementedInterfaceAST> interfaces,
                     std::vector<ClassFieldDecl> fields,
                     std::vector<ClassMethodDecl> methods,
                     bool precompiled = false)
      : name(std::move(name)),
        typeParameters(std::move(typeParams)),
        implementedInterfaces(std::move(interfaces)),
        fields(std::move(fields)),
        methods(std::move(methods)) {
    precompiled_ = precompiled;
  }

  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const override { return ASTNodeType::CLASS_DEFINITION; }

  /** Visits replaceable child expressions so tree passes can rewrite them in
   * place. */
  void forEachChildSlot(const ChildSlotFn& fn) override {
    for (auto& field : fields) {
      if (field.initializer) fn(field.initializer);
    }
    for (auto& method : methods) {
      if (method.function) method.function->forEachChildSlot(fn);
    }
  }
  /** Returns a readable representation for diagnostics and debugging. */
  std::string toString() const override {
    std::string result;
    if (isPublic()) result += "public ";
    if (isPartial_) result += "partial ";
    result += (isPacked_ ? "packed_class " : "class ") + name;
    if (!typeParameters.empty()) {
      result += "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i].toString();
      }
      result += ">";
    }
    if (!implementedInterfaces.empty()) {
      result += " implements ";
      for (size_t i = 0; i < implementedInterfaces.size(); ++i) {
        if (i > 0) result += ", ";
        result += implementedInterfaces[i].name;
        if (!implementedInterfaces[i].typeArguments.empty()) {
          result += "<...>";
        }
      }
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
    return classAnalysis().qualifiedName;
  }
  /** Records the declaration name together with its enclosing scopes. */
  void setQualifiedName(sun::semantic_analysis::QualifiedName qname) {
    classAnalysis().qualifiedName = std::move(qname);
  }
  /** Reports whether a name including the enclosing scopes has been assigned.
   */
  bool hasQualifiedName() const {
    return analysis_ &&
           !static_cast<ClassAnalysis&>(*analysis_).qualifiedName.empty();
  }
  /** Stores the declared lifetime parameters used to check borrowed values. */
  void setLifetimeParameters(std::vector<LifetimeParameter> params) {
    lifetimeParameters = std::move(params);
  }
  /** Provides the declared lifetime parameters used to check borrowed values.
   */
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
  /** Reports whether this object has generic methods. */
  bool hasGenericMethods() const {
    for (const auto& method : methods) {
      if (method.function->getProto().isGeneric()) return true;
    }
    return false;
  }
  /** Provides the interfaces implemented by this class. */
  const std::vector<ImplementedInterfaceAST>& getImplementedInterfaces() const {
    return implementedInterfaces;
  }
  /** Provides the field declarations belonging to this type. */
  const std::vector<ClassFieldDecl>& getFields() const { return fields; }
  /** Provides the method declarations belonging to this type. */
  const std::vector<ClassMethodDecl>& getMethods() const { return methods; }

  /**
   * Find the constructor (init method)
   */
  const ClassMethodDecl* getConstructor() const {
    for (const auto& method : methods) {
      if (method.isConstructor) return &method;
    }
    return nullptr;
  }

  /** Record a specialization supplied by the imported bundle. */
  void addCompiledSpecialization(std::string key) {
    compiledSpecializations_.insert(
        sun::semantic_analysis::DeclarationId::fromString(key).encoding());
  }
  /** Return the specializations already compiled into the bundle. */
  const std::set<std::string>& getCompiledSpecializations() const {
    return compiledSpecializations_;
  }
  /** Report whether the bundle supplies this concrete class. */
  bool hasCompiledSpecialization(const std::string& key) const {
    return compiledSpecializations_.count(key) != 0;
  }

  /**
   * Specialization storage for generic classes
   * Called by semantic analyzer when a generic class is instantiated
   */
  void addSpecialization(
      DeclarationId id,
      std::shared_ptr<ClassDefinitionAST> specializedAST) const {
    classAnalysis().specializations[id] = std::move(specializedAST);
  }
  const std::map<DeclarationId, std::shared_ptr<ClassDefinitionAST>>&
  /** Provides the concrete instances created from this generic declaration. */
  getSpecializations() const {
    return classAnalysis().specializations;
  }
  /** Reports whether an instance already exists for the supplied type
   * arguments. */
  bool hasSpecialization(DeclarationId id) const {
    return analysis_ &&
           static_cast<ClassAnalysis&>(*analysis_).specializations.find(id) !=
               static_cast<ClassAnalysis&>(*analysis_).specializations.end();
  }
  /** Looks up the instance previously created for the supplied type arguments.
   */
  std::shared_ptr<ClassDefinitionAST> getSpecialization(
      DeclarationId id) const {
    if (!analysis_) return nullptr;
    auto& specs = static_cast<ClassAnalysis&>(*analysis_).specializations;
    auto it = specs.find(id);
    return it != specs.end() ? it->second : nullptr;
  }

  /**
   * Partial class support: "partial class X {}" adds methods to existing class
   */
  bool isPartial() const { return isPartial_; }
  /** Marks whether this class declaration extends a partial class. */
  void setIsPartial(bool v) { isPartial_ = v; }

  /**
   * Packed class support: "packed class X {}" lays fields out with no padding
   */
  bool isPacked() const { return isPacked_; }
  /** Controls whether the class uses a packed memory layout. */
  void setIsPacked(bool v) { isPacked_ = v; }

  /**
   * Allow adding methods from extensions (mutable for merging)
   */
  std::vector<ClassMethodDecl>& getMutableMethods() {
    compiledSpecializations_.clear();
    return methods;
  }
  /** Provides mutable access to field declarations for later compiler passes.
   */
  std::vector<ClassFieldDecl>& getMutableFields() { return fields; }

  /**
   * Comment written above the class (see doc_comments.h)
   */
  const std::string& getDoc() const { return doc_; }
  /** Stores the source documentation comment for this declaration. */
  void setDoc(std::string doc) { doc_ = std::move(doc); }

  /** Returns the node label used in syntax-tree graph visualizations. */
  std::string dotLabel() const override {
    return std::string(isPacked_ ? "Packed Class\n" : "Class\n") + name;
  }

  /**
   * Keyword that introduces this qualifiedName in source
   */
  const char* classKeyword() const {
    return isPacked_ ? "packed_class" : "class";
  }
};

}  // namespace sun::ast
