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
#include "lexer.h"
#include "qualified_name.h"

// Field declaration in a class: var name: type;
struct ClassFieldDecl {
  std::string name;
  TypeAnnotation type;
  Position location;  // Source location of field declaration
};

// Method declaration in a class (uses FunctionAST internally)
struct ClassMethodDecl {
  std::unique_ptr<FunctionAST> function;
  bool isConstructor;  // true if method name is "init"
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
  std::vector<std::string>
      typeParameters;  // Generic type parameters: T, U, etc.
  std::vector<ImplementedInterfaceAST>
      implementedInterfaces;  // Interfaces with type args
  std::vector<ClassFieldDecl> fields;
  std::vector<ClassMethodDecl> methods;
  bool isPartial_ = false;  // True for "partial class X {}" (methods only)

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
  ClassDefinitionAST(std::string name, std::vector<std::string> typeParams,
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
  std::string toString() const override {
    std::string result = "class " + name;
    if (!typeParameters.empty()) {
      result += "<";
      for (size_t i = 0; i < typeParameters.size(); ++i) {
        if (i > 0) result += ", ";
        result += typeParameters[i];
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
  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
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

  // Allow adding methods from extensions (mutable for merging)
  std::vector<ClassMethodDecl>& getMutableMethods() { return methods; }

  std::string dotLabel() const override { return "Class\n" + name; }
  std::unique_ptr<ExprAST> clone() const override;
};
