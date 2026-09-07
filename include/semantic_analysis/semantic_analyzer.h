// semantic_analyzer.h — Pre-codegen semantic analysis pass
//
// The analyzer delegates to these classes, each with its own header:
//   SemanticContext       scopes, symbol tables, the type registry
//   DeclarationCollector  the pre-pass, and what counts as already declared
//   GenericSpecializer    monomorphization and its cache
//   TypeInferer           what type is this expression / this annotation
//   CallAnalyzer          what a call calls, and how its arguments get there
//   EnumAnalyzer          enum definitions, variants, and match patterns
// They all share the one SemanticContext by reference. The analyzer itself is
// the part that walks the AST: it checks what it finds, and stamps the
// resolved types and conversions codegen reads back off the nodes.
//
// Its implementation is split by topic across src/semantic_analysis/:
//   analysis.cpp             analyzeExpr, analyzeBlock, analyzeFunction
//   analysis_utils.cpp       places, constness, `_is<T>` type guards
//   call_analyzer.cpp        every form of call (its own class, see above)
//   captures.cpp             free variables and closure captures
//   enum_analyzer.cpp        enum definitions, construction, match
//   interfaces.cpp           interfaces and conformance validation
//   packed_classes.cpp       the rules a packed class has to obey
//
// Rules that need no analyzer state live outside the class, so other passes
// can reach the same answers: sun::rules (type_rules.h), sun::names
// (symbol_names.h), sun::access (access_checker.h, item_refs.h),
// sun::conversions (argument_conversion.h), sun::generics
// (generic_type_arguments.h) and sun::traits (type_traits.h).

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/type_annotation.h"
#include "semantic_analysis/access_checker.h"
#include "semantic_analysis/call_analyzer.h"
#include "semantic_analysis/declaration_collector.h"
#include "semantic_analysis/enum_analyzer.h"
#include "semantic_analysis/generic_specializer.h"
#include "semantic_analysis/semantic_context.h"
#include "semantic_analysis/semantic_scope.h"
#include "semantic_analysis/type_inferer.h"

// Forward declarations
struct Position;

// Alias for use in this header and semantic analyzer implementations
using QualifiedName = sun::QualifiedName;

/**
 * Semantic analyzer that runs before codegen to:
 * 1. Build symbol tables with proper scoping
 * 2. Resolve variable types
 * 3. Populate closure captures for each function
 * 4. Infer return types for functions without explicit annotations
 * 5. Handle namespace scoping and using statements
 * 6. Handle class definitions and member access
 * 7. Handle generic class instantiation (monomorphization)
 */
class SemanticAnalyzer {
  // Scopes, symbol tables, the type registry and the current class. Shared by
  // reference with everything else this analysis run is made of.
  SemanticContext ctx_;

  // Builds and caches every specialization the program asks for.
  GenericSpecializer generics_{ctx_, *this};

  // Registers a block's declarations before its bodies are analyzed, and
  // remembers what has already been declared.
  DeclarationCollector declarations_{ctx_, *this};

  // What type is this expression, and what type does this annotation name.
  TypeInferer types_{ctx_, *this, generics_};

  // Checks enum definitions, variant construction, and match patterns.
  EnumAnalyzer enums_{ctx_, *this, generics_, types_};

  // Resolves and checks every form of call.
  CallAnalyzer calls_{ctx_, *this, generics_, types_};

 public:
  /** Start with an empty global scope holding the builtin functions. */
  explicit SemanticAnalyzer(std::shared_ptr<sun::TypeRegistry> registry)
      : ctx_(std::move(registry)) {}

  /** Scopes, symbol tables and the type registry of this analysis run. */
  SemanticContext &context() { return ctx_; }

  /** Monomorphization: the specializations this run has built. */
  GenericSpecializer &generics() { return generics_; }

  /** The declaration pre-pass and its record of what is already declared. */
  DeclarationCollector &declarations() { return declarations_; }

  /** Type inference and type-annotation resolution. */
  TypeInferer &types() { return types_; }

  /** Enum definitions, variant construction, and match patterns. */
  EnumAnalyzer &enums() { return enums_; }

  /** Call resolution and checking. */
  CallAnalyzer &calls() { return calls_; }

  /** The global scope, for debugging and visualization. */
  const SemanticScope &getRootScope() const { return ctx_.rootScope(); }

  /** Clear resolved types on an AST tree (for re-analysis of generic methods).
   */
  void clearResolvedTypes(ExprAST &expr);

