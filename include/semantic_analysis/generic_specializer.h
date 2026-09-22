// generic_specializer.h — Monomorphization: turning a template plus type
// arguments into a real class, function, method, interface or enum.
//
// Sun has no runtime generics. Every `Vec<i32>`, every `spawn<T>(...)` call and
// every `Option<ref T>` payload is a distinct specialization, built the first
// time it is asked for and interned by its semantic arguments. This class owns
// the prepared callable cache and the queue of bodies awaiting explicit analysis.
//
// Checking queued bodies requires a reference back to the
// analyzer. The direction that matters is the other one: nothing else needs to
// know how a specialization is built or when it is cached.

#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "semantic_analysis/semantic_context.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::ast::PrototypeAST;

class SemanticAnalyzer;

/**
 * Builds and caches the specializations a program asks for. Bodies are
 * analyzed in the scope the template was declared in, so names inside a
 * template resolve as written at the definition site rather than at the call
 * site that triggered the instantiation. Instantiation returns prepared
 * signatures immediately; bodies are always queued until analyzePendingBodies.
 */
class GenericSpecializer {
 public:
  /** Share scope state and checking helpers. */
  GenericSpecializer(SemanticContext &ctx, SemanticAnalyzer &sema)
      : ctx_(ctx), sema_(sema) {}

  // ---- Classes -----------------------------------------------------------

  /**
   * Monomorphize a generic class for the given type arguments, reusing the
   * specialization if it already exists.
   */
  std::shared_ptr<sun::types::ClassType> instantiateGenericClass(
      const std::string &baseName,
      const std::vector<sun::types::TypePtr> &typeArgs);

  /** The same when the template has already been looked up. */
  std::shared_ptr<sun::types::ClassType> instantiateGenericClass(
      const GenericClassInfo &genericClassInfo,
      const std::vector<sun::types::TypePtr> &typeArgs);

  /** Retrieve the template or generic-method definition by declaration ID. */
  const GenericClassInfo *lookupGenericClassOf(
      const sun::types::ClassType &specialized) const;

  /**
   * The scope a class's template was declared in: for a specialization, the
   * generic's; for a plain class with generic methods, its own registration.
   * Null when the class has no template.
   */
  SemanticScope *classDefinitionScope(
      const sun::types::ClassType &classType) const;

  /** Drain prepared specialization bodies in FIFO order, including new jobs.
   * Direct instantiation callers must drain before consuming analyzed bodies.
   * On failure, discard remaining work and preserve the original diagnostic.
   */
  void analyzePendingBodies();

  /** Discard pending work without analyzing it after a failed compilation. */
  void discardPendingBodies() { pendingBodies_.clear(); }

  /** Whether prepared specializations still require body checking. */
  bool hasPendingBodies() const { return !pendingBodies_.empty(); }

  // ---- Functions ---------------------------------------------------------

  /**
   * Monomorphize a generic function for the given type arguments, reusing the
   * cached specialization when there is one. Empty when it cannot be built.
   * variadicArgTypes carries the types filling an `args...` pack at the call
   * site; like the method path, they drive the specialization's arity and its
   * specialization key, and `std::nullopt` defers a pack-bearing template until
   * a call site supplies them.
   */
  std::optional<SpecializedFunctionInfo> instantiateGenericFunction(
      const GenericFunctionInfo &genericInfo,
      const std::vector<sun::types::TypePtr> &typeArgs,
      const std::optional<std::vector<sun::types::TypePtr>> &variadicArgTypes =
          std::nullopt);

  /**
   * Instantiate for a call site: same as instantiateGenericFunction, but a
   * failure is the call's error rather than an empty optional to unpack.
   */
  SpecializedFunctionInfo requireGenericSpecialization(
      const GenericFunctionInfo &genericInfo,
      const std::vector<sun::types::TypePtr> &typeArgs,
      const std::string &displayName, std::optional<sun::support::Position> loc,
      const std::optional<std::vector<sun::types::TypePtr>> &variadicArgTypes =
          std::nullopt);

  /**
   * Type-argument inference itself is sun::semantic_analysis
   * (generic_type_arguments.h). The signature a generic function has under
   * the given type arguments, without instantiating it: what a call in a
   * template body resolves to until the enclosing generic is specialized.
   */
  sun::types::TypePtr genericFunctionSignature(
      const GenericFunctionInfo &genericInfo,
      const std::vector<sun::types::TypePtr> &typeArgs);

