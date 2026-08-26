// semantic_analyzer.h — Pre-codegen semantic analysis pass

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "semantic_analysis/access_checker.h"
#include "semantic_analysis/semantic_scope.h"

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
class SemanticAnalyzer : public AccessContext {
  // Type registry for class/interface types (shared with codegen)
  std::shared_ptr<sun::TypeRegistry> typeRegistry;

  // Scope tree — rootScope is the global scope, currentScope walks the tree
  std::shared_ptr<GlobalScope> rootScope = std::make_shared<GlobalScope>();
  SemanticScope *currentScope = rootScope.get();

  // Track classes currently being instantiated (to detect/break mutual
  // recursion)
  std::set<std::string> classesBeingInstantiated;

  // Cache of specialized (monomorphized) functions: mangledName →
  // SpecializedFunctionInfo
  std::map<std::string, SpecializedFunctionInfo> specializedFunctionCache;

  // Current class being analyzed (for 'this' resolution)
  std::shared_ptr<sun::ClassType> currentClass = nullptr;

  // Symbols defined at module level (depth 0) — used to detect
  // redefinition errors for classes, interfaces, and enums.
  std::unordered_set<std::string> definedSymbols_;

  // Pending class extensions collected during import processing.
  // Maps class name → list of extension ASTs to merge when primary is analyzed.
  std::unordered_map<std::string, std::vector<ClassDefinitionAST *>>
      pendingExtensions_;

  // Classes (by mangled name) whose fields and method signatures were
  // registered by the declaration pre-pass. The sequential pass skips
  // re-adding them and only analyzes bodies.
  std::unordered_set<std::string> preRegisteredClassShapes_;

  /**
   * Register a non-generic class's fields and method signatures on its
   * ClassType so that any body analyzed afterwards — including bodies of
   * generic specializations triggered from function signatures — can call
   * its methods regardless of declaration order.
   */
  void registerClassShape(ClassDefinitionAST &classDef,
                          const sun::QualifiedName &qualifiedClass,
                          std::shared_ptr<sun::ClassType> classType);

  /** Bind a `using` declaration in the current scope (idempotent). */
  void registerUsing(UsingAST &usingDecl);

  // Depth of the declaration pre-pass (collectDeclarations). Generic class
  // specializations requested while > 0 register their type and method
  // signatures immediately (so shapes and signatures can refer to them) but
  // defer method-body analysis to the end of the outermost pre-pass, once
  // every declaration in the program is registered.
  int declarationPrepassDepth_ = 0;

  struct DeferredSpecialization {
    std::shared_ptr<sun::ClassType> specializedClass;
    const GenericClassInfo *genericInfo;
    std::vector<sun::TypePtr> typeArgs;
    std::shared_ptr<ClassDefinitionAST> specializedAST;  // bodies unanalyzed
  };
  std::vector<DeferredSpecialization> deferredSpecializations_;

  /**
   * Analyze the method bodies the pre-pass deferred, now that every
   * declaration in the program is registered. A body may ask for further
   * specializations; those are analyzed straight away.
   */
  void analyzeDeferredSpecializations();

  /** True when not inside any function scope (i.e. at module/global level). */
  bool isAtModuleLevel() const {
    for (auto *s = currentScope; s != nullptr; s = s->parent)
      if (s->getType() == ScopeType::Function) return false;
    return true;
  }

  /** Nearest enclosing function scope, or nullptr at module/global level. */
  FunctionScope *currentFunctionScope() const {
    for (auto *s = currentScope; s != nullptr; s = s->parent)
      if (s->getType() == ScopeType::Function)
        return static_cast<FunctionScope *>(s);
    return nullptr;
  }

 public:
  /** Start with an empty global scope holding the builtin functions. */
  explicit SemanticAnalyzer(std::shared_ptr<sun::TypeRegistry> registry)
      : typeRegistry(std::move(registry)) {
    rootScope->accessContext = this;  // lookups filter by visibility
    registerBuiltinFunctions();
  }

  /** The global scope, for debugging and visualization. */
  const SemanticScope &getRootScope() const { return *rootScope; }

  /** Main entry point: analyze a top-level expression or statement. */
  void analyze(ExprAST &expr);