  /** Main entry point: analyze a top-level expression or statement. */
  void analyze(ExprAST &expr);

  /**
   * Analyze an expression: resolve its type, check it, and record what codegen
   * needs. expectedType is an optional hint from the context, such as the
   * declared type of the variable being assigned.
   */
  void analyzeExpr(ExprAST &expr, sun::TypePtr expectedType = nullptr);

  // ---- Per-node handlers -------------------------------------------------
  //
  // analyzeExpr dispatches one of these per AST node kind. Each resolves its
  // node's type, checks it, and records what codegen needs; the dispatcher
  // itself only picks which one to run.

  /**
   * Reject a '<'_>' lambda type in return position: its captured
   * environment lives in a stack frame that dies when the function returns.
   */
  void rejectRefEnvReturnType(const std::optional<TypeAnnotation> &returnType,
                              const Position &location,
                              bool allowNamed = false);

  // Lifetime names usable at the current analysis point: the enclosing
  // class or interface's declared lifetimes, plus the enclosing function's
  // while its signature and body are analyzed. The builtin 'this is not
  // stored here - allowThisLifetime_ below governs it.
  std::vector<std::string> activeLifetimeNames_;

  // True while class or interface members are analyzed - the only places
  // the builtin 'this lifetime may appear.
  bool allowThisLifetime_ = false;

  /**
   * Reject any lifetime name the annotation uses (recursively, through
   * element, parameter, return and argument positions) that is neither an
   * active declared name nor the builtin 'this where 'this is legal.
   */
  void checkAnnotationLifetimes(const TypeAnnotation &annot,
                                const Position &location);

  /**
   * Validate a signature's lifetime declarations (no duplicates, no
   * collision with the enclosing class's names) and every lifetime name
   * its parameter and return annotations use.
   */
  void checkSignatureLifetimes(const PrototypeAST &proto,
                               const Position &location);

  // Declarations (analysis_declarations.cpp)
  void analyzeClassDefinition(ClassDefinitionAST &classDef);
  void analyzeInterfaceDefinition(InterfaceDefinitionAST &interfaceDef);
  void analyzeFunctionDefinition(FunctionAST &func);
  void analyzeLambdaExpr(LambdaAST &lambda);
  void analyzeModuleDefinition(ModuleAST &nsDecl);
  void analyzeMoonScope(ExprAST &expr);
  void analyzeDeclareType(DeclareTypeAST &declareExpr);

  // Control flow (analysis_control_flow.cpp)
  void analyzeIfExpr(IfExprAST &ifExpr);
  void analyzeMatchExpr(MatchExprAST &matchExpr, sun::TypePtr expectedType);
  void analyzeTernaryExpr(TernaryExprAST &ternary, sun::TypePtr expectedType);
  void analyzeForLoop(ForExprAST &forExpr);
  void analyzeForInLoop(ForInExprAST &forInExpr);
  void analyzeTryCatch(TryCatchExprAST &tryCatchExpr);
  void analyzeThrowExpr(ThrowExprAST &throwExpr);
  void analyzeUnsafeBlock(UnsafeBlockAST &unsafeBlock);
  void analyzeReturnExpr(ReturnExprAST &returnExpr);

  // Bindings and assignments (analysis_statements.cpp)
  void analyzeVariableCreation(VariableCreationAST &varCreate);
  void analyzeVariableAssignment(VariableAssignmentAST &varAssign);
  void analyzeCompoundAssignment(CompoundAssignmentAST &compound);
  void analyzeMemberAssignment(MemberAssignmentAST &memberAssign);
  void analyzeIndexedAssignment(IndexedAssignmentAST &assignment);
  void analyzeReferenceCreation(ReferenceCreationAST &refCreate);

  // Value expressions (analysis_expressions.cpp)
  void analyzeNumberLiteral(ExprAST &expr, sun::TypePtr expectedType);
  void analyzeArrayLiteral(ArrayLiteralAST &arrLit,
                           sun::TypePtr expectedType = nullptr);
  void analyzeIndexExpr(IndexAST &arrIdx);
  void analyzeSliceExpr(ExprAST &expr);
  void analyzeBinaryExpr(BinaryExprAST &binExpr, sun::TypePtr expectedType);
  void analyzeUnaryExpr(UnaryExprAST &unaryExpr);
  void analyzeMemberAccess(MemberAccessAST &memberAccess,
                           sun::TypePtr expectedType);
  void analyzeQualifiedName(QualifiedNameAST &qualName);

