// class_definition_ast.h — ClassDefinitionAST class

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ast/analysis.h"
#include "ast/expr_ast.h"
#include "ast/function_ast.h"
#include "ast/type_annotation.h"
#include "parsing/lexer.h"
#include "semantic_analysis/qualified_name.h"

// Field declaration in a class: var name: type;
struct ClassFieldDecl {
  std::string name;
  TypeAnnotation type;
  Position location;  // Source location of field declaration
  sun::Visibility visibility = sun::Visibility::Private;
  std::string doc;  // Comment written above the field
};

// Method declaration in a class (uses FunctionAST internally)
struct ClassMethodDecl {
  std::unique_ptr<FunctionAST> function;
  bool isConstructor;    // true if method name is "init"
  bool isConst = false;  // `const method`: does not mutate `this`
  sun::Visibility visibility() const { return function->getVisibility(); }
};

// Implemented interface with optional type arguments
// e.g., IIterator<T> or IComparable<i32>
struct ImplementedInterfaceAST {
  std::string name;                           // Interface name: "IIterator"
  std::vector<TypeAnnotation> typeArguments;  // Type args: [T] or [i32]
};

// Class definition: class Name<T, U> implements Interface1<T>, Interface2 {
// fields and methods }
class ClassDefinitionAST : public ExprAST {
  std::string name;  // Source name as written by user (for error messages)
  std::vector<TypeParameter>
      typeParameters;  // Generic type parameters: <T, U: Trait>
  std::vector<ImplementedInterfaceAST>
      implementedInterfaces;  // Interfaces with type args
  std::vector<ClassFieldDecl> fields;
  std::vector<ClassMethodDecl> methods;
  bool isPartial_ = false;  // True for "partial class X {}" (methods only)
  bool isPacked_ = false;   // True for "packed class X {}" (no field padding)
  std::string doc_;         // Comment written above the class

 protected:
  // Override to allocate ClassAnalysis instead of base ExprAnalysis
  void ensureAnalysis() const override {
    if (!analysis_) {
      analysis_ = std::make_unique<ClassAnalysis>();
    }
  }

 private:
  // Access as ClassAnalysis
  ClassAnalysis& classAnalysis() const {
    ensureAnalysis();
    return static_cast<ClassAnalysis&>(*analysis_);
  }

 public:
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

  ASTNodeType getType() const override { return ASTNodeType::CLASS_DEFINITION; }

  void forEachChildSlot(const ChildSlotFn& fn) override {
    for (auto& method : methods) {
      if (method.function) method.function->forEachChildSlot(fn);
    }
  }
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

  const std::string& getName() const { return name; }
  // Qualified name (after semantic analysis qualifies it)
  const sun::QualifiedName& getQualifiedName() const {
    return classAnalysis().qualifiedName;
  }
  // Returns mangled form for codegen symbol lookup
  std::string getMangledName() const {
    auto& qn = classAnalysis().qualifiedName;
    return qn.empty() ? name : qn.mangled();
  }
  void setQualifiedName(sun::QualifiedName qname) {
    classAnalysis().qualifiedName = std::move(qname);
  }
  bool hasQualifiedName() const {
    return analysis_ &&
           !static_cast<ClassAnalysis&>(*analysis_).qualifiedName.empty();
  }
  const std::vector<TypeParameter>& getTypeParameters() const {
    return typeParameters;
  }
  std::vector<std::string> getTypeParameterNames() const {
    return typeParameterNames(typeParameters);
  }
  bool isGeneric() const { return !typeParameters.empty(); }
  bool hasGenericMethods() const {
    for (const auto& method : methods) {
      if (method.function->getProto().isGeneric()) return true;
    }
    return false;
  }
  const std::vector<ImplementedInterfaceAST>& getImplementedInterfaces() const {
    return implementedInterfaces;
  }
  const std::vector<ClassFieldDecl>& getFields() const { return fields; }
  const std::vector<ClassMethodDecl>& getMethods() const { return methods; }

  // Find the constructor (init method)
  const ClassMethodDecl* getConstructor() const {
    for (const auto& method : methods) {
      if (method.isConstructor) return &method;
    }
    return nullptr;
  }

  // Specialization storage for generic classes
  // Called by semantic analyzer when a generic class is instantiated
  void addSpecialization(
      const std::string& mangledName,
      std::shared_ptr<ClassDefinitionAST> specializedAST) const {
    classAnalysis().specializations[mangledName] = std::move(specializedAST);
  }
  const std::map<std::string, std::shared_ptr<ClassDefinitionAST>>&
  getSpecializations() const {
    return classAnalysis().specializations;
  }
  bool hasSpecialization(const std::string& mangledName) const {
    return analysis_ &&
           static_cast<ClassAnalysis&>(*analysis_)
                   .specializations.find(mangledName) !=
               static_cast<ClassAnalysis&>(*analysis_).specializations.end();
  }
  std::shared_ptr<ClassDefinitionAST> getSpecialization(
      const std::string& mangledName) const {
    if (!analysis_) return nullptr;
    auto& specs = static_cast<ClassAnalysis&>(*analysis_).specializations;
    auto it = specs.find(mangledName);
    return it != specs.end() ? it->second : nullptr;
  }

  // Partial class support: "partial class X {}" adds methods to existing class
  bool isPartial() const { return isPartial_; }
  void setIsPartial(bool v) { isPartial_ = v; }

  // Packed class support: "packed class X {}" lays fields out with no padding
  bool isPacked() const { return isPacked_; }
  void setIsPacked(bool v) { isPacked_ = v; }

  // Allow adding methods from extensions (mutable for merging)
  std::vector<ClassMethodDecl>& getMutableMethods() { return methods; }
  std::vector<ClassFieldDecl>& getMutableFields() { return fields; }

  // Comment written above the class (see doc_comments.h)
  const std::string& getDoc() const { return doc_; }
  void setDoc(std::string doc) { doc_ = std::move(doc); }

  std::string dotLabel() const override {
    return std::string(isPacked_ ? "Packed Class\n" : "Class\n") + name;
  }

  // Keyword that introduces this declaration in source
  const char* classKeyword() const {
    return isPacked_ ? "packed_class" : "class";
  }
};