  /**
   * Declaration pre-pass: register all functions, classes, interfaces, enums,
   * and modules in a block before analyzing bodies. This allows forward
   * references between declarations at the same scope level.
   */
  void collectDeclarations(BlockExprAST &block);

  /** Register one named function's signature in the current scope. */
  void collectFunctionSignature(FunctionAST &func);

  /**
   * Enter (creating or reusing) a module scope and record its visibility;
   * re-openings must agree.
   */
  void declareModule(const ModuleAST &module);

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
   * Infer the type of an expression without modifying it.
   * Recursively traverses the AST to compute types based on:
   * - Literals: number format determines i32 vs f64, strings are String
   * - Variables: looked up in symbol table (local scope, then global)
   * - Binary ops: comparison returns bool, arithmetic returns LHS type
   * - Calls: return type from function/lambda signature
   * - References: wraps target type in ref(T)
   * Returns f64 as fallback for unknown expressions.
   */
  sun::TypePtr inferType(const ExprAST &expr);

  /**
   * The type of `object.member`, where the object may be a value, a class or
   * enum name, or a module. Also stamps the resolved symbol name onto the
   * node for codegen.
   */
  sun::TypePtr inferType(const MemberAccessAST &expr);

  /**
   * The type a `f<T>(...)` call produces, dispatching on whether the name is
   * an intrinsic, a generic function, or a generic class.
   */
  sun::TypePtr inferGenericCallType(const GenericCallAST &call);

  /**
   * The static_ptr<T> type when `type` is a static_ptr to a non-class, else
   * null. A static_ptr<Class> dispatches to the class's own methods instead
   * of the builtin ones.
   */
  static sun::StaticPointerType *asNonClassStaticPtr(const sun::TypePtr &type);

  /** True for a builtin static_ptr<T> method name: length() or raw(). */
  static bool isStaticPtrMethod(const std::string &name);

  /**
   * The result type of a static_ptr<T> builtin method call, checking the
   * argument count.
   */
  sun::TypePtr inferStaticPtrMethodType(const sun::StaticPointerType &ptrType,
                                        const std::string &name,
                                        size_t argCount, const Position &loc);

  /** The result type of an intrinsic call (_sizeof, _load, _to_ref, ...). */
  sun::TypePtr inferIntrinsicCallType(const GenericCallAST &call);

  /**
   * The return type of a call to a generic function, instantiating the
   * specialization if it does not exist yet.
   */
  sun::TypePtr inferGenericFunctionCallType(const GenericCallAST &call);

  /**
   * The type `C<T>(...)` constructs, instantiating the generic class if that
   * specialization does not exist yet.
   */
  sun::TypePtr inferGenericClassConstructionType(const GenericCallAST &call);

  /** Register a function prototype (key = name + param types for overloads). */
  void registerFunctionInCurrentScope(const std::string &name,
                                      const FunctionInfo &info);

  /**
   * Look up a function by name and exact argument types. Returns nullopt when
   * no overload matches.
   */
  std::optional<FunctionInfo> lookupFunction(
      const std::string &name, const std::vector<sun::TypePtr> &argTypes) const;

  /** Every overload declared under the given name. */
  std::vector<FunctionInfo> getAllFunctions(const std::string &name) const;

  /** Build a function signature string: "name(type1,type2,...)". */
  static std::string getFunctionSignature(
      const std::string &name, const std::vector<sun::TypePtr> &paramTypes);

  /**
   * Find a variable by name in the scope chain (public so codegen can read
   * the resolved types).
   */
  VariableInfo *lookupVariable(const std::string &name);

  /**
   * Record a class in the current scope. A repeated registration of the same
   * name is ignored, which is what a diamond import produces.
   */
  void registerClass(const std::string &name,
                     std::shared_ptr<sun::ClassType> classType,
                     std::optional<Position> loc = std::nullopt);

  /** Find a class by name in the scope chain (null when there is none). */
  std::shared_ptr<sun::ClassType> lookupClass(const std::string &name) const;

  /** Set the class whose body is being analyzed, so `this` resolves to it. */
  void setCurrentClass(std::shared_ptr<sun::ClassType> classType);

  /** The class whose body is being analyzed, or null outside one. */
  std::shared_ptr<sun::ClassType> getCurrentClass() const;