  /**
   * Extract function signature info (param types, captures, explicit return
   * type). Sets captures on the prototype and handles auto-ref conversion for
   * params. Does NOT register the function — caller is responsible for that.
   * Returns FunctionInfo with returnType set if explicit, nullptr if needs
   * inference.
   */
  FunctionInfo getFunctionInfo(FunctionAST &func);

  /** The same for a lambda: parameter types, captures, and return type. */
  FunctionInfo getLambdaInfo(LambdaAST &lambda);

  /**
   * Apply FunctionInfo to a prototype (sets captures, param types, return
   * type).
   */
  void applyFunctionInfoToProto(PrototypeAST &proto, const FunctionInfo &info);

  /**
   * Analyze a function body. Call getFunctionInfo first to get signature info.
   * If return type was not explicit, this infers it and updates the prototype.
   * Does NOT register the function — caller is responsible for that.
   */
  void analyzeFunction(FunctionAST &func);

  /** The same for a lambda body. */
  void analyzeLambda(LambdaAST &lambda);

  /**
   * Reject extern signatures that have no C spelling. Primitives, raw_ptr<T>,
   * `ref T` (C's T*) and objects by value all lower correctly; arrays,
   * slices, interfaces and lambdas do not, and must error rather than
   * silently miscompile.
   */
  void validateExternSignature(FunctionAST &func);

  /**
   * Analyze a partial class definition. Partial classes add methods to an
   * existing primary class. If the primary has been analyzed, merges now;
   * otherwise stashes for later merging.
   */
  void analyzePartialClass(ClassDefinitionAST &classDef, ExprAST &expr);

  /**
   * Validate that a type parameter exists when the type is a
   * TypeParameterType. Throws an error with source location if the type
   * parameter is not found.
   */
  void validateTypeParameter(const sun::TypePtr &type, const ExprAST &node);

  /**
   * Validate that an identifier name is not reserved (doesn't start with '_').
   * Throws an error if the name is reserved.
   */
  void validateNotReserved(const std::string &name, const std::string &kind,
                           std::optional<Position> location);

  /**
   * Analyze a method body with type bindings.
   * Runs semantic analysis with 'this' bound to the given class type.
   * @param methodFunc The method to analyze (must have a body)
   * @param classType The class type for 'this' parameter binding
   * @param typeParams Type parameter names to bind
   * @param typeArgs Type argument values for the type parameters
   */
  void analyzeMethodWithBindings(FunctionAST &methodFunc,
                                 std::shared_ptr<sun::ClassType> classType,
                                 const std::vector<std::string> &typeParams,
                                 const std::vector<sun::TypePtr> &typeArgs);

  /**
   * Analyze a block: register its declarations first, so their order within
   * the block does not matter, then analyze each statement in turn.
   */
  void analyzeBlock(BlockExprAST &block);

  /**
   * Throw unless `target` is something a borrow can bind: an addressable
   * lvalue that is not a slice, a class __index__ result, or a packed field.
   */
  void validateBorrowTarget(const ExprAST &target, const Position &loc);

  /**
   * Throw when `target` names a variable the enclosing lambda picked up
   * without a capture list. That capture is the closure's own copy, so a
   * borrow of it would alias the copy rather than the original. Every borrow
   * site calls this, so the explanation is worded once.
   */
  void rejectBorrowOfByValueCapture(const ExprAST &target, const Position &loc);

  /**
   * Constness. A place (`x`, `x.f`, `x[i]`, `this.f`, `a ? x : y`, a call
   * result) cannot be changed when its base is a `const` variable, a
   * `const ref`, or `this` inside a const method. Returns why, or an empty
   * string when the place may be changed.
   */
  std::string immutableBaseOf(const ExprAST &place);

  /** Throws "Cannot <action> <why>" when `place` cannot be changed. */
  void requireMutablePlace(const ExprAST &place, const std::string &action,
                           const Position &loc);

  /**
   * `value` is consumed by value: a compound field read out of an immutable
   * object (a partial move) or a constant global is rejected.
   */
  void checkMoveSource(const ExprAST &value, const Position &loc);

  /**
   * An argument bound to a `ref T` parameter must be a mutable place; one
   * bound to a by-value compound parameter is a move (see checkMoveSource).
   */
  void checkArgumentPlaces(const std::vector<std::unique_ptr<ExprAST>> &args,
                           const std::vector<sun::TypePtr> &paramTypes,
                           const std::string &callee, const Position &loc);

