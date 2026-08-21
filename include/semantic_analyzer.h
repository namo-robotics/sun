// semantic_analyzer.h — Pre-codegen semantic analysis pass

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "access_checker.h"
#include "semantic_scope.h"

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
  SemanticScope* currentScope = rootScope.get();

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
  std::unordered_map<std::string, std::vector<ClassDefinitionAST*>>
      pendingExtensions_;

  // Classes (by mangled name) whose fields and method signatures were
  // registered by the declaration pre-pass. The sequential pass skips
  // re-adding them and only analyzes bodies.
  std::unordered_set<std::string> preRegisteredClassShapes_;

  // Register a non-generic class's fields and method signatures on its
  // ClassType so that any body analyzed afterwards — including bodies of
  // generic specializations triggered from function signatures — can call
  // its methods regardless of declaration order.
  void registerClassShape(ClassDefinitionAST& classDef,
                          const sun::QualifiedName& qualifiedClass,
                          std::shared_ptr<sun::ClassType> classType);

  // Bind a `using` declaration in the current scope (idempotent)
  void registerUsing(UsingAST& usingDecl);

  // Depth of the declaration pre-pass (collectDeclarations). Generic class
  // specializations requested while > 0 register their type and method
  // signatures immediately (so shapes and signatures can refer to them) but
  // defer method-body analysis to the end of the outermost pre-pass, once
  // every declaration in the program is registered.
  int declarationPrepassDepth_ = 0;

  struct DeferredSpecialization {
    std::shared_ptr<sun::ClassType> specializedClass;
    const GenericClassInfo* genericInfo;
    std::vector<sun::TypePtr> typeArgs;
    std::shared_ptr<ClassDefinitionAST> specializedAST;  // bodies unanalyzed
  };
  std::vector<DeferredSpecialization> deferredSpecializations_;
  void analyzeDeferredSpecializations();

  // True when not inside any function scope (i.e. at module/global level)
  bool isAtModuleLevel() const {
    for (auto* s = currentScope; s != nullptr; s = s->parent)
      if (s->getType() == ScopeType::Function) return false;
    return true;
  }

  // Nearest enclosing function scope, or nullptr at module/global level.
  FunctionScope* currentFunctionScope() const {
    for (auto* s = currentScope; s != nullptr; s = s->parent)
      if (s->getType() == ScopeType::Function)
        return static_cast<FunctionScope*>(s);
    return nullptr;
  }

 public:
  explicit SemanticAnalyzer(std::shared_ptr<sun::TypeRegistry> registry)
      : typeRegistry(std::move(registry)) {
    rootScope->accessContext = this;  // lookups filter by visibility
    registerBuiltinFunctions();
  }

  // Get the root scope for debugging/visualization
  const SemanticScope& getRootScope() const { return *rootScope; }

  // Main entry point: analyze a top-level expression/statement
  void analyze(ExprAST& expr);

  // Declaration pre-pass: register all functions, classes, interfaces, enums,
  // and modules in a block before analyzing bodies. This allows forward
  // references between declarations at the same scope level.
  void collectDeclarations(BlockExprAST& block);
  // Register one named function's signature in the current scope
  void collectFunctionSignature(FunctionAST& func);
  // Enter (creating or reusing) a module scope and record its visibility;
  // re-openings must agree.
  void declareModule(const ModuleAST& module);

  // Extract function signature info (param types, captures, explicit return
  // type) Sets captures on the prototype and handles auto-ref conversion for
  // params. Does NOT register the function - caller is responsible for that.
  // Returns FunctionInfo with returnType set if explicit, nullptr if needs
  // inference.
  FunctionInfo getFunctionInfo(FunctionAST& func);
  FunctionInfo getLambdaInfo(LambdaAST& lambda);

  // Apply FunctionInfo to a prototype (sets captures, param types, return type)
  void applyFunctionInfoToProto(PrototypeAST& proto, const FunctionInfo& info);

  // Analyze a function body. Call getFunctionInfo first to get signature info.
  // If return type was not explicit, this infers it and updates the prototype.
  // Does NOT register the function - caller is responsible for that.
  void analyzeFunction(FunctionAST& func);
  void analyzeLambda(LambdaAST& lambda);

  // Reject extern signatures that have no C spelling. Primitives, raw_ptr<T>,
  // `ref T` (C's T*) and objects by value all lower correctly; arrays,
  // slices, interfaces and lambdas do not, and must error rather than
  // silently miscompile.
  void validateExternSignature(FunctionAST& func);

  // Analyze a partial class definition. Partial classes add methods to an
  // existing primary class. If the primary has been analyzed, merges now;
  // otherwise stashes for later merging.
  void analyzePartialClass(ClassDefinitionAST& classDef, ExprAST& expr);

  // Validate that a type parameter exists when the type is a TypeParameterType.
  // Throws an error with source location if the type parameter is not found.
  void validateTypeParameter(const sun::TypePtr& type, const ExprAST& node);

  // Validate that an identifier name is not reserved (doesn't start with '_').
  // Throws an error if the name is reserved.
  void validateNotReserved(const std::string& name, const std::string& kind,
                           std::optional<Position> location);

  // Analyze a method body with type bindings.
  // Runs semantic analysis with 'this' bound to the given class type.
  // @param methodFunc The method to analyze (must have a body)
  // @param classType The class type for 'this' parameter binding
  // @param typeParams Type parameter names to bind
  // @param typeArgs Type argument values for the type parameters
  void analyzeMethodWithBindings(FunctionAST& methodFunc,
                                 std::shared_ptr<sun::ClassType> classType,
                                 const std::vector<std::string>& typeParams,
                                 const std::vector<sun::TypePtr>& typeArgs);

  void analyzeBlock(BlockExprAST& block);

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
  sun::TypePtr inferType(const ExprAST& expr);
  sun::TypePtr inferType(const MemberAccessAST& expr);



  // Infer type for generic call using pre-resolved type arguments
  sun::TypePtr inferGenericCallType(const GenericCallAST& call);

  // static_ptr<T> builtin methods (call form only): length(), raw().
  // asNonClassStaticPtr returns the type when it is a static_ptr to a
  // non-class (a static_ptr<Class> dispatches to the class's own methods).
  static sun::StaticPointerType* asNonClassStaticPtr(const sun::TypePtr& type);
  static bool isStaticPtrMethod(const std::string& name);
  sun::TypePtr inferStaticPtrMethodType(const sun::StaticPointerType& ptrType,
                                        const std::string& name,
                                        size_t argCount,
                                        const Position& loc);

  // Generic call type inference helpers
  sun::TypePtr inferIntrinsicCallType(const GenericCallAST& call);
  sun::TypePtr inferGenericFunctionCallType(const GenericCallAST& call);
  sun::TypePtr inferGenericClassConstructionType(const GenericCallAST& call);

  void registerGlobal(const std::string& name, sun::TypePtr type);

  // Register a function prototype (key = name + param types for overloads)
  void registernFunctionInCurrentScope(const std::string& name,
                                       const FunctionInfo& info);

  // Lookup function by name and exact argument types (returns nullopt if no
  // match)
  std::optional<FunctionInfo> lookupFunction(
      const std::string& name, const std::vector<sun::TypePtr>& argTypes) const;

  // Get all function overloads with the given name
  std::vector<FunctionInfo> getAllFunctions(const std::string& name) const;

  // Generate function signature string: "name(type1,type2,...)"
  static std::string getFunctionSignature(
      const std::string& name, const std::vector<sun::TypePtr>& paramTypes);

  // Lookup variable info (public for codegen to access types)
  VariableInfo* lookupVariable(const std::string& name);

  // Class support
  void registerClass(const std::string& name,
                     std::shared_ptr<sun::ClassType> classType,
                     std::optional<Position> loc = std::nullopt);
  std::shared_ptr<sun::ClassType> lookupClass(const std::string& name) const;
  void setCurrentClass(std::shared_ptr<sun::ClassType> classType);
  std::shared_ptr<sun::ClassType> getCurrentClass() const;

  // A borrow binds the storage of an addressable lvalue: a variable, a field,
  // or an array element. Rejects everything else (temporaries, class
  // __index__ results, slices), so `ref r = x` and `var r: ref T = x` agree.
  static bool isBorrowableLvalue(const ExprAST& target);
  void validateBorrowTarget(const ExprAST& target, const Position& loc);

  // Packed class rules (see include/packed_layout.h for what "packed" means).
  // Each rejects one way a packed field's layout guarantee could be violated.
  void checkPackedFieldNotBorrowed(const ExprAST& target,
                                   const Position& loc) const;
  void checkPackedRefArguments(
      const std::vector<std::unique_ptr<ExprAST>>& args,
      const std::vector<sun::TypePtr>& paramTypes) const;
  void checkPackedFieldType(const ClassDefinitionAST& classDef,
                            const ClassFieldDecl& field,
                            const sun::TypePtr& fieldType) const;

  // Generic class support
  void registerGenericClass(const std::string& name,
                            const GenericClassInfo& info,
                            std::optional<Position> loc = std::nullopt);
  const GenericClassInfo* lookupGenericClass(const std::string& name) const;
  const GenericClassInfo* lookupGenericClass(
      const sun::QualifiedName& qualifiedName) const;
  // Generic definition a specialized class was instantiated from
  const GenericClassInfo* lookupGenericClassOf(
      const sun::ClassType& specialized) const;
  std::shared_ptr<sun::ClassType> instantiateGenericClass(
      const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs);
  std::shared_ptr<sun::ClassType> instantiateGenericClass(
      const GenericClassInfo& genericClassInfo,
      const std::vector<sun::TypePtr>& typeArgs);

  // Generic function support
  // Register a generic function template in current scope
  void registerGenericFunctionInCurrentScope(FunctionAST& func);
  // Lookup a generic function by name. Tries direct name first, then falls back
  // to enclosing function prefix + name (for nested generic functions).
  const GenericFunctionInfo* lookupGenericFunction(
      const std::string& name) const;
  std::optional<SpecializedFunctionInfo> instantiateGenericFunction(
      const GenericFunctionInfo& genericInfo,
      const std::vector<sun::TypePtr>& typeArgs);
  // Type-argument inference itself is sun::generics (generic_type_arguments.h).
  // The signature a generic function has under the given type arguments,
  // without instantiating it: what a call in a template body resolves to
  // until the enclosing generic is specialized.
  sun::TypePtr genericFunctionSignature(
      const GenericFunctionInfo& genericInfo,
      const std::vector<sun::TypePtr>& typeArgs);
  // Instantiate for a call site: same as instantiateGenericFunction, but a
  // failure is the call's error rather than an empty optional to unpack.
  SpecializedFunctionInfo requireGenericSpecialization(
      const GenericFunctionInfo& genericInfo,
      const std::vector<sun::TypePtr>& typeArgs, const std::string& displayName,
      std::optional<Position> loc);

  // Generic method support
  // Instantiates a generic method on a class with specific type arguments.
  // Stores the specialization on the generic method's FunctionAST.
  // Returns the specialized FunctionAST for codegen lookup.
  // variadicArgTypes carries the resolved types of the actual variadic
  // arguments at the call site (for methods with an _init_args<T> pack). When
  // the method is variadic, these drive the specialization's arity, its init
  // overload selection, and its mangled name. `std::nullopt` means "no call
  // info available" (e.g. from type inference): a variadic method is then not
  // specialized here and the call-site trigger, which supplies the types
  // (possibly an empty vector for a zero-arg call), does the real work.
  std::shared_ptr<FunctionAST> instantiateGenericMethod(
      std::shared_ptr<sun::ClassType> classType, const std::string& methodName,
      const std::vector<sun::TypePtr>& methodTypeArgs,
      const std::optional<std::vector<sun::TypePtr>>& variadicArgTypes =
          std::nullopt);

  // Find a generic method's FunctionAST on a class by name (nullptr if none).
  FunctionAST* findGenericMethodAST(const sun::ClassType* classType,
                                    const std::string& methodName);

  // Type parameter bindings (now scope-based)
  void addTypeParameterBindings(const std::vector<std::string>& params,
                                const std::vector<sun::TypePtr>& args);
  sun::TypePtr findTypeParameter(const std::string& name) const;

  // Type alias support (lexically scoped)
  sun::TypePtr findTypeAlias(const std::string& name) const;

  // Interface support
  void registerInterface(const std::string& name,
                         std::shared_ptr<sun::InterfaceType> interfaceType,
                         std::optional<Position> loc = std::nullopt);
  std::shared_ptr<sun::InterfaceType> lookupInterface(
      const std::string& name) const;

  // Generic interface support
  void registerGenericInterface(const std::string& name,
                                const GenericInterfaceInfo& info,
                                std::optional<Position> loc = std::nullopt);
  const GenericInterfaceInfo* lookupGenericInterface(
      const std::string& name) const;
  std::shared_ptr<sun::InterfaceType> instantiateGenericInterface(
      const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs);

  // Enum support
  void registerEnum(const std::string& name,
                    std::shared_ptr<sun::EnumType> enumType);
  std::shared_ptr<sun::EnumType> lookupEnum(const std::string& name) const;

  // Inherit fields from implemented interfaces (must be called before analyzing
  // methods)
  void inheritInterfaceFields(const ClassDefinitionAST& classDef,
                              std::shared_ptr<sun::ClassType> classType);

  // Validate that a class implements all required methods of its interfaces
  void validateInterfaceImplementation(
      const ClassDefinitionAST& classDef,
      std::shared_ptr<sun::ClassType> classType);

  // Module/namespace support (module scopes are tracked via the scope stack)
  // enterModuleScope() and exitScope() are used to manage module scopes

  // Register namespaced symbols (used during namespace analysis)
  void registerModuleVariable(const std::string& baseName,
                              const std::string& qualifiedName,
                              sun::TypePtr type, sun::Visibility visibility);

  // Register a module-level variable imported from a .moon bundle. The stub
  // carries a type annotation and a content-hash-scoped qualified name, but
  // no initializer — the storage is in the bundle.
  void registerPrecompiledModuleVariable(VariableCreationAST& varCreate);

  // Lookup functions that handle using statements and namespace resolution
  VariableInfo* lookupQualifiedVariable(const std::string& qualifiedName);
  const FunctionInfo* lookupQualifiedFunction(
      const std::string& qualifiedName) const;

  // Resolve a name considering using statements
  sun::QualifiedName resolveNameWithUsings(const std::string& name) const;

  // Add a using import (legacy string-based)
  void addUsingImport(const UsingImport& import);
  // Add a scope-based import binding
  void addImportBinding(const ImportBinding& binding);

 private:
  // Check if an identifier starts with underscore (reserved for builtins)
  static bool isReservedIdentifier(const std::string& name);

  // Check if a function name is an intrinsic (starts with '_')
  static bool isIntrinsic(const std::string& name) {
    return !name.empty() && name[0] == '_';
  }

  // Check if a type can be assigned to another type.
  // Handles interface assignability: class C can be assigned to interface I
  // if C implements I. Also handles ref unwrapping and exact equality.
  static bool isAssignableTo(const sun::TypePtr& from, const sun::TypePtr& to);

  // Try to coerce an integer literal to a target primitive type.
  // Returns true if coercion happened. If throwOnFail, throws on mismatch.
  static bool tryCoerceIntegerLiteral(ExprAST* expr, sun::TypePtr targetType,
                                      bool throwOnFail = false);

  // Give an untyped numeric literal operand of a binary expression its type
  // from context: the surrounding expected type if there is one, otherwise the
  // other operand's type. Without this `u8_var + 32` would promote to the
  // literal's default i32.
  static void coerceBinaryLiteralOperands(const BinaryExprAST& binExpr,
                                          const sun::TypePtr& expectedType);

  // The type an arithmetic/bitwise/shift binary expression produces: the wider
  // of the two operand types, mirroring codegen's operand unification.
  static sun::TypePtr promoteBinaryOperands(const sun::TypePtr& lhsType,
                                            const sun::TypePtr& rhsType);

  // Unify the branch types of a ternary expression: exact match, or the
  // wider type when one side widens to the other (never narrows f64 to f32).
  // Throws a compile error when the types are incompatible.
  static sun::TypePtr unifyTernaryTypes(const sun::TypePtr& thenType,
                                        const sun::TypePtr& elseType,
                                        std::optional<Position> loc);

  // Extract type guard pattern from condition (_is<T>(var)).
  // Returns (varName, narrowedType) if matched.
  std::optional<std::pair<std::string, sun::TypePtr>> extractTypeGuard(
      const ExprAST& cond);

  // Validate parameter names and resolve their types from prototype
  // Throws if any parameter name is reserved; applies auto-ref conversion
  // Returns the resolved param types and sets them on the prototype
  //
  // allowByValueObjects exempts C externs from REQUIRE_REF_FOR_COMPOUND_PARAMS
  // when that policy is enabled: passing a struct by value is what the C ABI
  // specifies, so it is the callee's signature rather than a Sun choice.
  std::vector<sun::TypePtr> validateAndResolveParamTypes(
      PrototypeAST& proto, std::optional<Position> loc = std::nullopt,
      bool allowByValueObjects = false);

  // Register built-in functions (print, println, file I/O, etc.)
  void registerBuiltinFunctions();

  // Scope management - typed scopes
  void enterScope(ScopeType type = ScopeType::Block);
  // Enter a type parameter scope with bindings (combines enterScope +
  // addTypeParameterBindings)
  void enterTypeParamScope(const std::vector<std::string>& params,
                           const std::vector<sun::TypePtr>& args);
  void enterModuleScope(const std::string& moduleName);
  // Enter a class scope with qualified name for proper scope path
  void enterClassScope(const sun::QualifiedName& className);
  // Enter an interface scope with qualified name for proper scope path
  void enterInterfaceScope(const sun::QualifiedName& interfaceName);
  // Enter a function scope with the function's signature for nested function
  // qualified names. The signature should be "funcName(paramType1,paramType2)".
  // funcName is the qualified name of the function.
  void enterFunctionScope(const std::string& funcSig,
                          const sun::QualifiedName& funcName,
                          bool canThrow = false,
                          sun::TypePtr returnType = nullptr);

  // Return type of the nearest enclosing function scope (null outside
  // functions or when unresolved); used for return-position inference
  sun::TypePtr currentFunctionReturnType() const;
  void exitScope();

  // Get the current module prefix for name mangling (e.g., "sun_")
  // Returns empty string if not inside any module scope
  std::string getCurrentModulePrefix() const;

  // Get the current scope path as a vector of segments
  // e.g., inside "module A { module B { } }", returns {"A", "B"}
  std::vector<std::string> getCurrentScopePath() const;

  // Create a QualifiedName for a symbol in the current module scope
  // Preserves module path in display form for proper error messages
  sun::QualifiedName makeQualifiedName(const std::string& baseName) const;

  // Get the fully qualified name for a symbol in current scope
  // e.g., inside "module sun { }", qualifyName("Vec") returns "sun_Vec"
  std::string qualifyNameInCurrentModule(const std::string& name) const;

  // Check if we're currently inside a function declared with ", IError"
  // Traverses parent scopes to find the nearest function scope
  bool isInThrowingFunction() const;

  // Check if we're currently inside a try block
  bool isInTryBlock() const;

  // Enter/exit a try block (increments/decrements try depth counter)
  void enterTryBlock();
  void exitTryBlock();

  // Check if we're currently inside an unsafe block
  bool isInUnsafeBlock() const;

  // Enter/exit an unsafe block (increments/decrements unsafe depth counter)
  void enterUnsafeBlock();
  void exitUnsafeBlock();

  // Module name registration for qualified name resolution (mod_x.mod_y.var)

  bool isModuleName(const std::string& name) const;

  // Traverse childModules from global scope to find a module by dot-separated
  // path (e.g., "sun" or "sun.collections"). Returns nullptr if not found.
  SemanticScope* lookupModuleScope(const std::string& dotPath) const;

  // Get the full module path including library scope hashes
  // e.g., "b" -> "$hash$.b" if b is inside a library scope
  std::string getFullModulePath(const std::string& visiblePath) const;

  // -------------------------------------------------------------------
  // Unified symbol lookup - traverses library scopes transparently
  // Throws on ambiguity (same name in multiple library scopes)
  // -------------------------------------------------------------------

  // Find a symbol in a specific module path (dot-separated, user-visible)
  // e.g., findSymbolInModule("b", "get_version") finds b.get_version
  // even if b is inside a library scope like $hash$.b
  // Optional filterKind restricts to specific symbol type (None = any)
  // Optional argTypes selects the matching overload when the symbol is a
  // function; without it the first registered overload is returned.
  SymbolMatch findSymbolInModule(
      const std::string& modulePath, const std::string& name,
      SymbolKind filterKind = SymbolKind::None,
      const std::vector<sun::TypePtr>* argTypes = nullptr) const;

  // Calling into C leaves everything the borrow checker and type system
  // guarantee, so it is gated on `unsafe` — the same rule the equivalent
  // intrinsics (_malloc, _free, ...) already follow. Throws if `info` names a
  // C extern and the call site is not inside an unsafe block.
  void checkExternCallAllowed(const FunctionInfo& info,
                              const std::string& displayName,
                              const Position& loc) const;

  // Resolve a module-qualified call `mod.foo(args...)` against the actual
  // argument types and stamp the chosen overload's own mangled name onto the
  // member access. Rebuilding the name from the module path instead would
  // drop the overload param suffix and name a symbol codegen never emits.
  // Returns nullptr if the module has no overload matching those arguments.
  const FunctionInfo* resolveModuleQualifiedCall(
      const MemberAccessAST& memberAccess, const sun::TypePtr& objectType,
      const std::vector<sun::TypePtr>& argTypes) const;

  // Get all active using imports (from all enclosing scopes)
  std::vector<UsingImport> getActiveUsingImports() const;

  // Variable management
  void declareVariable(const std::string& name, sun::TypePtr type,
                       bool isParam = false);

  // Type narrowing (from _is<T> type guards in conditionals)
  void narrowVariable(const std::string& varName, sun::TypePtr narrowedType);
  sun::TypePtr getNarrowedType(const std::string& varName,
                               sun::TypePtr originalType) const;

  // Type inference helpers
  sun::TypePtr typeAnnotationToType(const TypeAnnotation& annot);
  std::vector<sun::TypePtr> resolveTypeArguments(
      const std::vector<std::unique_ptr<TypeAnnotation>>& typeAnnotations,
      const std::optional<Position>& location, const std::string& context);
  sun::TypePtr substituteTypeParameters(
      sun::TypePtr type);  // Substitute type params with bindings

  // Free variable collection (variables that need capturing)
  std::set<std::string> collectFreeVariables(
      const ExprAST& expr, const std::set<std::string>& bound);
  std::set<std::string> collectFreeVariablesInBlock(
      const BlockExprAST& block, std::set<std::string> bound);

  // Build captures for a function or lambda
  std::vector<Capture> buildCaptures(const FunctionAST& func);
  std::vector<Capture> buildCaptures(const LambdaAST& lambda);

  // Clear resolved types on an AST tree (for re-analysis of generic methods)
  void clearResolvedTypes(ExprAST& expr);

  // Analyze expressions (populates captures, validates types)
  // expectedType: optional type hint for type inference (e.g., from variable
  // declaration)
  void analyzeExpr(ExprAST& expr, sun::TypePtr expectedType = nullptr);

  // Resolve a `{ field: value }` literal against the type the context
  // expects. A struct literal has no type of its own, so without an expected
  // class type there is nothing to check the field names against.
  void analyzeStructLiteral(StructLiteralAST& literal,
                            const sun::TypePtr& expectedType);

  // If the member access names a class method in value position, resolve it
  // as a bound method reference: pick the overload (using expectedType when
  // the name is overloaded), set a LambdaType resolved type and the
  // isBoundMethodRef flag. No-op for fields, non-class receivers, and
  // call-position callees (those never route through here).
  void maybeResolveBoundMethodRef(MemberAccessAST& memberAccess,
                                  sun::TypePtr expectedType);
  void analyzeCall(CallExprAST& callExpr, sun::TypePtr expectedType = nullptr);

  // ===== Enums (all implemented in semantic_analysis/enums.cpp) =====

  // Enum definition analysis: validation, payload resolution, registration
  // (generic enums register as templates)
  void analyzeEnumDefinition(EnumDefinitionAST& enumDef);

  // Declaration-collection pre-pass: register a block's enums (and generic
  // enum templates) so function signatures collected afterwards can resolve
  // enum-typed parameters/returns
  void collectEnumDeclarations(const BlockExprAST& block);

  // Call interception for EnumName.Variant(args...) on concrete and generic
  // enums; returns true when the call was an enum construction
  bool tryAnalyzeEnumConstruction(CallExprAST& callExpr,
                                  sun::TypePtr expectedType);

  // Member-access interception for generic enum unit variants (Option.None);
  // returns true when handled (type arguments taken from the expected type)
  bool tryAnalyzeGenericEnumUnitVariant(MemberAccessAST& memberAccess,
                                        sun::TypePtr expectedType);

  // Enum variant construction: EnumName.Variant(args...)
  void analyzeEnumVariantConstruction(CallExprAST& callExpr,
                                      MemberAccessAST& memberAccess,
                                      const std::shared_ptr<sun::EnumType>& enumType);

  // Generic enum templates
  void registerGenericEnum(const std::string& name, GenericEnumInfo info);
  const GenericEnumInfo* lookupGenericEnum(const std::string& name) const;

  // Instantiate Option<i32> from a generic enum template (monomorphization)
  std::shared_ptr<sun::EnumType> instantiateGenericEnum(
      const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs);

  // Option.Some(42): infer type arguments from payload args (falling back to
  // the expected type), instantiate, then check like a concrete construction
  void analyzeGenericEnumConstruction(CallExprAST& callExpr,
                                      MemberAccessAST& memberAccess,
                                      const std::string& genericName,
                                      const GenericEnumInfo& genericInfo,
                                      sun::TypePtr expectedType);

  // Match analysis on enum discriminants: variant patterns, payload bindings,
  // exhaustiveness
  void analyzeEnumMatch(MatchExprAST& matchExpr,
                        const std::shared_ptr<sun::EnumType>& enumType,
                        sun::TypePtr expectedType);

  // Validate a resolved payload type for an enum variant (Stage 1 rules)
  void validateEnumPayloadType(const sun::TypePtr& type,
                               const std::shared_ptr<sun::EnumType>& enumType,
                               const std::string& variantName,
                               const Position& location);

  // Generic call analysis helpers
  void analyzeIntrinsicCall(GenericCallAST& genericCall);
  void analyzeGenericFunctionCall(GenericCallAST& genericCall);
  void analyzeGenericClassConstruction(GenericCallAST& genericCall);

  // Expand a variadic pack (`args...`) in a call's argument list into concrete,
  // already-typed VariableReferenceAST nodes ("args.0", "args.1", ...), using
  // the enclosing function scope's recorded variadic param. No-op when there is
  // no enclosing variadic param or no pack argument is present.
  void expandPackArguments(std::vector<std::unique_ptr<ExprAST>>& args);

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
  std::vector<const Position*> locationStack_;

 public:
  // Make `target` the current scope for the enclosed block (restored on
  // exit, including by exception). Generic instantiation uses it to analyze
  // a template's body in the scope the template was declared in.
  struct ScopeSwitchGuard {
    SemanticAnalyzer& sema;
    SemanticScope* saved;
    ScopeSwitchGuard(SemanticAnalyzer& s, SemanticScope* target)
        : sema(s), saved(s.currentScope) {
      if (target) sema.currentScope = target;
    }
    ~ScopeSwitchGuard() { sema.currentScope = saved; }
    ScopeSwitchGuard(const ScopeSwitchGuard&) = delete;
    ScopeSwitchGuard& operator=(const ScopeSwitchGuard&) = delete;
  };
  // The scope a generic template was declared in (nullptr if unknown, in
  // which case ScopeSwitchGuard keeps the current scope).
  template <typename GenericInfo>
  static SemanticScope* definitionScopeOf(const GenericInfo& info) {
    return info.definitionScope.lock().get();
  }
  SemanticScope* classDefinitionScope(const sun::ClassType& classType) const;

  struct LocationGuard {
    SemanticAnalyzer& sema;
    LocationGuard(SemanticAnalyzer& s, const Position& loc) : sema(s) {
      sema.locationStack_.push_back(&loc);
    }
    ~LocationGuard() { sema.locationStack_.pop_back(); }
    LocationGuard(const LocationGuard&) = delete;
    LocationGuard& operator=(const LocationGuard&) = delete;
  };
  std::optional<Position> currentLocation() const {
    if (locationStack_.empty()) return std::nullopt;
    return *locationStack_.back();
  }

  // AccessContext
  sun::ModulePath currentModulePath() const override;
  [[noreturn]] void denyAccess(
      const sun::access::ItemRef& item) const override;
  // `deinit` is compiler-invoked and therefore always public.
  static sun::Visibility methodVisibility(const FunctionAST& method);

  void requireAccessible(const sun::access::ItemRef& item,
                         const Position& loc) const {
    sun::access::requireAccessible(currentModulePath(), item, loc);
  }
  void requireAccessible(const sun::access::ItemRef& item) const {
    if (!isAccessible(item)) denyAccess(item);
  }
  bool isAccessible(const sun::access::ItemRef& item) const {
    return sun::access::isAccessible(currentModulePath(), item);
  }

  // Member resolution with access checks: nullptr when the member does not
  // exist; throws when it exists but is not accessible from here.
  const sun::ClassField* accessibleField(const sun::ClassType& cls,
                                         const std::string& name,
                                         const Position& loc) const;
  const sun::ClassMethod* accessibleMethod(const sun::ClassType& cls,
                                           const std::string& name,
                                           const Position& loc) const;
  const sun::ClassMethod* accessibleMethodForArgs(
      const sun::ClassType& cls, const std::string& name,
      const std::vector<sun::TypePtr>& argTypes, const Position& loc) const;
  // Throws "No matching overload" when the class has methods called `name`
  // but none of them takes `argTypes.size()` arguments. Silent otherwise, so
  // callers can still fall back on their own type-mismatch diagnostics.
  void reportNoMethodForArgCount(const sun::ClassType& cls,
                                 const std::string& name,
                                 const std::vector<sun::TypePtr>& argTypes,
                                 const Position& loc) const;
  const sun::InterfaceField* accessibleField(const sun::InterfaceType& iface,
                                             const std::string& name,
                                             const Position& loc) const;
  const sun::InterfaceMethod* accessibleMethod(const sun::InterfaceType& iface,
                                               const std::string& name,
                                               const Position& loc) const;

  // A module named by user code (`a.b`, `using a.b;`, `b.f()`): every
  // module on its path must be visible from here.
  void requireModuleAccessible(const SemanticScopeBase& moduleScope,
                               const Position& loc) const;

  // ItemRef builders (uniform diagnostics)
  static sun::access::ItemRef fieldRef(const sun::ClassType& cls,
                                       const sun::ClassField& f);
  static sun::access::ItemRef methodRef(const sun::ClassType& cls,
                                        const sun::ClassMethod& m);
  static sun::access::ItemRef fieldRef(const sun::InterfaceType& iface,
                                       const sun::InterfaceField& f);
  static sun::access::ItemRef methodRef(const sun::InterfaceType& iface,
                                        const sun::InterfaceMethod& m);
  static sun::access::ItemRef moduleRef(const ModuleScope& scope) {
    return accessItem(scope);
  }
};
