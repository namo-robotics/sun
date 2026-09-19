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

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::semantic_analysis::DeclarationId;
using sun::semantic_analysis::TypePtr;

/**
 * Top-level nodes (not derived from ExprAST)
 */
class PrototypeAST {
  std::string Name;  // Source name as written by user (for error messages)
  std::vector<TypeParameter> typeParameters;  // Generic type params: <T, U: X>
  // Lifetime params: <'a, T> declares 'a. Kept apart from typeParameters so
  // generic arity, inference and specialization never see lifetimes - they
  // are erased before codegen and never mint a specialization.
  std::vector<LifetimeParameter> lifetimeParameters;
  std::vector<std::pair<std::string, TypeAnnotation>> args;
  std::optional<TypeAnnotation> returnType;
  std::vector<std::string> refCaptureNames;  // Declared [ref x, ...] list
  // The subset of refCaptureNames written `[const ref x]`: a read-only
  // borrow, so several lambdas may capture the same variable
  std::vector<std::string> constRefCaptureNames;
  // Names written in the capture list without `ref`: the closure owns them.
  // A compound value moves in and is dropped with the closure's scope.
  std::vector<std::string> ownedCaptureNames;
  std::optional<VariadicParam> variadicParam_;  // Trailing `args...` pack
  bool cVariadic_ = false;     // C-style trailing `...` (extern declarations)
  bool unsafeMethod_ = false;  // Calls require an unsafe block.
  bool constMethod_ = false;   // `const method`: `this` is immutable
  std::optional<std::string> linkName_;  // `as "c_symbol"` override
  sun::support::Position location_;      // Source span of the signature
  std::string doc_;  // Comment written above the declaration

  // Analysis data populated by semantic analyzer
  mutable std::unique_ptr<PrototypeAnalysis> analysis_;

  /**
   * Lazy accessor for analysis data
   */
  PrototypeAnalysis& analysis() const {
    if (!analysis_) {
      analysis_ = std::make_unique<PrototypeAnalysis>();
    }
    return *analysis_;
  }

 public:
  /** Creates this syntax node from its operands and declaration information. */
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

  /** Record the bindings selected for this closure in the current session. */
  void setCaptures(const std::vector<Capture>& caps) {
    analysis().captures = caps;
  }
  /** Return computed captures, or an empty list before analysis. */
  const std::vector<Capture>& getCaptures() const {
    static const std::vector<Capture> empty;
    return analysis_ ? analysis_->captures : empty;
  }
  /** Report whether analysis found a closure environment. */
  bool hasClosure() const { return !getCaptures().empty(); }

  /**
   * Names declared in the lambda's [ref x, ...] capture list (parser-derived
   * source of truth; Capture::kind is derived from it during analysis)
   */
  void setRefCaptureNames(std::vector<std::string> names) {
    refCaptureNames = std::move(names);
  }
  /** Returns the ref capture names stored by this object. */
  const std::vector<std::string>& getRefCaptureNames() const {
    return refCaptureNames;
  }
  /** Updates the const ref capture names stored by this object. */
  void setConstRefCaptureNames(std::vector<std::string> names) {
    constRefCaptureNames = std::move(names);
  }
  /** Returns the const ref capture names stored by this object. */
  const std::vector<std::string>& getConstRefCaptureNames() const {
    return constRefCaptureNames;
  }
  /**
   * True if `name` was written `[const ref name]` rather than `[ref name]`
   */
  bool isConstRefCapture(const std::string& name) const {
    return std::find(constRefCaptureNames.begin(), constRefCaptureNames.end(),
                     name) != constRefCaptureNames.end();
  }
  /** Updates the owned capture names stored by this object. */
  void setOwnedCaptureNames(std::vector<std::string> names) {
    ownedCaptureNames = std::move(names);
  }
  /** Returns the owned capture names stored by this object. */
  const std::vector<std::string>& getOwnedCaptureNames() const {
    return ownedCaptureNames;
  }
  /**
   * True if `name` was written in the capture list without `ref`
   */
  bool isOwnedCapture(const std::string& name) const {
    return std::find(ownedCaptureNames.begin(), ownedCaptureNames.end(),
                     name) != ownedCaptureNames.end();
  }
  /**
   * True if the closure holds state bound to the frame that built it: a
   * borrow of a local, or a value it owns and drops there. Either way the
   * closure must not outlive that frame.
   */
  bool hasRefCaptures() const {
    for (const auto& cap : getCaptures()) {
      if (cap.kind != CaptureKind::ByValue) return true;
    }
    return false;
  }
  /** Returns the syntax-node kind used to dispatch tree visitors. */
  ASTNodeType getType() const { return ASTNodeType::PROTOTYPE; }
  /** Returns the declared name used to identify this object. */
  const std::string& getName() const { return Name; }
  /** Updates the name stored by this object. */
  void setName(std::string name) { Name = std::move(name); }

