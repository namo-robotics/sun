// call_analyzer.h — Semantic analysis of calls.
//
// Every spelling of a call comes through here: a function by name, a
// constructor, a method on an object, a module-qualified function, a callable
// value, and the `f<T>(...)` forms that name their type arguments. The job is
// the same for all of them — work out what is being called, check the
// arguments against that signature, and stamp what codegen needs onto the
// node: the callee's resolved name and type, and one conversion per argument.
//
// Like TypeInferer, this holds the analyzer rather than being part of it: it
// analyzes arguments and callee expressions through the analyzer, and the
// analyzer dispatches each call node here.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "semantic_analysis/generic_specializer.h"
#include "semantic_analysis/semantic_context.h"
#include "semantic_analysis/semantic_scope.h"
#include "semantic_analysis/type_inferer.h"
#include "support/position.h"

class SemanticAnalyzer;

/**
 * Resolves and checks call expressions, recording the resolved callee and
 * argument conversions on the AST for codegen.
 */
class CallAnalyzer {
 public:
  CallAnalyzer(SemanticContext &ctx, SemanticAnalyzer &sema,
               GenericSpecializer &generics, TypeInferer &types)
      : ctx_(ctx), sema_(sema), generics_(generics), types_(types) {}

  /**
   * Analyze a call: resolve the callee against the argument types, check the
   * arguments against the chosen signature, and record one ArgConversion per
   * argument for codegen.
   */
  void analyzeCall(CallExprAST &callExpr, sun::TypePtr expectedType = nullptr);

  /**
   * Analyze `name<T, ...>(args)`: an intrinsic, a generic class construction
   * or a generic function call, told apart by what the name resolves to.
   */
  void analyzeGenericCall(GenericCallAST &genericCall);

  /**
   * Resolve a module-qualified call `mod.foo(args...)` against the actual
   * argument types and stamp the chosen overload's own mangled name onto the
   * member access. Rebuilding the name from the module path instead would
   * drop the overload param suffix and name a symbol codegen never emits.
   * Returns nullptr if the module has no overload matching those arguments.
   */
  const FunctionInfo *resolveModuleQualifiedCall(
      const MemberAccessAST &memberAccess, const sun::TypePtr &objectType,
      const std::vector<sun::TypePtr> &argTypes) const;

 private:
  // ---- analyzeCall, phase by phase ---------------------------------------

  /** What resolving a call's callee established about the call. */
  struct CalleeResolution {
    // The overload a plain `f(...)` call resolved to, if it named a function.
    std::optional<FunctionInfo> function;
    // The class a constructor call `C(...)` names, if it named one.
    std::shared_ptr<sun::ClassType> classType;
    // The callee swallows a variadic pack, whose arguments are not part of
    // its recorded parameter list — so the arity check sits out.
    bool takesPack = false;
    // The method was called on a constant receiver (see checkMethodReceiver),
    // so a `ref T` result becomes `const ref T`.
    bool receiverImmutable = false;
  };

  /** The parameter list a call is checked against. */
  struct CallSignature {
    std::vector<sun::TypePtr> paramTypes;
    // Whether paramTypes came from a callee whose signature is actually
    // known. An empty parameter list is a real signature (`f()`), so
    // emptiness alone cannot stand in for "unknown" — that is what let calls
    // to zero-parameter methods and lambdas past the arity check (issue #87).
    bool known = false;
  };

  /**
   * Analyze a call's arguments and give back their types. Arguments go first
   * so overload resolution has real types to match against, which means an
   * argument that needs a hint — an array literal, an overloaded bound method
   * reference — takes it from a provisional look at the callee. Also expands
   * a variadic pack (`f(args...)`) into the concrete arguments it stands for.
   */
  std::vector<sun::TypePtr> analyzeCallArguments(CallExprAST &callExpr,
                                                 sun::TypePtr expectedType);

