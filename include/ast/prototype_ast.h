// prototype_ast.h — PrototypeAST class (function signature)

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ast/analysis.h"
#include "ast/ast_common.h"
#include "ast/type_annotation.h"
#include "position.h"
#include "qualified_name.h"
#include "types.h"

// Top-level nodes (not derived from ExprAST)
class PrototypeAST {
  std::string Name;  // Source name as written by user (for error messages)
  std::vector<std::string> typeParameters;  // Generic type parameters: <T, U>
  std::vector<std::pair<std::string, TypeAnnotation>> args;
  std::optional<TypeAnnotation> returnType;
  std::vector<Capture> captures;
  std::vector<std::string> refCaptureNames;  // Declared [ref x, ...] list
  std::optional<std::string>
      variadicParamName_;  // Name of variadic param if present
  std::optional<TypeAnnotation> variadicConstraint_;  // e.g., _init_args<T>
  bool cVariadic_ = false;  // C-style trailing `...` (extern declarations)
  std::optional<std::string> linkName_;  // `as "c_symbol"` override
  Position location_;                    // Source span of the signature

  // Analysis data populated by semantic analyzer
  mutable std::unique_ptr<PrototypeAnalysis> analysis_;

  // Lazy accessor for analysis data
  PrototypeAnalysis& analysis() const {
    if (!analysis_) {
      analysis_ = std::make_unique<PrototypeAnalysis>();
    }
    return *analysis_;
  }

 public:
  PrototypeAST(std::string Name,
               std::vector<std::pair<std::string, TypeAnnotation>> args,
               std::optional<TypeAnnotation> retType = std::nullopt,
               std::vector<std::string> typeParams = {},
               std::optional<std::string> variadicParam = std::nullopt,
               std::optional<TypeAnnotation> variadicConstraint = std::nullopt)
      : Name(std::move(Name)),
        typeParameters(std::move(typeParams)),
        args(std::move(args)),
        returnType(std::move(retType)),
        variadicParamName_(std::move(variadicParam)),
        variadicConstraint_(std::move(variadicConstraint)) {}

  void setCaptures(const std::vector<Capture>& caps) { captures = caps; }
  const std::vector<Capture>& getCaptures() const { return captures; }
  bool hasClosure() const { return !captures.empty(); }

  // Names declared in the lambda's [ref x, ...] capture list (parser-derived
  // source of truth; Capture::byRef is derived from it during analysis)
  void setRefCaptureNames(std::vector<std::string> names) {
    refCaptureNames = std::move(names);
  }
  const std::vector<std::string>& getRefCaptureNames() const {
    return refCaptureNames;
  }
  bool hasRefCaptures() const {
    for (const auto& cap : captures) {
      if (cap.byRef) return true;
    }
    return false;
  }
  ASTNodeType getType() const { return ASTNodeType::PROTOTYPE; }
  const std::string& getName() const { return Name; }
  void setName(std::string name) { Name = std::move(name); }

  void setLocation(Position loc) { location_ = std::move(loc); }
  const Position& getLocation() const { return location_; }

  // Analysis data access
  bool hasAnalysis() const { return analysis_ != nullptr; }
  void clearAnalysis() const { analysis_.reset(); }
  const PrototypeAnalysis* getAnalysis() const { return analysis_.get(); }

  // Qualified name (after semantic analysis qualifies it)
  const sun::QualifiedName& getQualifiedName() const {
    return analysis().qualifiedName;
  }
  // Returns mangled form for codegen symbol lookup
  std::string getMangledName() const {
    auto& qn = analysis().qualifiedName;
    return qn.empty() ? Name : qn.mangled();
  }
  void setQualifiedName(sun::QualifiedName qname) {
    analysis().qualifiedName = std::move(qname);
  }
  bool hasQualifiedName() const {
    return analysis_ && !analysis_->qualifiedName.empty();
  }

  // Generic method support
  const std::vector<std::string>& getTypeParameters() const {
    return typeParameters;
  }
  bool isGeneric() const { return !typeParameters.empty(); }
  void clearTypeParameters() { typeParameters.clear(); }