  /** Sets the source position used for diagnostics. */
  void setLocation(sun::support::Position loc) { location_ = std::move(loc); }
  /** Returns the source position used for diagnostics. */
  const sun::support::Position& getLocation() const { return location_; }

  /**
   * Comment written above the function (see doc_comments.h)
   */
  const std::string& getDoc() const { return doc_; }
  /** Stores the source documentation comment for this declaration. */
  void setDoc(std::string doc) { doc_ = std::move(doc); }

  // Analysis data access
  /** Reports whether this object has analysis. */
  bool hasAnalysis() const { return analysis_ != nullptr; }
  /** Return this function's identity in the current analysis session. */
  DeclarationId getDeclarationId() const {
    return analysis_ ? analysis_->declaration.id : DeclarationId{};
  }
  /** Assign the identity shared by the function and its prototype. */
  void setDeclarationId(DeclarationId id) const {
    analysis().declaration.id = id;
  }
  /** Access identities for the function and its parameters. */
  sun::semantic_analysis::DeclarationIdentity& declarationIdentity() const {
    return analysis().declaration;
  }
  /** Clear the resolved signature while retaining declaration identities. */
  void clearComputedAnalysis() const {
    if (!analysis_) return;
    auto identity = std::move(analysis_->declaration);
    analysis_ = std::make_unique<PrototypeAnalysis>();
    analysis_->declaration = std::move(identity);
  }
  /** Drop all annotations when discarding the owning session. */
  void resetAnalysisSession() const {
    if (!analysis_) return;
    auto imported = std::move(analysis_->declaration.imported);
    analysis_.reset();
    if (imported) analysis().declaration.imported = std::move(imported);
  }
  /** Returns the analysis stored by this object. */
  const PrototypeAnalysis* getAnalysis() const { return analysis_.get(); }

  /**
   * Qualified name (after semantic analysis qualifies it)
   */
  const sun::semantic_analysis::QualifiedName& getQualifiedName() const {
    return analysis().qualifiedName;
  }
  /** Records the declaration name together with its enclosing scopes. */
  void setQualifiedName(sun::semantic_analysis::QualifiedName qname) {
    analysis().qualifiedName = std::move(qname);
  }
  /** Reports whether a name including the enclosing scopes has been assigned. */
  bool hasQualifiedName() const {
    return analysis_ && !analysis_->qualifiedName.empty();
  }

  /**
   * Lifetime parameter support
   */
  void setLifetimeParameters(std::vector<LifetimeParameter> params) {
    lifetimeParameters = std::move(params);
  }
  /** Provides the declared lifetime parameters used to check borrowed values. */
  const std::vector<LifetimeParameter>& getLifetimeParameters() const {
    return lifetimeParameters;
  }

  /**
   * Generic method support
   */
  const std::vector<TypeParameter>& getTypeParameters() const {
    return typeParameters;
  }
  /** Returns the names used to bind generic arguments during specialization. */
  std::vector<std::string> getTypeParameterNames() const {
    return typeParameterNames(typeParameters);
  }
  /** Reports whether this declaration still has unbound type parameters. */
  bool isGeneric() const { return !typeParameters.empty(); }
  /**
   * Emitted as one function per specialization: either because it has type
   * parameters, or because its `args...` pack is keyed on the call's argument
   * types. A pack-only template has no type arguments but many arities.
   *
   * A specialization keeps its pack — codegen needs the name to number the
   * elements — so what marks it as no longer a template is that the pack's
   * types are resolved, the same way clearTypeParameters() does for `<T>`.
   */
  bool isTemplate() const {
    return isGeneric() || (hasVariadicParam() && !hasResolvedVariadicTypes());
  }
  /** Removes generic parameters after a declaration is specialized. */
  void clearTypeParameters() { typeParameters.clear(); }