  /**
   * A borrow binds the storage of an addressable lvalue: a variable, a field,
   * or an array element. Rejects everything else (temporaries, class
   * __index__ results, slices), so `ref r = x` and `var r: ref T = x` agree.
   */
  static bool isBorrowableLvalue(const ExprAST &target);

  /**
   * Throw unless `target` is something a borrow can bind: an addressable
   * lvalue that is not a slice, a class __index__ result, or a packed field.
   */
  void validateBorrowTarget(const ExprAST &target, const Position &loc);

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
   * Record a generic class template in the current scope, along with the
   * scope it was declared in, so its bodies resolve names as written there.
   */
  void registerGenericClass(const std::string &name,
                            const GenericClassInfo &info,
                            std::optional<Position> loc = std::nullopt);

  /** Find a generic class template by name in the scope chain. */
  const GenericClassInfo *lookupGenericClass(const std::string &name) const;

  /** The same by qualified name, for a template in a known module. */
  const GenericClassInfo *lookupGenericClass(
      const sun::QualifiedName &qualifiedName) const;

  /** The generic definition a specialized class was instantiated from. */
  const GenericClassInfo *lookupGenericClassOf(
      const sun::ClassType &specialized) const;

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

  /**
   * Record a generic function template in the current scope, along with the
   * scope it was declared in.
   */
  void registerGenericFunctionInCurrentScope(FunctionAST &func);

  /**
   * Look up a generic function by name. Tries the direct name first, then
   * falls back to enclosing function prefix + name (for nested generic
   * functions).
   */
  const GenericFunctionInfo *lookupGenericFunction(
      const std::string &name) const;

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
   * Monomorphize a generic function for the given type arguments, reusing the
   * cached specialization when there is one. Empty when it cannot be built.
   */
  std::optional<SpecializedFunctionInfo> instantiateGenericFunction(
      const GenericFunctionInfo &genericInfo,
      const std::vector<sun::TypePtr> &typeArgs);

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
   * Instantiate for a call site: same as instantiateGenericFunction, but a
   * failure is the call's error rather than an empty optional to unpack.
   */
  SpecializedFunctionInfo requireGenericSpecialization(
      const GenericFunctionInfo &genericInfo,
      const std::vector<sun::TypePtr> &typeArgs, const std::string &displayName,
      std::optional<Position> loc);

  /**
   * Instantiates a generic method on a class with specific type arguments.
   * Stores the specialization on the generic method's FunctionAST.
   * Returns the specialized FunctionAST for codegen lookup.
   * variadicArgTypes carries the resolved types of the actual variadic
   * arguments at the call site (for methods with an _init_args<T> pack). When
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

  /** Bind type parameter names to concrete types in the current scope. */
  void addTypeParameterBindings(const std::vector<std::string> &params,
                                const std::vector<sun::TypePtr> &args);

  /** The type a type parameter is bound to, searching outwards (null if none).
   */
  sun::TypePtr findTypeParameter(const std::string &name) const;

  /** The type a `type` alias names, searching outwards (null if none). */
  sun::TypePtr findTypeAlias(const std::string &name) const;

  /**
   * Record an interface in the current scope. A repeated registration of the
   * same name is ignored, which is what a diamond import produces.
   */
  void registerInterface(const std::string &name,
                         std::shared_ptr<sun::InterfaceType> interfaceType,
                         std::optional<Position> loc = std::nullopt);

  /**
   * Find an interface by name in the scope chain, falling back to the builtin
   * interfaces (IError).
   */
  std::shared_ptr<sun::InterfaceType> lookupInterface(
      const std::string &name) const;

  /** Record a generic interface template in the current scope. */
  void registerGenericInterface(const std::string &name,
                                const GenericInterfaceInfo &info,
                                std::optional<Position> loc = std::nullopt);

  /** Find a generic interface template by name in the scope chain. */
  const GenericInterfaceInfo *lookupGenericInterface(
      const std::string &name) const;

  /**
   * Monomorphize a generic interface for the given type arguments, reusing
   * the specialization if it already exists.
   */
  std::shared_ptr<sun::InterfaceType> instantiateGenericInterface(
      const std::string &baseName, const std::vector<sun::TypePtr> &typeArgs);

  /** Record an enum in the current scope. */
  void registerEnum(const std::string &name,
                    std::shared_ptr<sun::EnumType> enumType);

