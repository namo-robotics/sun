// prototype_ast.h — PrototypeAST class (function signature)

#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ast/analysis.h"
#include "ast/ast_common.h"
#include "ast/type_annotation.h"
#include "semantic_analysis/qualified_name.h"
#include "semantic_analysis/types.h"
#include "support/position.h"

// Top-level nodes (not derived from ExprAST)
class PrototypeAST {
  std::string Name;  // Source name as written by user (for error messages)
  std::vector<TypeParameter> typeParameters;  // Generic type params: <T, U: X>
  std::vector<std::pair<std::string, TypeAnnotation>> args;
  std::optional<TypeAnnotation> returnType;
  std::vector<Capture> captures;
  std::vector<std::string> refCaptureNames;  // Declared [ref x, ...] list
  // The subset of refCaptureNames written `[const ref x]`: a read-only
  // borrow, so several lambdas may capture the same variable
  std::vector<std::string> constRefCaptureNames;
  // Names written in the capture list without `ref`: the closure owns them.
  // A compound value moves in and is dropped with the closure's scope.
  std::vector<std::string> ownedCaptureNames;
  std::optional<VariadicParam> variadicParam_;  // Trailing `args...` pack
  bool cVariadic_ = false;    // C-style trailing `...` (extern declarations)
  bool constMethod_ = false;  // `const function`: `this` is immutable
  std::optional<std::string> linkName_;  // `as "c_symbol"` override
  Position location_;                    // Source span of the signature
  std::string doc_;  // Comment written above the declaration

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
               std::vector<TypeParameter> typeParams = {},
               std::optional<VariadicParam> variadicParam = std::nullopt)
      : Name(std::move(Name)),
        typeParameters(std::move(typeParams)),
        args(std::move(args)),
        returnType(std::move(retType)),
        variadicParam_(std::move(variadicParam)) {}

  void setCaptures(const std::vector<Capture>& caps) { captures = caps; }
  const std::vector<Capture>& getCaptures() const { return captures; }
  bool hasClosure() const { return !captures.empty(); }

  // Names declared in the lambda's [ref x, ...] capture list (parser-derived
  // source of truth; Capture::kind is derived from it during analysis)
  void setRefCaptureNames(std::vector<std::string> names) {
    refCaptureNames = std::move(names);
  }
  const std::vector<std::string>& getRefCaptureNames() const {
    return refCaptureNames;
  }
  void setConstRefCaptureNames(std::vector<std::string> names) {
    constRefCaptureNames = std::move(names);
  }
  const std::vector<std::string>& getConstRefCaptureNames() const {
    return constRefCaptureNames;
  }
  // True if `name` was written `[const ref name]` rather than `[ref name]`
  bool isConstRefCapture(const std::string& name) const {
    return std::find(constRefCaptureNames.begin(), constRefCaptureNames.end(),
                     name) != constRefCaptureNames.end();
  }
  void setOwnedCaptureNames(std::vector<std::string> names) {
    ownedCaptureNames = std::move(names);
  }
  const std::vector<std::string>& getOwnedCaptureNames() const {
    return ownedCaptureNames;
  }
  // True if `name` was written in the capture list without `ref`
  bool isOwnedCapture(const std::string& name) const {
    return std::find(ownedCaptureNames.begin(), ownedCaptureNames.end(),
                     name) != ownedCaptureNames.end();
  }
  // True if the closure holds state bound to the frame that built it: a
  // borrow of a local, or a value it owns and drops there. Either way the
  // closure must not outlive that frame.
  bool hasRefCaptures() const {
    for (const auto& cap : captures) {
      if (cap.kind != CaptureKind::ByValue) return true;
    }
    return false;
  }
  ASTNodeType getType() const { return ASTNodeType::PROTOTYPE; }
  const std::string& getName() const { return Name; }
  void setName(std::string name) { Name = std::move(name); }

  void setLocation(Position loc) { location_ = std::move(loc); }
  const Position& getLocation() const { return location_; }

  // Comment written above the function (see doc_comments.h)
  const std::string& getDoc() const { return doc_; }
  void setDoc(std::string doc) { doc_ = std::move(doc); }

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
  const std::vector<TypeParameter>& getTypeParameters() const {
    return typeParameters;
  }
  std::vector<std::string> getTypeParameterNames() const {
    return typeParameterNames(typeParameters);
  }
  bool isGeneric() const { return !typeParameters.empty(); }
  // Emitted as one function per specialization: either because it has type
  // parameters, or because its `args...` pack is keyed on the call's argument
  // types. A pack-only template has no type arguments but many arities.
  //
  // A specialization keeps its pack — codegen needs the name to number the
  // elements — so what marks it as no longer a template is that the pack's
  // types are resolved, the same way clearTypeParameters() does for `<T>`.
  bool isTemplate() const {
    return isGeneric() || (hasVariadicParam() && !hasResolvedVariadicTypes());
  }
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

  // The trailing `args...` pack, when the signature declares one. Callers
  // that only want a piece of it have the three shorthands below.
  bool hasVariadicParam() const { return variadicParam_.has_value(); }
  const VariadicParam& getVariadicParam() const { return *variadicParam_; }
  // The pack's name, or empty when the signature declares no pack.
  const std::string& getVariadicParamName() const {
    static const std::string none;
    return variadicParam_ ? variadicParam_->name : none;
  }
  bool hasVariadicTypeAnnotation() const {
    return variadicParam_ && variadicParam_->hasTypeAnnotation();
  }
  const TypeAnnotation& getVariadicTypeAnnotation() const {
    return *variadicParam_->typeAnnotation;
  }

  // C-style trailing varargs: `fn(fmt: raw_ptr<u8>, ...)`. Unrelated to the
  // named `args...` pack above — this one binds no name and only affects the
  // LLVM function type's isVarArg flag. Extern declarations only.
  bool isCVariadic() const { return cVariadic_; }
  void setCVariadic(bool v) { cVariadic_ = v; }

  // A class/interface method declared `const function`: its body may not
  // change `this`, and it may be called on a constant receiver.
  bool isConstMethod() const { return constMethod_; }
  void setConstMethod(bool v) { constMethod_ = v; }

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

  // The pack's element types for this specialization, in order. An empty
  // list is still a resolved pack — the call simply passed nothing.
  void setResolvedVariadicTypes(std::vector<sun::TypePtr> types) {
    analysis().resolvedVariadicTypes = std::move(types);
    analysis().resolvedVariadicTypesSet = true;
  }
  const std::vector<sun::TypePtr>& getResolvedVariadicTypes() const {
    return analysis().resolvedVariadicTypes;
  }
  bool hasResolvedVariadicTypes() const {
    return analysis_ && analysis_->resolvedVariadicTypesSet;
  }

  // The full parameter list this specialization is emitted with: the fixed
  // parameters followed by the pack's elements. Codegen appends them in this
  // order, so every argument check lines up against the same list.
  std::vector<sun::TypePtr> getAllParamTypes() const {
    std::vector<sun::TypePtr> all = getResolvedParamTypes();
    const auto& pack = getResolvedVariadicTypes();
    all.insert(all.end(), pack.begin(), pack.end());
    return all;
  }

  // Their names, in the same order. A pack's elements are `args.0`, `args.1`,
  // … — the names the body's expanded references resolve against.
  std::vector<std::string> getAllParamNames() const {
    std::vector<std::string> names = getArgNames();
    if (!hasVariadicParam()) return names;
    const VariadicParam& pack = getVariadicParam();
    for (size_t i = 0; i < getResolvedVariadicTypes().size(); ++i) {
      names.push_back(pack.elementName(i));
    }
    return names;
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