  /**
   * Calling `method` on `receiver`: a non-const method needs a mutable
   * receiver. Returns true when the receiver is immutable, so a `ref T`
   * result must be downgraded to `const ref T`.
   */
  bool checkMethodReceiver(const ExprAST &receiver, const std::string &name,
                           bool methodIsConst, bool isConstructor,
                           const Position &loc);

  // Packed class rules (see include/packed_layout.h for what "packed" means).
  // Each rejects one way a packed field's layout guarantee could be violated.

  /** A packed field has no guaranteed alignment, so it cannot be borrowed. */
  void checkPackedFieldNotBorrowed(const ExprAST &target,
                                   const Position &loc) const;

  /** The same rule for an argument passed to a `ref T` parameter. */
  void checkPackedRefArguments(
      const std::vector<std::unique_ptr<ExprAST>> &args,
      const std::vector<sun::TypePtr> &paramTypes) const;

  /** Reject a field type a packed class cannot lay out. */
  void checkPackedFieldType(const ClassDefinitionAST &classDef,
                            const ClassFieldDecl &field,
                            const sun::TypePtr &fieldType) const;

  /**
   * Copy the fields an implemented interface declares onto the class. Must run
   * before its methods are analyzed, since they may read those fields.
   */
  void inheritInterfaceFields(const ClassDefinitionAST &classDef,
                              std::shared_ptr<sun::ClassType> classType);

  /**
   * Check that a class implements every method its interfaces require, with
   * matching signatures and constness.
   */
  void validateInterfaceImplementation(
      const ClassDefinitionAST &classDef,
      std::shared_ptr<sun::ClassType> classType);

  // Module/namespace support (module scopes are tracked via the scope stack)
  // enterModuleScope() and exitScope() are used to manage module scopes

 private:
  /**
   * Extract type guard pattern from condition (_is<T>(var)).
   * Returns (varName, narrowedType) if matched.
   */
  std::optional<std::pair<std::string, sun::TypePtr>> extractTypeGuard(
      const ExprAST &cond);

  /**
   * Validate parameter names and resolve their types from prototype.
   * Throws if any parameter name is reserved; applies auto-ref conversion.
   * Returns the resolved param types and sets them on the prototype.
   *
   * allowByValueObjects exempts C externs from REQUIRE_REF_FOR_COMPOUND_PARAMS
   * when that policy is enabled: passing a struct by value is what the C ABI
   * specifies, so it is the callee's signature rather than a Sun choice.
   */
  std::vector<sun::TypePtr> validateAndResolveParamTypes(
      PrototypeAST &proto, std::optional<Position> loc = std::nullopt,
      bool allowByValueObjects = false);

  /**
   * Reject access to C-owned global storage outside an unsafe block. (Calls
   * into C and unsafe intrinsics are gated the same way by CallAnalyzer.)
   */
  void checkExternVariableAccessAllowed(const VariableInfo &info,
                                        const std::string &displayName,
                                        const Position &loc) const;

  /**
   * Check `mod.name = value`: the target must be a visible, assignable
   * module-level variable, and the value must fit its type. Also records the
   * global's symbol name on the node for codegen.
   */
  void analyzeModuleGlobalAssignment(MemberAssignmentAST &assign,
                                     const sun::Type &objectType);

  /**
   * The variables an expression reads but does not bind — what a lambda has
   * to capture. `bound` names the ones already in scope.
   */
  std::set<std::string> collectFreeVariables(
      const ExprAST &expr, const std::set<std::string> &bound);

  /**
   * The same over a block, adding each declaration to `bound` as it is
   * reached so later statements do not count it as free.
   */
  std::set<std::string> collectFreeVariablesInBlock(
      const BlockExprAST &block, std::set<std::string> bound);

  /**
   * The same for a lambda, marking the ones its `[ref x]` list asks to
   * capture by reference.
   */
  std::vector<Capture> buildCaptures(const LambdaAST &lambda);

  /**
   * Resolve a `{ field: value }` literal against the type the context
   * expects. A struct literal has no type of its own, so without an expected
   * class type there is nothing to check the field names against.
   */
  void analyzeStructLiteral(StructLiteralAST &literal,
                            const sun::TypePtr &expectedType);

  /**
   * If the member access names a class method in value position, resolve it
   * as a bound method reference: pick the overload (using expectedType when
   * the name is overloaded), set a LambdaType resolved type and the
   * isBoundMethodRef flag. No-op for fields, non-class receivers, and
   * call-position callees (those never route through here).
   */
  void maybeResolveBoundMethodRef(MemberAccessAST &memberAccess,
                                  sun::TypePtr expectedType);

};