  /** Find an enum by name in the scope chain (null when there is none). */
  std::shared_ptr<sun::EnumType> lookupEnum(const std::string &name) const;

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

  /**
   * Record a module-level variable under both its plain and its qualified
   * name, so it can be reached from inside the module and from outside it.
   */
  void registerModuleVariable(const std::string &baseName,
                              const std::string &qualifiedName,
                              sun::TypePtr type, sun::Visibility visibility,
                              bool isConst = false);

  /**
   * Register a module-level variable imported from a .moon bundle. The stub
   * carries a type annotation and a content-hash-scoped qualified name, but
   * no initializer — the storage is in the bundle.
   */
  void registerPrecompiledModuleVariable(VariableCreationAST &varCreate);

  /**
   * Find a variable by its dotted name (`a.b.x`). An undotted name is an
   * ordinary variable lookup.
   */
  VariableInfo *lookupQualifiedVariable(const std::string &qualifiedName);

  /** Find a function by its dotted name (`a.b.f`); undotted names never match.
   */
  const FunctionInfo *lookupQualifiedFunction(
      const std::string &qualifiedName) const;

  /**
   * Resolve a bare name against the `using` imports in scope, giving the
   * qualified name it refers to (the name itself when nothing matches).
   */
  sun::QualifiedName resolveNameWithUsings(const std::string &name) const;

  /** Add a using import (legacy string-based). */
  void addUsingImport(const UsingImport &import);

  /** Add a scope-based import binding. */
  void addImportBinding(const ImportBinding &binding);

 private:
  /** True for a name starting with '_', which is reserved for builtins. */
  static bool isReservedIdentifier(const std::string &name);

  /** True for an intrinsic function name, which also starts with '_'. */
  static bool isIntrinsic(const std::string &name) {
    return !name.empty() && name[0] == '_';
  }

  /**
   * Check if a type can be assigned to another type.
   * Handles interface assignability: class C can be assigned to interface I
   * if C implements I. Also handles ref unwrapping and exact equality.
   */
  static bool isAssignableTo(const sun::TypePtr &from, const sun::TypePtr &to);

  /**
   * Try to coerce an integer literal to a target primitive type.
   * Returns true if coercion happened. If throwOnFail, throws on mismatch.
   */
  static bool tryCoerceIntegerLiteral(ExprAST *expr, sun::TypePtr targetType,
                                      bool throwOnFail = false);

  /**
   * Give an untyped numeric literal operand of a binary expression its type
   * from context: the surrounding expected type if there is one, otherwise the
   * other operand's type. Without this `u8_var + 32` would promote to the
   * literal's default i32.
   */
  static void coerceBinaryLiteralOperands(const BinaryExprAST &binExpr,
                                          const sun::TypePtr &expectedType);

  /**
   * A char only compares with a char, and never takes part in arithmetic.
   * Without this `'a' + 1` and `c == 65` would quietly fall through to the
   * integer paths, since a char is an i32 underneath.
   */
  static void checkCharOperands(const BinaryExprAST &binExpr);

  /**
   * The type an arithmetic/bitwise/shift binary expression produces: the wider
   * of the two operand types, mirroring codegen's operand unification.
   */
  static sun::TypePtr promoteBinaryOperands(const sun::TypePtr &lhsType,
                                            const sun::TypePtr &rhsType);

  /**
   * Unify the branch types of a ternary expression: exact match, or the
   * wider type when one side widens to the other (never narrows f64 to f32).
   * Throws a compile error when the types are incompatible.
   */
  static sun::TypePtr unifyTernaryTypes(const sun::TypePtr &thenType,
                                        const sun::TypePtr &elseType,
                                        std::optional<Position> loc);

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

  /** Register built-in functions (print, println, file I/O, etc.). */
  void registerBuiltinFunctions();

  /** Push a new scope of the given kind and make it current. */
  void enterScope(ScopeType type = ScopeType::Block);

  /**
   * Enter a type parameter scope with bindings (combines enterScope +
   * addTypeParameterBindings).
   */
  void enterTypeParamScope(const std::vector<std::string> &params,
                           const std::vector<sun::TypePtr> &args);

  /**
   * Enter a module's scope, creating it if this is the first time the module
   * is opened. Re-opening a module returns to the same scope.
   */
  void enterModuleScope(const std::string &moduleName);

