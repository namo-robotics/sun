// generic_specializer.h — Monomorphization: turning a template plus type
// arguments into a real class, function, method, interface or enum.
//
// Sun has no runtime generics. Every `Vec<i32>`, every `spawn<T>(...)` call and
// every `Option<ref T>` payload is a distinct specialization, built the first
// time it is asked for and cached under its mangled name. This class owns that
// cache, the recursion guard that stops a mutually recursive template from
// instantiating forever, and the queue of specializations whose bodies the
// declaration pre-pass deferred.
//
// Specializing means analyzing a body, so this holds a reference back to the
// analyzer. The direction that matters is the other one: nothing else needs to
// know how a specialization is built or when it is cached.

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "semantic_analysis/semantic_context.h"

class SemanticAnalyzer;

/**
 * Builds and caches the specializations a program asks for. Bodies are
 * analyzed in the scope the template was declared in, so names inside a
 * template resolve as written at the definition site rather than at the call
 * site that triggered the instantiation.
 */
class GenericSpecializer {
 public:
  GenericSpecializer(SemanticContext &ctx, SemanticAnalyzer &sema)
      : ctx_(ctx), sema_(sema) {}

  // ---- Classes -----------------------------------------------------------

  /**
   * Monomorphize a generic class for the given type arguments, reusing the
   * specialization if it already exists.
   */
  std::shared_ptr<sun::ClassType> instantiateGenericClass(
      const std::string &baseName, const std::vector<sun::TypePtr> &typeArgs);

  /** The same when the template has already been looked up. */
  std::shared_ptr<sun::ClassType> instantiateGenericClass(
      const GenericClassInfo &genericClassInfo,
      const std::vector<sun::TypePtr> &typeArgs);

  /** The generic definition a specialized class was instantiated from. */
  const GenericClassInfo *lookupGenericClassOf(
      const sun::ClassType &specialized) const;

  /**
   * The scope a class's template was declared in: for a specialization, the
   * generic's; for a plain class with generic methods, its own registration.
   * Null when the class has no template.
   */
  SemanticScope *classDefinitionScope(const sun::ClassType &classType) const;

  /**
   * Analyze the method bodies the pre-pass deferred, now that every
   * declaration in the program is registered. A body may ask for further
   * specializations; those are analyzed straight away.
   */
  void analyzeDeferredSpecializations();

  /**
   * True while the declaration pre-pass is running, so a class specialization
   * registers its type and method signatures now but defers its method bodies
   * until every declaration in the program is known.
   */
  void setInDeclarationPrepass(bool inPrepass) { inPrepass_ = inPrepass; }

  // ---- Functions ---------------------------------------------------------

  /**
   * Monomorphize a generic function for the given type arguments, reusing the
   * cached specialization when there is one. Empty when it cannot be built.
   * variadicArgTypes carries the types filling an `args...` pack at the call
   * site; like the method path, they drive the specialization's arity and its
   * mangled name, and `std::nullopt` defers a pack-bearing template until a
   * call site supplies them.
   */
  std::optional<SpecializedFunctionInfo> instantiateGenericFunction(
      const GenericFunctionInfo &genericInfo,
      const std::vector<sun::TypePtr> &typeArgs,
      const std::optional<std::vector<sun::TypePtr>> &variadicArgTypes =
          std::nullopt);

  /**
   * Instantiate for a call site: same as instantiateGenericFunction, but a
   * failure is the call's error rather than an empty optional to unpack.
   */
  SpecializedFunctionInfo requireGenericSpecialization(
      const GenericFunctionInfo &genericInfo,
      const std::vector<sun::TypePtr> &typeArgs, const std::string &displayName,
      std::optional<Position> loc,
      const std::optional<std::vector<sun::TypePtr>> &variadicArgTypes =
          std::nullopt);

  /**
   * Type-argument inference itself is sun::generics
   * (generic_type_arguments.h). The signature a generic function has under
   * the given type arguments, without instantiating it: what a call in a
   * template body resolves to until the enclosing generic is specialized.
   */
  sun::TypePtr genericFunctionSignature(
      const GenericFunctionInfo &genericInfo,
      const std::vector<sun::TypePtr> &typeArgs);

  /**
   * True when a call cannot be specialized yet because the template's type
   * arguments are still type parameters. Usually that shows in typeArgs, but
   * a pack-only template has none of its own and may still borrow a type
   * parameter from an enclosing generic through `args...: _params_of<T>`.
   */
  bool templateStillAbstract(const GenericFunctionInfo &genericInfo,
                             const std::vector<sun::TypePtr> &typeArgs);

  // ---- Methods -----------------------------------------------------------