  /**
   * True when a call cannot be specialized yet because the template's type
   * arguments are still type parameters. Usually that shows in typeArgs, but
   * a pack-only template has none of its own and may still borrow a type
   * parameter from an enclosing generic through `args...: _params_of<T>`.
   */
  bool templateStillAbstract(const GenericFunctionInfo &genericInfo,
                             const std::vector<sun::types::TypePtr> &typeArgs);

  // ---- Methods -----------------------------------------------------------

  /**
   * Instantiates a generic method on a class with specific type arguments.
   * Stores the specialization on the generic method's FunctionAST.
   * Returns the specialized FunctionAST for codegen lookup.
   * variadicArgTypes carries the resolved types of the actual variadic
   * arguments at the call site (for a method ending in a pack). When
   * the method is variadic, these drive the specialization's arity, its init
   * overload selection, and its specialization key. `std::nullopt` means "no
   * call info available" (e.g. from type inference): a variadic method is then
   * not specialized here and the call-site trigger, which supplies the types
   * (possibly an empty vector for a zero-arg call), does the real work.
   */
  std::shared_ptr<sun::ast::FunctionAST> instantiateGenericMethod(
      std::shared_ptr<sun::types::ClassType> classType,
      const std::string &methodName,
      const std::vector<sun::types::TypePtr> &methodTypeArgs,
      const std::optional<std::vector<sun::types::TypePtr>> &variadicArgTypes =
          std::nullopt);

  /**
   * Find a generic method's FunctionAST on a class by name (nullptr if none).
   */
  sun::ast::FunctionAST *findGenericMethodAST(
      const sun::types::ClassType *classType, const std::string &methodName);

  // ---- Interfaces and enums ----------------------------------------------

  /**
   * Monomorphize a generic interface for the given type arguments, reusing
   * the specialization if it already exists.
   */
  std::shared_ptr<sun::types::InterfaceType> instantiateGenericInterface(
      const std::string &baseName,
      const std::vector<sun::types::TypePtr> &typeArgs);

  /** Instantiate an already selected template without repeating name lookup. */
  std::shared_ptr<sun::types::InterfaceType> instantiateGenericInterface(
      const GenericInterfaceInfo &genericInfo,
      const std::vector<sun::types::TypePtr> &typeArgs);

  /** Instantiate Option<i32> from a generic enum template (monomorphization).
   */
  std::shared_ptr<sun::types::EnumType> instantiateGenericEnum(
      const std::string &baseName,
      const std::vector<sun::types::TypePtr> &typeArgs);

  /** Instantiate an already selected template without repeating name lookup. */
  std::shared_ptr<sun::types::EnumType> instantiateGenericEnum(
      const GenericEnumInfo &genericInfo,
      const std::vector<sun::types::TypePtr> &typeArgs);

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
      const std::vector<sun::ast::TypeParameter> &typeParams,
      const std::vector<sun::types::TypePtr> &typeArgs, const std::string &what,
      const std::string &name,
      std::optional<sun::support::Position> loc = std::nullopt);

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
  std::optional<std::vector<sun::types::TypePtr>> splitPackArgTypes(
      const PrototypeAST &proto,
      const std::vector<sun::types::TypePtr> &argTypes,
      const std::string &displayName,
      std::optional<sun::support::Position> loc);

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
      const std::vector<sun::types::TypePtr> &variadicArgTypes,
      std::optional<sun::support::Position> loc);

 private:
  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;

  // Prepared callable instances by their declaration identity.
  std::map<sun::semantic_analysis::DeclarationId, SpecializedFunctionInfo>
      specializedFunctionCache_;

  /** A prepared specialization and the session-owned scope containing its
   * concrete bindings. The AST retains captures, pack types and source
   * identity.
   */
  struct PendingBody {
    SemanticScope *scope;
    std::shared_ptr<sun::types::ClassType> ownerClass;
    std::variant<std::shared_ptr<sun::ast::ClassDefinitionAST>,
                 std::shared_ptr<sun::ast::FunctionAST>>
        ast;
    bool isMethod = false;
  };
  std::deque<PendingBody> pendingBodies_;

  /** Check ordinary specialized methods, then constructor initialization. */
  void analyzeClassBody(sun::ast::ClassDefinitionAST &ast,
                        std::shared_ptr<sun::types::ClassType> classType);
  /** Bind prepared parameters, captures and packs before checking a callable.
   */
  void analyzeCallableBody(sun::ast::FunctionAST &ast,
                           std::shared_ptr<sun::types::ClassType> classType,
                           bool isMethod);
};

}  // namespace sun::semantic_analysis