  /** Enter a class scope with qualified name for proper scope path. */
  void enterClassScope(const sun::QualifiedName &className);

  /** Enter an interface scope with qualified name for proper scope path. */
  void enterInterfaceScope(const sun::QualifiedName &interfaceName);

  /**
   * Enter a function scope with the function's signature for nested function
   * qualified names. The signature should be "funcName(paramType1,paramType2)".
   * funcName is the qualified name of the function.
   */
  void enterFunctionScope(const std::string &funcSig,
                          const sun::QualifiedName &funcName,
                          bool canThrow = false,
                          sun::TypePtr returnType = nullptr);

  /**
   * Return type of the nearest enclosing function scope (null outside
   * functions or when unresolved); used for return-position inference.
   */
  sun::TypePtr currentFunctionReturnType() const;

  /**
   * Make the parent scope current again. The scope itself stays in the tree
   * for debugging and visualization.
   */
  void exitScope();

  /**
   * Get the current module prefix for name mangling (e.g., "sun_").
   * Returns empty string if not inside any module scope.
   */
  std::string getCurrentModulePrefix() const;

  /**
   * Get the current scope path as a vector of segments.
   * e.g., inside "module A { module B { } }", returns {"A", "B"}.
   */
  std::vector<std::string> getCurrentScopePath() const;

  /**
   * Create a QualifiedName for a symbol in the current module scope.
   * Preserves module path in display form for proper error messages.
   */
  sun::QualifiedName makeQualifiedName(const std::string &baseName) const;

  /**
   * Get the fully qualified name for a symbol in current scope.
   * e.g., inside "module sun { }", qualifyName("Vec") returns "sun_Vec".
   */
  std::string qualifyNameInCurrentModule(const std::string &name) const;

  /**
   * Check if we're currently inside a function declared with ", IError".
   * Traverses parent scopes to find the nearest function scope.
   */
  bool isInThrowingFunction() const;

  /** Check if we're currently inside a try block. */
  bool isInTryBlock() const;

  /** Enter a try block (increments the try depth counter). */
  void enterTryBlock();

  /** Leave a try block (decrements the try depth counter). */
  void exitTryBlock();

  /** Check if we're currently inside an unsafe block. */
  bool isInUnsafeBlock() const;

  /** Enter an unsafe block (increments the unsafe depth counter). */
  void enterUnsafeBlock();

  /** Leave an unsafe block (decrements the unsafe depth counter). */
  void exitUnsafeBlock();

  /**
   * True when the name refers to a module, so `x.y` can be read as a
   * qualified name rather than a member access on a value.
   */
  bool isModuleName(const std::string &name) const;

  /**
   * Traverse childModules from global scope to find a module by dot-separated
   * path (e.g., "sun" or "sun.collections"). Returns nullptr if not found.
   */
  SemanticScope *lookupModuleScope(const std::string &dotPath) const;

  /**
   * Get the full module path including library scope hashes.
   * e.g., "b" -> "$hash$.b" if b is inside a library scope.
   */
  std::string getFullModulePath(const std::string &visiblePath) const;

  /**
   * Find a symbol in a specific module path (dot-separated, user-visible),
   * traversing library scopes transparently: findSymbolInModule("b",
   * "get_version") finds b.get_version even if b is inside a library scope
   * like $hash$.b. Optional filterKind restricts to a specific symbol type
   * (None = any). Optional argTypes selects the matching overload when the
   * symbol is a function; without it the first registered overload is
   * returned. Throws when the same name is found in several libraries.
   */
  SymbolMatch findSymbolInModule(
      const std::string &modulePath, const std::string &name,
      SymbolKind filterKind = SymbolKind::None,
      const std::vector<sun::TypePtr> *argTypes = nullptr) const;

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
   * Resolve a module-qualified call `mod.foo(args...)` against the actual
   * argument types and stamp the chosen overload's own mangled name onto the
   * member access. Rebuilding the name from the module path instead would
   * drop the overload param suffix and name a symbol codegen never emits.
   * Returns nullptr if the module has no overload matching those arguments.
   */
  const FunctionInfo *resolveModuleQualifiedCall(
      const MemberAccessAST &memberAccess, const sun::TypePtr &objectType,
      const std::vector<sun::TypePtr> &argTypes) const;

