// semantic_analyzer.h — Shared semantic state and checking helpers
//
// The semantic session shares state and checking helpers with these classes:
//   SemanticContext       scopes, symbol tables, the type registry
//   DeclarationCollectionPass  declaration registration
//   GenericSpecializer    monomorphization and its cache
//   type_analysis::TypeResolver  annotations, substitutions, and const views
//   CallAnalyzer          what a call calls, and how its arguments get there
//   EnumAnalyzer          enum definitions, variants, and match patterns
// They share SemanticContext by reference. SemanticPipeline owns the passes;
// BodyAnalyzer traverses prepared statements. The analyzer's expression
// helpers check nodes and record the types and conversions codegen consumes.
//
// Its implementation is split by topic across src/semantic_analysis/:
//   body_analyzer.cpp   statement traversal and function scopes
//   analysis.cpp             expression and signature checking
//   analysis_utils.cpp       places, constness, `_is<T>` type guards
//   call_analyzer.cpp        every form of call (its own class, see above)
//   captures.cpp             free variables and closure captures
//   enum_analyzer.cpp        enum definitions, construction, match
//   interfaces.cpp           interfaces and conformance validation
//   packed_classes.cpp       the rules a packed class has to obey
//
// Type-related helpers live in semantic_analysis/type_analysis. Pure inference
// and compatibility checks use prepared types; resolution and contextual rules
// may use semantic state or annotate syntax. Other shared checks live alongside
// the analyzer in symbol_names, access_checker, argument_conversion, and
// expression_properties.

#pragma once

/** Provides shared diagnostics, source tracking, and compiler utilities. */
namespace sun::support {
struct Position;
}

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/type_annotation.h"
#include "semantic_analysis/access_checker.h"
#include "semantic_analysis/body_analyzer.h"
#include "semantic_analysis/call_analyzer.h"
#include "semantic_analysis/enum_analyzer.h"
#include "semantic_analysis/generic_specializer.h"
#include "semantic_analysis/inference_result.h"
#include "semantic_analysis/passes/declaration_collection_pass.h"
#include "semantic_analysis/semantic_context.h"
#include "semantic_analysis/semantic_pipeline.h"
#include "semantic_analysis/semantic_scope.h"
#include "semantic_analysis/type_analysis/type_resolver.h"
#include "semantic_analysis/type_registry.h"

// Forward declarations
/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::ast::ClassDefinitionAST;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::ast::LambdaAST;
using sun::ast::PrototypeAST;

}  // namespace sun::semantic_analysis

/** Resolves names and types and checks program semantics. */
namespace sun::semantic_analysis {

/**
 * Alias for use in this header and semantic analyzer implementations
 */
using QualifiedName = sun::semantic_analysis::QualifiedName;

/**
 * Own the semantic context, pipeline, and expression-checking helpers.
 * The pipeline owns the passes, which share this session's state and helpers.
 */
class SemanticAnalyzer {
  // Scopes, symbol tables, the type registry and the current class. Shared by
  // reference with everything else this analysis run is made of.
  SemanticContext ctx_;

  // One persistent pipeline owns all passes for this analysis session.
  sun::semantic_analysis::SemanticPipeline pipeline_{*this};

  // Recursively checks statements and manages function scopes.
  BodyAnalyzer bodies_{ctx_, *this};

  // Builds and caches every specialization the program asks for.
  GenericSpecializer generics_{ctx_, *this};

  // Resolve written types and scoped substitutions, requesting specializations
  // as needed.
  type_analysis::TypeResolver resolver_{ctx_, generics_};

  // Checks enum definitions, variant construction, and match patterns.
  EnumAnalyzer enums_{ctx_, *this, generics_, resolver_};

  // Resolves and checks every form of call.
  CallAnalyzer calls_{ctx_, *this, generics_, resolver_};

 public:
  /** Create the shared context, checking helpers, and pipeline for a program.
   */
  explicit SemanticAnalyzer(
      std::shared_ptr<sun::semantic_analysis::TypeRegistry> registry)
      : ctx_(std::move(registry)) {}

  /** Keep pass and helper references tied to this session. */
  SemanticAnalyzer(const SemanticAnalyzer &) = delete;
  /** Disallows assignment so ownership and object identity cannot be duplicated. */
  SemanticAnalyzer &operator=(const SemanticAnalyzer &) = delete;