  /** Provides the ordered arguments associated with this expression. */
  const std::vector<std::pair<std::string, TypeAnnotation>>& getArgs() const {
    return args;
  }

  /** Returns the mutable args stored by this object. */
  std::vector<std::pair<std::string, TypeAnnotation>>& getMutableArgs() {
    return args;
  }

  /** Returns the arg names stored by this object. */
  std::vector<std::string> getArgNames() const {
    std::vector<std::string> names;
    for (const auto& [name, type] : args) {
      names.push_back(name);
    }
    return names;
  }

  /** Returns the semantic type of the function result. */
  const std::optional<TypeAnnotation>& getReturnType() const {
    return returnType;
  }
  /** Reports whether this object has return type. */
  bool hasReturnType() const { return returnType.has_value(); }

  /**
   * Set the return type (used by semantic analyzer for type inference)
   */
  void setReturnType(TypeAnnotation type) { returnType = std::move(type); }

  /**
   * The trailing `args...` pack, when the signature declares one. Callers
   * that only want a piece of it have the three shorthands below.
   */
  bool hasVariadicParam() const { return variadicParam_.has_value(); }
  /** Returns the variadic param stored by this object. */
  const VariadicParam& getVariadicParam() const { return *variadicParam_; }
  /**
   * The pack's name, or empty when the signature declares no pack.
   */
  const std::string& getVariadicParamName() const {
    static const std::string none;
    return variadicParam_ ? variadicParam_->name : none;
  }
  /** Reports whether this object has variadic type annotation. */
  bool hasVariadicTypeAnnotation() const {
    return variadicParam_ && variadicParam_->hasTypeAnnotation();
  }
  /** Returns the variadic type annotation stored by this object. */
  const TypeAnnotation& getVariadicTypeAnnotation() const {
    return *variadicParam_->typeAnnotation;
  }

  /**
   * C-style trailing varargs: `fn(fmt: raw_ptr<u8>, ...)`. Unrelated to the
   * named `args...` pack above — this one binds no name and only affects the
   * LLVM function type's isVarArg flag. Extern declarations only.
   */
  bool isCVariadic() const { return cVariadic_; }
  /** Updates the c variadic stored by this object. */
  void setCVariadic(bool v) { cVariadic_ = v; }

  /** Whether callers must uphold this method's safety contract. */
  bool isUnsafeMethod() const { return unsafeMethod_; }
  /** Mark a method as requiring an unsafe block at each call. */
  void setUnsafeMethod(bool v) { unsafeMethod_ = v; }

  /**
   * A class/interface method declared `const method`: its body may not
   * change `this`, and it may be called on a constant receiver.
   */
  bool isConstMethod() const { return constMethod_; }
  /** Updates the const method stored by this object. */
  void setConstMethod(bool v) { constMethod_ = v; }

  /**
   * Explicit C symbol from `extern function sunName(...) T as "c_name";`.
   * Lets a Sun-side name differ from the symbol actually linked against.
   */
  bool hasLinkName() const { return linkName_.has_value(); }
  /** Updates the link name stored by this object. */
  void setLinkName(std::string name) { linkName_ = std::move(name); }
  /**
   * The symbol to emit: the `as` name when given, otherwise the Sun name.
   */
  const std::string& getLinkName() const {
    return linkName_.has_value() ? *linkName_ : Name;
  }

  /**
   * Resolved types for specialized generic functions
   * Set during instantiation, used by codegen to skip type annotation
   * conversion
   */
  void setResolvedParamTypes(std::vector<TypePtr> types) {
    analysis().resolvedParamTypes = std::move(types);
    analysis().resolvedParamTypesSet = true;
  }
  /** Returns the resolved param types stored by this object. */
  const std::vector<TypePtr>& getResolvedParamTypes() const {
    return analysis().resolvedParamTypes;
  }
  /** Reports whether this object has resolved param types. */
  bool hasResolvedParamTypes() const {
    return analysis_ && analysis_->resolvedParamTypesSet;
  }