  /**
   * Check `mod.name = value`: the target must be a visible, assignable
   * module-level variable, and the value must fit its type. Also records the
   * global's symbol name on the node for codegen.
   */
  void analyzeModuleGlobalAssignment(MemberAssignmentAST &assign,
                                     const sun::Type &objectType);

  /** Get all active using imports (from all enclosing scopes). */
  std::vector<UsingImport> getActiveUsingImports() const;

  /** Record a variable in the current scope under the given type. */
  void declareVariable(const std::string &name, sun::TypePtr type,
                       bool isParam = false, bool isConst = false);

  /**
   * Narrow a variable's type for the rest of the current scope, after an
   * `_is<T>` guard proved it holds a T.
   */
  void narrowVariable(const std::string &varName, sun::TypePtr narrowedType);

  /**
   * The narrowed type in effect for a variable, or its original type when no
   * guard applies. The more specific of the two wins (class over interface).
   */
  sun::TypePtr getNarrowedType(const std::string &varName,
                               sun::TypePtr originalType) const;

  /**
   * Resolve a written type annotation to a type, instantiating any generic
   * it names.
   */
  sun::TypePtr typeAnnotationToType(const TypeAnnotation &annot);

  /**
   * The const view of a type: every `ref T` in it, including inside a payload
   * enum (Option<ref T> -> Option<const ref T>), becomes `const ref T`. It is
   * what a const method's result looks like through a constant receiver, and
   * what its body returns against (type_conversion.cpp).
   */
  sun::TypePtr createConstView(sun::TypePtr type);

  /**
   * Resolve a list of written type arguments, reporting `context` in the
   * error when one of them is not a type.
   */
  std::vector<sun::TypePtr> resolveTypeArguments(
      const std::vector<std::unique_ptr<TypeAnnotation>> &typeAnnotations,
      const std::optional<Position> &location, const std::string &context);

  /**
   * Replace the type parameters in a type with what they are bound to in
   * scope, recursing through references, arrays, and generic arguments.
   */
  sun::TypePtr substituteTypeParameters(sun::TypePtr type);

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

  /** The captures a nested function needs, from its free variables. */
  std::vector<Capture> buildCaptures(const FunctionAST &func);

  /**
   * The same for a lambda, marking the ones its `[ref x]` list asks to
   * capture by reference.
   */
  std::vector<Capture> buildCaptures(const LambdaAST &lambda);

  /** Clear resolved types on an AST tree (for re-analysis of generic methods).
   */
  void clearResolvedTypes(ExprAST &expr);

  /**
   * Analyze an expression: resolve its type, check it, and record what codegen
   * needs. expectedType is an optional hint from the context, such as the
   * declared type of the variable being assigned.
   */
  void analyzeExpr(ExprAST &expr, sun::TypePtr expectedType = nullptr);

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

  /**
   * Analyze a call: resolve the callee against the argument types, check the
   * arguments against the chosen signature, and record one ArgConversion per
   * argument for codegen.
   */
  void analyzeCall(CallExprAST &callExpr, sun::TypePtr expectedType = nullptr);

  // ===== Enums (all implemented in semantic_analysis/enums.cpp) =====

  /**
   * Enum definition analysis: validation, payload resolution, registration
   * (generic enums register as templates).
   */
  void analyzeEnumDefinition(EnumDefinitionAST &enumDef);

  /**
   * Declaration-collection pre-pass: register a block's enums (and generic
   * enum templates) so function signatures collected afterwards can resolve
   * enum-typed parameters/returns.
   */
  void collectEnumDeclarations(const BlockExprAST &block);

  /**
   * Call interception for EnumName.Variant(args...) on concrete and generic
   * enums; returns true when the call was an enum construction.
   */
  bool tryAnalyzeEnumConstruction(CallExprAST &callExpr,
                                  sun::TypePtr expectedType);

  /**
   * Member-access interception for generic enum unit variants (Option.None);
   * returns true when handled (type arguments taken from the expected type).
   */
  bool tryAnalyzeGenericEnumUnitVariant(MemberAccessAST &memberAccess,
                                        sun::TypePtr expectedType);