  /**
   * Resolve what a call is actually calling: an overload by name, a
   * constructor, a method on an object, or an expression that evaluates to
   * something callable. Analyzes the callee and stamps the resolved name and
   * type onto it; the checking of arguments against the result is
   * analyzeCall's own step.
   */
  CalleeResolution resolveCallee(CallExprAST &callExpr,
                                 const std::vector<sun::TypePtr> &argTypes);

  /** `f(args)`: an overload, a constructor, a generic, or a variable. */
  CalleeResolution resolveNamedCallee(
      CallExprAST &callExpr, VariableReferenceAST &varRef,
      const std::vector<sun::TypePtr> &argTypes);

  /** `obj.m(args)`: a method, a module member, or a builtin type's method. */
  CalleeResolution resolveMemberCallee(
      CallExprAST &callExpr, MemberAccessAST &memberAccess,
      const std::vector<sun::TypePtr> &argTypes);

  /** `obj.m(args)` on a class: a callable field, or a method overload. */
  CalleeResolution resolveMethodCallee(
      MemberAccessAST &memberAccess, const sun::TypePtr &objectType,
      const std::vector<sun::TypePtr> &argTypes);

  /**
   * The signature the arguments are checked against, from the resolved
   * overload, the callee's function or lambda type, or a constructor's
   * chosen `init`.
   */
  CallSignature resolveCallSignature(CallExprAST &callExpr,
                                     CalleeResolution &callee,
                                     const std::vector<sun::TypePtr> &argTypes);

  /**
   * Check each argument against its parameter when no overload resolution
   * vouched for them already: literal coercion, then the implicit conversions
   * a call site allows. `calleeIsIntrinsic` unlocks the byte-pointer erasure
   * only intrinsics may use.
   */
  void checkArgumentTypes(CallExprAST &callExpr,
                          const std::vector<sun::TypePtr> &paramTypes,
                          const std::string &funcName, bool calleeIsIntrinsic);

  /**
   * A call to something that throws must sit in a try block or in a function
   * that itself throws.
   */
  void checkThrowPropagation(const CallExprAST &callExpr,
                             const CalleeResolution &callee,
                             const std::string &funcName);

  // ---- Shared by the call forms ------------------------------------------

  /**
   * The parameter list of the `init` overload a positional construction
   * `C(args)` selects. Throws when the class has an `init` but none takes
   * these arguments, or has none at all yet arguments were given. Returns
   * nullopt for the one silent case: no `init` and no arguments.
   */
  std::optional<std::vector<sun::TypePtr>> resolveConstructorParams(
      const sun::ClassType &classType,
      const std::vector<sun::TypePtr> &argTypes, const Position &loc);

  /**
   * Give array-literal arguments their element type before analysis, from
   * the parameter each will bind to, so they generate with the right type.
   */
  static void hintArrayLiteralArguments(
      const std::vector<std::unique_ptr<ExprAST>> &args,
      const std::vector<sun::TypePtr> &paramTypes);

  /**
   * Expand a variadic pack (`args...`) in a call's argument list into
   * concrete, already-typed VariableReferenceAST nodes ("args.0", "args.1",
   * ...), using the enclosing function scope's recorded variadic param. No-op
   * when there is no enclosing variadic param or no pack argument is present.
   */
  void expandPackArguments(std::vector<std::unique_ptr<ExprAST>> &args);

  /**
   * The member a module-qualified call `mod.name(args...)` names, of the
   * given kind, or an empty match when the object is not a module or the
   * module has no such member. Pass the argument types to pick among a
   * function's overloads.
   */
  SymbolMatch findModuleCallee(
      const MemberAccessAST &memberAccess, const sun::TypePtr &objectType,
      SymbolKind kind,
      const std::vector<sun::TypePtr> *argTypes = nullptr) const;

  /**
   * Resolve a module-qualified call of a generic function, `mod.f(args...)`
   * or `mod.f<A>(args...)`, by way of resolveGenericCallTarget. Pins the
   * callee to what that gave and returns the resolution, whose takesPack is
   * set when the signature cannot yet list the pack's elements. Returns
   * nullopt if the module has no generic function of that name.
   */
  std::optional<CalleeResolution> resolveModuleQualifiedGenericCall(
      const MemberAccessAST &memberAccess, const sun::TypePtr &objectType,
      const std::vector<sun::TypePtr> &argTypes);