  /** Updates the resolved return type stored by this object. */
  void setResolvedReturnType(TypePtr type) {
    analysis().resolvedReturnType = std::move(type);
  }
  /** Returns the resolved return type stored by this object. */
  TypePtr getResolvedReturnType() const {
    return analysis_ ? analysis_->resolvedReturnType : nullptr;
  }
  /** Reports whether this object has resolved return type. */
  bool hasResolvedReturnType() const {
    return analysis_ && analysis_->resolvedReturnType != nullptr;
  }

  /**
   * The pack's element types for this specialization, in order. An empty
   * list is still a resolved pack — the call simply passed nothing.
   */
  void setResolvedVariadicTypes(std::vector<TypePtr> types) {
    analysis().resolvedVariadicTypes = std::move(types);
    analysis().resolvedVariadicTypesSet = true;
  }
  /** Returns the resolved variadic types stored by this object. */
  const std::vector<TypePtr>& getResolvedVariadicTypes() const {
    return analysis().resolvedVariadicTypes;
  }
  /** Reports whether this object has resolved variadic types. */
  bool hasResolvedVariadicTypes() const {
    return analysis_ && analysis_->resolvedVariadicTypesSet;
  }

  /**
   * The full parameter list this specialization is emitted with: the fixed
   * parameters followed by the pack's elements. Codegen appends them in this
   * order, so every argument check lines up against the same list.
   */
  std::vector<TypePtr> getAllParamTypes() const {
    std::vector<TypePtr> all = getResolvedParamTypes();
    const auto& pack = getResolvedVariadicTypes();
    all.insert(all.end(), pack.begin(), pack.end());
    return all;
  }

  /**
   * Their names, in the same order. A pack's elements are `args.0`, `args.1`,
   * … — the names the body's expanded references resolve against.
   */
  std::vector<std::string> getAllParamNames() const {
    std::vector<std::string> names = getArgNames();
    if (!hasVariadicParam()) return names;
    const VariadicParam& pack = getVariadicParam();
    for (size_t i = 0; i < getResolvedVariadicTypes().size(); ++i) {
      names.push_back(pack.elementName(i));
    }
    return names;
  }

  /**
   * The declared type of the parameter named `name`, or null if there is no
   * such parameter. Codegen walks a function's LLVM arguments, which are
   * named from getAllParamNames(), so asking by name rather than by index
   * keeps the two lists from drifting — and a closure or fat-pointer
   * argument, which is not a parameter at all, simply answers null.
   */
  TypePtr paramTypeNamed(const std::string& name) const {
    if (!hasResolvedParamTypes()) return nullptr;
    const std::vector<TypePtr>& fixed = getResolvedParamTypes();
    for (size_t i = 0; i < args.size() && i < fixed.size(); ++i) {
      if (args[i].first == name) return fixed[i];
    }
    if (!hasVariadicParam()) return nullptr;
    const VariadicParam& pack = getVariadicParam();
    const std::vector<TypePtr>& packTypes = getResolvedVariadicTypes();
    for (size_t i = 0; i < packTypes.size(); ++i) {
      if (pack.elementName(i) == name) return packTypes[i];
    }
    return nullptr;
  }

  /**
   * Type parameter bindings for specialized generic functions
   */
  void setTypeBindings(std::vector<std::pair<std::string, TypePtr>> bindings) {
    analysis().typeBindings = std::move(bindings);
  }
  /** Returns the type bindings stored by this object. */
  const std::vector<std::pair<std::string, TypePtr>>& getTypeBindings() const {
    return analysis().typeBindings;
  }
  /** Reports whether this object has type bindings. */
  bool hasTypeBindings() const {
    return analysis_ && !analysis_->typeBindings.empty();
  }

  /**
   * Check if this function can throw (declared with "throws IError")
   */
  bool canThrow() const {
    return returnType.has_value() && returnType->canError;
  }

  /**
   * Clone the prototype via protobuf serialization (deep copy)
   */
  std::unique_ptr<PrototypeAST> clone() const;
};

}  // namespace sun::ast