  /**
   * Check a concrete enum variant construction, EnumName.Variant(args...),
   * against the payload the variant declares.
   */
  void analyzeEnumVariantConstruction(
      CallExprAST &callExpr, MemberAccessAST &memberAccess,
      const std::shared_ptr<sun::EnumType> &enumType);

  /**
   * Record a generic enum template in the current scope, along with the scope
   * it was declared in.
   */
  void registerGenericEnum(const std::string &name, GenericEnumInfo info);

  /** Find a generic enum template by name in the scope chain. */
  const GenericEnumInfo *lookupGenericEnum(const std::string &name) const;

  /** Instantiate Option<i32> from a generic enum template (monomorphization).
   */
  std::shared_ptr<sun::EnumType> instantiateGenericEnum(
      const std::string &baseName, const std::vector<sun::TypePtr> &typeArgs);

  /**
   * Option.Some(42): infer type arguments from payload args (falling back to
   * the expected type), instantiate, then check like a concrete construction.
   */
  void analyzeGenericEnumConstruction(CallExprAST &callExpr,
                                      MemberAccessAST &memberAccess,
                                      const std::string &genericName,
                                      const GenericEnumInfo &genericInfo,
                                      sun::TypePtr expectedType);

  /**
   * Match analysis on enum discriminants: variant patterns, payload bindings,
   * exhaustiveness.
   */
  void analyzeEnumMatch(MatchExprAST &matchExpr,
                        const std::shared_ptr<sun::EnumType> &enumType,
                        sun::TypePtr expectedType);

  /** Validate a resolved payload type for an enum variant (Stage 1 rules). */
  void validateEnumPayloadType(const sun::TypePtr &type,
                               const std::shared_ptr<sun::EnumType> &enumType,
                               const std::string &variantName,
                               const Position &location);

  /**
   * Analyze an intrinsic call: the arguments only, since codegen decides what
   * the intrinsic does.
   */
  void analyzeIntrinsicCall(GenericCallAST &genericCall);

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

  /**
   * Expand a variadic pack (`args...`) in a call's argument list into
   * concrete, already-typed VariableReferenceAST nodes ("args.0", "args.1",
   * ...), using the enclosing function scope's recorded variadic param. No-op
   * when there is no enclosing variadic param or no pack argument is present.
   */
  void expandPackArguments(std::vector<std::unique_ptr<ExprAST>> &args);

  // ---- Access control (see include/visibility.h, access_checker.h) --------
  //
  // Module-level items are filtered inside the scope lookups (AccessFilter in
  // semantic_scope.h) via the AccessContext this analyzer implements; class
  // and interface members are checked where they are resolved on the type.
  // "Which module is asking" is always the nearest Module scope on the stack:
  // generic bodies are analyzed inside their definition scope (see
  // ScopeSwitchGuard), so no override is needed.

  // Locations of the expressions being analyzed (innermost last), so denials
  // raised inside lookups can still point at source.
  std::vector<const Position *> locationStack_;

 public:
  /**
   * Make `target` the current scope for the enclosed block (restored on
   * exit, including by exception). Generic instantiation uses it to analyze
   * a template's body in the scope the template was declared in.
   */
  struct ScopeSwitchGuard {
    SemanticAnalyzer &sema;
    SemanticScope *saved;
    ScopeSwitchGuard(SemanticAnalyzer &s, SemanticScope *target)
        : sema(s), saved(s.currentScope) {
      if (target) sema.currentScope = target;
    }
    ~ScopeSwitchGuard() { sema.currentScope = saved; }
    ScopeSwitchGuard(const ScopeSwitchGuard &) = delete;
    ScopeSwitchGuard &operator=(const ScopeSwitchGuard &) = delete;
  };

  /**
   * The scope a generic template was declared in (nullptr if unknown, in
   * which case ScopeSwitchGuard keeps the current scope).
   */
  template <typename GenericInfo>
  static SemanticScope *definitionScopeOf(const GenericInfo &info) {
    return info.definitionScope.lock().get();
  }

  /**
   * The scope a class's template was declared in: for a specialization, the
   * generic's; for a plain class with generic methods, its own registration.
   * Null when the class has no template.
   */
  SemanticScope *classDefinitionScope(const sun::ClassType &classType) const;