  /** Scopes, symbol tables and the type registry of this analysis run. */
  SemanticContext &context() { return ctx_; }

  /** The persistent pipeline that owns and orders this session's passes. */
  sun::semantic_analysis::SemanticPipeline &pipeline() { return pipeline_; }

  /** Recursive statement and function-body checking. */
  BodyAnalyzer &bodies() { return bodies_; }

  /** Monomorphization: the specializations this run has built. */
  GenericSpecializer &generics() { return generics_; }

  /** Type-annotation resolution and scoped substitutions. */
  type_analysis::TypeResolver &typeResolver() { return resolver_; }

  /** Enum definitions, variant construction, and match patterns. */
  EnumAnalyzer &enums() { return enums_; }

  /** Call resolution and checking. */
  CallAnalyzer &calls() { return calls_; }

  /** The global scope, for debugging and visualization. */
  const SemanticScope &getRootScope() const { return ctx_.rootScope(); }

  /** Clear resolved types on an AST tree (for re-analysis of generic methods).
   */
  void clearResolvedTypes(ExprAST &expr);

  /**
   * Check an expression after declaration passes have run. Resolve its type
   * and record what codegen
   * needs. expectedType is an optional hint from the context, such as the
   * declared type of the variable being assigned.
   */
  void analyzeExpr(ExprAST &expr, sun::types::TypePtr expectedType = nullptr);

  /** Require a type already established by semantic analysis. */
  static sun::types::TypePtr requireResolvedType(const ExprAST &expr);
  /** Resolve a module identity and enforce its visibility. */
  sun::types::TypePtr resolveModuleReference(const ExprAST &expr);
  /** Resolve a variable or callable name, recording its declaration identity.
   */
  sun::types::TypePtr resolveVariableReferenceType(
      const sun::ast::VariableReferenceAST &expr);
  /** Resolve a member against its prepared receiver and record its identity. */
  sun::types::TypePtr resolveMemberType(const sun::ast::MemberAccessAST &expr);
  /** Resolve the receiver member and enforce access rules. */
  sun::types::TypePtr resolveModuleMemberType(
      const sun::ast::MemberAccessAST &expr,
      const sun::types::TypePtr &objectType, const std::string &memberName);
  /** Resolve the receiver member and enforce access rules. */
  sun::types::TypePtr resolveClassMemberType(
      const sun::ast::MemberAccessAST &expr,
      const sun::types::TypePtr &objectType, const std::string &memberName);
  /** Resolve the receiver member and enforce access rules. */
  sun::types::TypePtr resolveInterfaceMemberType(
      const sun::ast::MemberAccessAST &expr,
      const sun::types::TypePtr &objectType, const std::string &memberName);
  /** Resolve the receiver member and enforce access rules. */
  sun::types::TypePtr resolveTypeParameterMemberType(
      const sun::ast::MemberAccessAST &expr,
      const sun::types::TypePtr &objectType, const std::string &memberName);

  /** Select a block result from statements analyzed in their original scope. */
  sun::types::TypePtr preparedBlockType(const sun::ast::BlockExprAST &block);
  /** Select the first reachable match value after pattern and body checking. */
  sun::types::TypePtr preparedMatchType(const sun::ast::MatchExprAST &match);

  /** Compute an array result from checked elements and a separate contextual
   * hint. */
  void resolveArrayLiteralResult(sun::ast::ArrayLiteralAST &literal,
                                 sun::types::TypePtr expectedType);

  // ---- Per-node handlers -------------------------------------------------
  //
  // analyzeExpr dispatches one of these per AST node kind. Each resolves its
  // node's type, checks it, and records what codegen needs; the dispatcher
  // itself only picks which one to run.

  /**
   * Reject a '<'_>' lambda type in return position: its captured
   * environment lives in a stack frame that dies when the function returns.
   */
  void rejectRefEnvReturnType(
      const std::optional<sun::ast::TypeAnnotation> &returnType,
      const sun::support::Position &location, bool allowNamed = false);

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
  void checkAnnotationLifetimes(const sun::ast::TypeAnnotation &annot,
                                const sun::support::Position &location);