  /** What a generic call resolves to once its type arguments are known. */
  struct GenericCallTarget {
    // The complete type arguments: those written at the call, then the rest
    // inferred from its arguments.
    std::vector<sun::TypePtr> typeArgs;
    // The specialization the call is pinned to; empty in a template body,
    // where the type arguments are still type parameters and the
    // specialization is made when the enclosing generic is instantiated.
    std::optional<SpecializedFunctionInfo> specialized;
    // The callee's type: the specialization's, or until then the template's
    // signature under the type arguments.
    sun::TypePtr calleeType;
    // The callee ends in a pack that calleeType cannot list yet (a template
    // body), so the arity check sits out. A specialization's parameter list
    // already includes the pack's elements.
    bool takesPack = false;
  };

  /**
   * Resolve a call of a generic function, qualified or not, against its
   * argument types. A call may name only the leading type parameters —
   * `f<i32>(x)` for `f<T, U>` — and leave the rest to the arguments; a call
   * with no type arguments infers them all. Inference reads the fixed
   * parameters only, so a call filling a pack still says what T is from its
   * leading arguments: `spawn(f, 1, 2)`.
   */
  GenericCallTarget resolveGenericCallTarget(
      const GenericFunctionInfo &genericInfo,
      const std::vector<sun::TypePtr> &argTypes,
      const std::vector<sun::TypePtr> &writtenTypeArgs,
      const std::string &displayName, std::optional<Position> loc);

  /**
   * Throws "No matching overload" when the class has methods called `name`
   * but none of them takes `argTypes.size()` arguments. Silent otherwise, so
   * callers can still fall back on their own type-mismatch diagnostics.
   */
  void reportNoMethodForArgCount(const sun::ClassType &cls,
                                 const std::string &name,
                                 const std::vector<sun::TypePtr> &argTypes,
                                 const Position &loc) const;

  /**
   * Calling into C leaves everything the borrow checker and type system
   * guarantee, so it is gated on `unsafe` — the same rule the equivalent
   * intrinsics (_malloc, _free, ...) already follow. Throws if `info` names a
   * C extern and the call site is not inside an unsafe block.
   */
  void checkExternCallAllowed(const FunctionInfo &info,
                              const std::string &displayName,
                              const Position &loc) const;

  /**
   * The same rule for intrinsics: those that read or write unchecked memory
   * are gated on `unsafe`. `sun::requiresUnsafeBlock` decides which, and this
   * is where it is applied — for generic and non-generic intrinsics alike.
   * Throws if `name` is one of them and the call site is not inside a block.
   */
  void checkRequiresUnsafeBlock(const std::string &name,
                                const Position &loc) const;

  // ---- `name<T>(args)` -----------------------------------------------------

  /**
   * Analyze an intrinsic call: the arguments only, since codegen decides what
   * the intrinsic does.
   */
  void analyzeIntrinsicCall(GenericCallAST &genericCall);

  /**
   * Decide how each argument of a _spawn call reaches the spawned lambda's
   * parameters, and mark what the thread takes over as moved.
   */
  void recordSpawnArgumentConversions(GenericCallAST &genericCall);

  /**
   * Analyze `f<T>(...)`: resolve the template, fill in any type arguments the
   * call left to the arguments, then specialize it.
   */
  void analyzeGenericFunctionCall(GenericCallAST &genericCall);

  /**
   * Analyze `C<T>(...)`: specialize the generic class, then check the
   * arguments against the chosen constructor.
   */
  void analyzeGenericClassConstruction(GenericCallAST &genericCall);

  SemanticContext &ctx_;
  SemanticAnalyzer &sema_;
  GenericSpecializer &generics_;
  TypeInferer &types_;
};