  /**
   * Instantiates a generic method on a class with specific type arguments.
   * Stores the specialization on the generic method's FunctionAST.
   * Returns the specialized FunctionAST for codegen lookup.
   * variadicArgTypes carries the resolved types of the actual variadic
   * arguments at the call site (for a method ending in a pack). When
   * the method is variadic, these drive the specialization's arity, its init
   * overload selection, and its mangled name. `std::nullopt` means "no call
   * info available" (e.g. from type inference): a variadic method is then not
   * specialized here and the call-site trigger, which supplies the types
   * (possibly an empty vector for a zero-arg call), does the real work.
   */
  std::shared_ptr<FunctionAST> instantiateGenericMethod(
      std::shared_ptr<sun::ClassType> classType, const std::string &methodName,
      const std::vector<sun::TypePtr> &methodTypeArgs,
      const std::optional<std::vector<sun::TypePtr>> &variadicArgTypes =
          std::nullopt);

  /**
   * Find a generic method's FunctionAST on a class by name (nullptr if none).
   */
  FunctionAST *findGenericMethodAST(const sun::ClassType *classType,
                                    const std::string &methodName);

  // ---- Interfaces and enums ----------------------------------------------

  /**
   * Monomorphize a generic interface for the given type arguments, reusing
   * the specialization if it already exists.
   */
  std::shared_ptr<sun::InterfaceType> instantiateGenericInterface(
      const std::string &baseName, const std::vector<sun::TypePtr> &typeArgs);

  /** Instantiate Option<i32> from a generic enum template (monomorphization).
   */
  std::shared_ptr<sun::EnumType> instantiateGenericEnum(
      const std::string &baseName, const std::vector<sun::TypePtr> &typeArgs);

  // ---- Constraints and variadic packs ------------------------------------

  /**
   * Check each type argument against its parameter's constraint, if it has
   * one, and report the first violation. Call it at every instantiation
   * point, right after the arity check.
   *
   * Only concrete arguments are checked: inside an uninstantiated template
   * body a type argument is still a type parameter, and the constraint is
   * checked later, when the enclosing generic is specialized for real.
   *
   * `what` and `name` name the thing being instantiated, e.g.
   * ("generic function", "spawn").
   */
  void checkTypeParameterConstraints(
      const std::vector<TypeParameter> &typeParams,
      const std::vector<sun::TypePtr> &typeArgs, const std::string &what,
      const std::string &name, std::optional<Position> loc = std::nullopt);

  /**
   * Bring a specialization's `args...` pack into scope for body analysis: the
   * pack itself, so `args...` can be expanded, and one variable per element
   * under the name codegen gives that parameter.
   */
  void declareVariadicPack(const PrototypeAST &proto);

  /**
   * A call's argument types, divided into the callee's fixed parameters and
   * the remainder that fills its `args...` pack. Returns the pack's share, or
   * nullopt when the callee declares no pack. Errors when the call does not
   * even cover the fixed parameters.
   */
  std::optional<std::vector<sun::TypePtr>> splitPackArgTypes(
      const PrototypeAST &proto, const std::vector<sun::TypePtr> &argTypes,
      const std::string &displayName, std::optional<Position> loc);

  /**
   * Record a specialization's pack element types on its cloned prototype and
   * check them against the pack's declared type annotation. `_params_of<C>`
   * for a class C means C must have a matching `init` overload; for a lambda,
   * that lambda's parameters must match. Any other annotation is recorded and
   * left unchecked. Call inside the type parameter scope, so the `T` in
   * `_params_of<T>` resolves.
   */
  void applyVariadicParamTypes(
      PrototypeAST &clonedProto, const PrototypeAST &proto,
      const std::vector<sun::TypePtr> &variadicArgTypes,
      std::optional<Position> loc);

 private:
  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;

  // Classes currently being instantiated, so a mutually recursive template
  // stops instead of specializing forever.
  std::set<std::string> classesBeingInstantiated_;

  // Specialized (monomorphized) functions by mangled name.
  std::map<std::string, SpecializedFunctionInfo> specializedFunctionCache_;

  // A class specialization whose type and method signatures are registered
  // but whose method bodies are not analyzed yet.
  struct DeferredSpecialization {
    std::shared_ptr<sun::ClassType> specializedClass;
    const GenericClassInfo *genericInfo;
    std::vector<sun::TypePtr> typeArgs;
    std::shared_ptr<ClassDefinitionAST> specializedAST;  // bodies unanalyzed
  };
  std::vector<DeferredSpecialization> deferredSpecializations_;

  // Set while the declaration pre-pass runs; see setInDeclarationPrepass.
  bool inPrepass_ = false;
};