  /**
   * Validate a signature's lifetime declarations (no duplicates, no
   * collision with the enclosing class's names) and every lifetime name
   * its parameter and return annotations use.
   */
  void checkSignatureLifetimes(const PrototypeAST &proto,
                               const sun::support::Position &location);

  /**
   * Declarations (analysis_declarations.cpp)
   */
  void analyzeClassDefinition(ClassDefinitionAST &classDef);
  /**
   * Resolves declarations and checks types in this interface definition, recording the
   * results on its syntax nodes.
   */
  void analyzeInterfaceDefinition(
      sun::ast::InterfaceDefinitionAST &interfaceDef);
  /**
   * Resolves declarations and checks types in this function definition, recording the
   * results on its syntax nodes.
   */
  void analyzeFunctionDefinition(FunctionAST &func);
  /**
   * Resolves declarations and checks types in this lambda expression, recording the
   * results on its syntax nodes.
   */
  void analyzeLambdaExpr(LambdaAST &lambda);
  /**
   * Resolves declarations and checks types in this module definition, recording the
   * results on its syntax nodes.
   */
  void analyzeModuleDefinition(sun::ast::ModuleAST &nsDecl);
  /**
   * Resolves declarations and checks types in this moon scope, recording the results on
   * its syntax nodes.
   */
  void analyzeMoonScope(ExprAST &expr);
  /**
   * Resolves declarations and checks types in this declare type, recording the results
   * on its syntax nodes.
   */
  void analyzeDeclareType(sun::ast::DeclareTypeAST &declareExpr);

  /**
   * Control flow (analysis_control_flow.cpp)
   */
  void analyzeIfExpr(sun::ast::IfExprAST &ifExpr);
  /**
   * Resolves declarations and checks types in this match expression, recording the
   * results on its syntax nodes.
   */
  void analyzeMatchExpr(sun::ast::MatchExprAST &matchExpr,
                        sun::types::TypePtr expectedType);
  /**
   * Resolves declarations and checks types in this ternary expression, recording the
   * results on its syntax nodes.
   */
  void analyzeTernaryExpr(sun::ast::TernaryExprAST &ternary,
                          sun::types::TypePtr expectedType);
  /**
   * Resolves declarations and checks types in this for loop, recording the results on
   * its syntax nodes.
   */
  void analyzeForLoop(sun::ast::ForExprAST &forExpr);
  /**
   * Resolves declarations and checks types in this for in loop, recording the results on
   * its syntax nodes.
   */
  void analyzeForInLoop(sun::ast::ForInExprAST &forInExpr);
  /**
   * Resolves declarations and checks types in this try catch, recording the results on
   * its syntax nodes.
   */
  void analyzeTryCatch(sun::ast::TryCatchExprAST &tryCatchExpr);
  /**
   * Resolves declarations and checks types in this throw expression, recording the
   * results on its syntax nodes.
   */
  void analyzeThrowExpr(sun::ast::ThrowExprAST &throwExpr);
  /**
   * Resolves declarations and checks types in this unsafe block, recording the results
   * on its syntax nodes.
   */
  void analyzeUnsafeBlock(sun::ast::UnsafeBlockAST &unsafeBlock);
  /**
   * Resolves declarations and checks types in this return expression, recording the
   * results on its syntax nodes.
   */
  void analyzeReturnExpr(sun::ast::ReturnExprAST &returnExpr);

  /**
   * Bindings and assignments (analysis_statements.cpp)
   */
  void analyzeVariableCreation(sun::ast::VariableCreationAST &varCreate);
  /**
   * Resolves declarations and checks types in this variable assignment, recording the
   * results on its syntax nodes.
   */
  void analyzeVariableAssignment(sun::ast::VariableAssignmentAST &varAssign);
  /**
   * Resolves declarations and checks types in this compound assignment, recording the
   * results on its syntax nodes.
   */
  void analyzeCompoundAssignment(sun::ast::CompoundAssignmentAST &compound);
  /**
   * Resolves declarations and checks types in this member assignment, recording the
   * results on its syntax nodes.
   */
  void analyzeMemberAssignment(sun::ast::MemberAssignmentAST &memberAssign);
  /**
   * Resolves declarations and checks types in this indexed assignment, recording the
   * results on its syntax nodes.
   */
  void analyzeIndexedAssignment(sun::ast::IndexedAssignmentAST &assignment);
  /**
   * Resolves declarations and checks types in this reference creation, recording the
   * results on its syntax nodes.
   */
  void analyzeReferenceCreation(sun::ast::ReferenceCreationAST &refCreate);