  const std::vector<std::pair<std::string, TypeAnnotation>>& getArgs() const {
    return args;
  }

  std::vector<std::pair<std::string, TypeAnnotation>>& getMutableArgs() {
    return args;
  }

  std::vector<std::string> getArgNames() const {
    std::vector<std::string> names;
    for (const auto& [name, type] : args) {
      names.push_back(name);
    }
    return names;
  }

  const std::optional<TypeAnnotation>& getReturnType() const {
    return returnType;
  }
  bool hasReturnType() const { return returnType.has_value(); }

  // Set the return type (used by semantic analyzer for type inference)
  void setReturnType(TypeAnnotation type) { returnType = std::move(type); }

  // Variadic parameter support
  bool hasVariadicParam() const { return variadicParamName_.has_value(); }
  const std::optional<std::string>& getVariadicParamName() const {
    return variadicParamName_;
  }
  bool hasVariadicConstraint() const { return variadicConstraint_.has_value(); }
  const std::optional<TypeAnnotation>& getVariadicConstraint() const {
    return variadicConstraint_;
  }

  // C-style trailing varargs: `fn(fmt: raw_ptr<u8>, ...)`. Unrelated to the
  // named `args...` pack above — this one binds no name and only affects the
  // LLVM function type's isVarArg flag. Extern declarations only.
  bool isCVariadic() const { return cVariadic_; }
  void setCVariadic(bool v) { cVariadic_ = v; }

  // Explicit C symbol from `extern function sunName(...) T as "c_name";`.
  // Lets a Sun-side name differ from the symbol actually linked against.
  bool hasLinkName() const { return linkName_.has_value(); }
  void setLinkName(std::string name) { linkName_ = std::move(name); }
  // The symbol to emit: the `as` name when given, otherwise the Sun name.
  const std::string& getLinkName() const {
    return linkName_.has_value() ? *linkName_ : Name;
  }

  // Resolved types for specialized generic functions
  // Set during instantiation, used by codegen to skip type annotation
  // conversion
  void setResolvedParamTypes(std::vector<sun::TypePtr> types) {
    analysis().resolvedParamTypes = std::move(types);
    analysis().resolvedParamTypesSet = true;
  }
  const std::vector<sun::TypePtr>& getResolvedParamTypes() const {
    return analysis().resolvedParamTypes;
  }
  bool hasResolvedParamTypes() const {
    return analysis_ && analysis_->resolvedParamTypesSet;
  }

  void setResolvedReturnType(sun::TypePtr type) {
    analysis().resolvedReturnType = std::move(type);
  }
  sun::TypePtr getResolvedReturnType() const {
    return analysis_ ? analysis_->resolvedReturnType : nullptr;
  }
  bool hasResolvedReturnType() const {
    return analysis_ && analysis_->resolvedReturnType != nullptr;
  }

  // Resolved variadic param types for _init_args<T> expansion
  void setResolvedVariadicTypes(std::vector<sun::TypePtr> types) {
    analysis().resolvedVariadicTypes = std::move(types);
  }
  const std::vector<sun::TypePtr>& getResolvedVariadicTypes() const {
    return analysis().resolvedVariadicTypes;
  }
  bool hasResolvedVariadicTypes() const {
    return analysis_ && !analysis_->resolvedVariadicTypes.empty();
  }

  // Type parameter bindings for specialized generic functions
  void setTypeBindings(
      std::vector<std::pair<std::string, sun::TypePtr>> bindings) {
    analysis().typeBindings = std::move(bindings);
  }
  const std::vector<std::pair<std::string, sun::TypePtr>>& getTypeBindings()
      const {
    return analysis().typeBindings;
  }
  bool hasTypeBindings() const {
    return analysis_ && !analysis_->typeBindings.empty();
  }

  // Check if this function can throw (declared with ", IError")
  bool canThrow() const {
    return returnType.has_value() && returnType->canError;
  }

  // Clone the prototype via protobuf serialization (deep copy)
  std::unique_ptr<PrototypeAST> clone() const;
};