  /**
   * Record the expression being analyzed for the enclosed block, so an access
   * denial raised deep inside a lookup can still point at source.
   */
  struct LocationGuard {
    SemanticAnalyzer &sema;
    LocationGuard(SemanticAnalyzer &s, const Position &loc) : sema(s) {
      sema.locationStack_.push_back(&loc);
    }
    ~LocationGuard() { sema.locationStack_.pop_back(); }
    LocationGuard(const LocationGuard &) = delete;
    LocationGuard &operator=(const LocationGuard &) = delete;
  };

  /** The innermost location a LocationGuard recorded, if any. */
  std::optional<Position> currentLocation() const {
    if (locationStack_.empty()) return std::nullopt;
    return *locationStack_.back();
  }

  /** The module asking for access: the nearest module scope on the stack. */
  sun::ModulePath currentModulePath() const override;

  /** Report that `item` is not reachable from here, pointing at source. */
  [[noreturn]] void denyAccess(const sun::access::ItemRef &item) const override;

  /** `deinit` is compiler-invoked and therefore always public. */
  static sun::Visibility methodVisibility(const FunctionAST &method);

  /** Throw at `loc` unless `item` is reachable from the current module. */
  void requireAccessible(const sun::access::ItemRef &item,
                         const Position &loc) const {
    sun::access::requireAccessible(currentModulePath(), item, loc);
  }

  /** The same, pointing at the innermost recorded location. */
  void requireAccessible(const sun::access::ItemRef &item) const {
    if (!isAccessible(item)) denyAccess(item);
  }

  /** True when `item` is reachable from the current module. */
  bool isAccessible(const sun::access::ItemRef &item) const {
    return sun::access::isAccessible(currentModulePath(), item);
  }

  /**
   * A class field by name: nullptr when it does not exist; throws when it
   * exists but is not accessible from here.
   */
  const sun::ClassField *accessibleField(const sun::ClassType &cls,
                                         const std::string &name,
                                         const Position &loc) const;

  /** The same for a class method, taking the first overload of that name. */
  const sun::ClassMethod *accessibleMethod(const sun::ClassType &cls,
                                           const std::string &name,
                                           const Position &loc) const;

  /** The same, picking the overload that matches the argument types. */
  const sun::ClassMethod *accessibleMethodForArgs(
      const sun::ClassType &cls, const std::string &name,
      const std::vector<sun::TypePtr> &argTypes, const Position &loc) const;

  /**
   * Throws "No matching overload" when the class has methods called `name`
   * but none of them takes `argTypes.size()` arguments. Silent otherwise, so
   * callers can still fall back on their own type-mismatch diagnostics.
   */
  void reportNoMethodForArgCount(const sun::ClassType &cls,
                                 const std::string &name,
                                 const std::vector<sun::TypePtr> &argTypes,
                                 const Position &loc) const;

  /** An interface field by name, with the same access rules as a class's. */
  const sun::InterfaceField *accessibleField(const sun::InterfaceType &iface,
                                             const std::string &name,
                                             const Position &loc) const;

  /** An interface method by name, with the same access rules as a class's. */
  const sun::InterfaceMethod *accessibleMethod(const sun::InterfaceType &iface,
                                               const std::string &name,
                                               const Position &loc) const;

  /**
   * A module named by user code (`a.b`, `using a.b;`, `b.f()`): every
   * module on its path must be visible from here.
   */
  void requireModuleAccessible(const SemanticScopeBase &moduleScope,
                               const Position &loc) const;

  /** Name a class field for a uniform access-denial message. */
  static sun::access::ItemRef fieldRef(const sun::ClassType &cls,
                                       const sun::ClassField &f);

  /** Name a class method for a uniform access-denial message. */
  static sun::access::ItemRef methodRef(const sun::ClassType &cls,
                                        const sun::ClassMethod &m);

  /** Name an interface field for a uniform access-denial message. */
  static sun::access::ItemRef fieldRef(const sun::InterfaceType &iface,
                                       const sun::InterfaceField &f);

  /** Name an interface method for a uniform access-denial message. */
  static sun::access::ItemRef methodRef(const sun::InterfaceType &iface,
                                        const sun::InterfaceMethod &m);

  /** Name a module for a uniform access-denial message. */
  static sun::access::ItemRef moduleRef(const ModuleScope &scope) {
    return accessItem(scope);
  }
};