  /**
   * Value expressions (analysis_expressions.cpp)
   */
  void analyzeNumberLiteral(ExprAST &expr, sun::types::TypePtr expectedType);
  /**
   * Resolves declarations and checks types in this array literal, recording the results
   * on its syntax nodes.
   */
  void analyzeArrayLiteral(sun::ast::ArrayLiteralAST &arrLit,
                           sun::types::TypePtr expectedType = nullptr);
  /**
   * Resolves declarations and checks types in this index expression, recording the
   * results on its syntax nodes.
   */
  void analyzeIndexExpr(sun::ast::IndexAST &arrIdx);
  /**
   * Resolves declarations and checks types in this slice expression, recording the
   * results on its syntax nodes.
   */
  void analyzeSliceExpr(ExprAST &expr);
  /**
   * Resolves declarations and checks types in this binary expression, recording the
   * results on its syntax nodes.
   */
  void analyzeBinaryExpr(sun::ast::BinaryExprAST &binExpr,
                         sun::types::TypePtr expectedType);
  /**
   * Resolves declarations and checks types in this unary expression, recording the
   * results on its syntax nodes.
   */
  void analyzeUnaryExpr(sun::ast::UnaryExprAST &unaryExpr);
  /**
   * Resolves declarations and checks types in this member access, recording the results
   * on its syntax nodes.
   */
  void analyzeMemberAccess(sun::ast::MemberAccessAST &memberAccess,
                           sun::types::TypePtr expectedType);
  /**
   * Resolves declarations and checks types in this qualified name, recording the results
   * on its syntax nodes.
   */
  void analyzeQualifiedName(sun::ast::QualifiedNameAST &qualName);

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
   * Reject extern signatures that have no C spelling. Primitives, raw_ptr&lt;T&gt;,
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
  void validateTypeParameter(const sun::types::TypePtr &type,
                             const ExprAST &node);

  /**
   * Validate that an identifier name is not reserved (doesn't start with '_').
   * Throws an error if the name is reserved.
   */
  void validateNotReserved(const std::string &name, const std::string &kind,
                           std::optional<sun::support::Position> location);

  /**
   * Throw unless `target` is something a borrow can bind: an addressable
   * lvalue that is not a slice, a class __index__ result, or a packed field.
   */
  void validateBorrowTarget(const ExprAST &target,
                            const sun::support::Position &loc);

  /**
   * Throw when `target` names a variable the enclosing lambda picked up
   * without a capture list. That capture is the closure's own copy, so a
   * borrow of it would alias the copy rather than the original. Every borrow
   * site calls this, so the explanation is worded once.
   */
  void rejectBorrowOfByValueCapture(const ExprAST &target,
                                    const sun::support::Position &loc);

  /**
   * Constness. A place (`x`, `x.f`, `x[i]`, `this.f`, `a ? x : y`, a call
   * result) cannot be changed when its base is a `const` variable, a
   * `const ref`, or `this` inside a const method. Returns why, or an empty
   * string when the place may be changed.
   */
  std::string immutableBaseOf(const ExprAST &place);

  /** Throws "Cannot <action> <why>" when `place` cannot be changed. */
  void requireMutablePlace(const ExprAST &place, const std::string &action,
                           const sun::support::Position &loc);

  /**
   * `value` is consumed by value: a compound field read out of an immutable
   * object (a partial move) or a constant global is rejected.
   */
  void checkMoveSource(const ExprAST &value, const sun::support::Position &loc);

  /**
   * An argument bound to a `ref T` parameter must be a mutable place; one
   * bound to a by-value compound parameter is a move (see checkMoveSource).
   */
  void checkArgumentPlaces(const std::vector<std::unique_ptr<ExprAST>> &args,
                           const std::vector<sun::types::TypePtr> &paramTypes,
                           const std::string &callee,
                           const sun::support::Position &loc);

  /** Require an unsafe block when a call has a caller-side safety contract. */
  void checkUnsafeCall(bool requiresUnsafe, const std::string &name,
                       const sun::support::Position &loc) const;

  /**
   * Calling `method` on `receiver`: a non-const method needs a mutable
   * receiver. Returns true when the receiver is immutable, so a `ref T`
   * result must be downgraded to `const ref T`.
   */
  bool checkMethodReceiver(const ExprAST &receiver, const std::string &name,
                           bool methodIsConst, bool isConstructor,
                           const sun::support::Position &loc);

  // Packed class rules (see include/packed_layout.h for what "packed" means).
  // Each rejects one way a packed field's layout guarantee could be violated.

  /** A packed field has no guaranteed alignment, so it cannot be borrowed. */
  void checkPackedFieldNotBorrowed(const ExprAST &target,
                                   const sun::support::Position &loc) const;

  /** The same rule for an argument passed to a `ref T` parameter. */
  void checkPackedRefArguments(
      const std::vector<std::unique_ptr<ExprAST>> &args,
      const std::vector<sun::types::TypePtr> &paramTypes) const;

  /** Reject a field type a packed class cannot lay out. */
  void checkPackedFieldType(const ClassDefinitionAST &classDef,
                            const sun::ast::ClassFieldDecl &field,
                            const sun::types::TypePtr &fieldType) const;

  /**
   * Copy the fields an implemented interface declares onto the class. Must run
   * before its methods are analyzed, since they may read those fields.
   */
  void inheritInterfaceFields(const ClassDefinitionAST &classDef,
                              std::shared_ptr<sun::types::ClassType> classType);

  /**
   * Check that a class implements every method its interfaces require, with
   * matching signatures and constness.
   */
  void validateInterfaceImplementation(
      const ClassDefinitionAST &classDef,
      std::shared_ptr<sun::types::ClassType> classType);

  // Module/namespace support (module scopes are tracked via the scope stack)
  // enterModuleScope() and exitScope() are used to manage module scopes

 private:
  /**
   * Extract type guard pattern from condition (_is&lt;T&gt;(var)).
   * Returns (varName, narrowedType) if matched.
   */
  std::optional<std::pair<std::string, sun::types::TypePtr>> extractTypeGuard(
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
  std::vector<sun::types::TypePtr> validateAndResolveParamTypes(
      PrototypeAST &proto,
      std::optional<sun::support::Position> loc = std::nullopt,
      bool allowByValueObjects = false);

  /**
   * Reject access to C-owned global storage outside an unsafe block. (Calls
   * into C and unsafe intrinsics are gated the same way by CallAnalyzer.)
   */
  void checkExternVariableAccessAllowed(
      const VariableInfo &info, const std::string &displayName,
      const sun::support::Position &loc) const;

  /**
   * Check `mod.name = value`: the target must be a visible, assignable
   * module-level variable, and the value must fit its type. Also records the
   * global's symbol name on the node for codegen.
   */
  void analyzeModuleGlobalAssignment(sun::ast::MemberAssignmentAST &assign,
                                     const sun::types::Type &objectType);

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
      const sun::ast::BlockExprAST &block, std::set<std::string> bound);

  /**
   * The same for a lambda, marking the ones its `[ref x]` list asks to
   * capture by reference.
   */
  std::vector<sun::ast::Capture> buildCaptures(const LambdaAST &lambda);

  /**
   * Resolve a `{ field: value }` literal against the type the context
   * expects. A struct literal has no type of its own, so without an expected
   * class type there is nothing to check the field names against.
   */
  void analyzeStructLiteral(sun::ast::StructLiteralAST &literal,
                            const sun::types::TypePtr &expectedType);

  /**
   * If the member access names a class method in value position, resolve it
   * as a bound method reference: pick the overload (using expectedType when
   * the name is overloaded), set a LambdaType resolved type and the
   * isBoundMethodRef flag. No-op for fields, non-class receivers, and
   * call-position callees (those never route through here).
   */
  void maybeResolveBoundMethodRef(sun::ast::MemberAccessAST &memberAccess,
                                  sun::types::TypePtr expectedType);
};

}  // namespace sun::semantic_analysis
