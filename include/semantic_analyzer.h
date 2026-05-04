// semantic_analyzer.h — Pre-codegen semantic analysis pass

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "semantic_scope.h"

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
  // Type registry for class/interface types (shared with codegen)
  std::shared_ptr<sun::TypeRegistry> typeRegistry;

  // Scope tree — rootScope is the global scope, currentScope walks the tree
  std::shared_ptr<SemanticScope> rootScope =
      std::make_shared<SemanticScope>(ScopeType::Global);
  SemanticScope* currentScope = rootScope.get();

  // Track classes currently being instantiated (to detect/break mutual
  // recursion)
  std::set<std::string> classesBeingInstantiated;

  // Cache of specialized (monomorphized) functions: mangledName →
  // SpecializedFunctionInfo
  std::map<std::string, SpecializedFunctionInfo> specializedFunctionCache;

  // Current class being analyzed (for 'this' resolution)
  std::shared_ptr<sun::ClassType> currentClass = nullptr;

  // True during Pass 1 (collectDeclarations). When true, registration
  // functions throw on redeclaration. When false (Pass 2), they skip
  // silently since Pass 1 already registered the symbol.
  bool collectingDeclarations = false;

  // Track nesting inside ImportScopeAST during analysis.
  // When > 0, duplicate symbol registration is allowed (diamond imports).
  int importScopeDepth_ = 0;

  // Classes already fully analyzed in Pass 2 (used to skip diamond duplicates)
  std::unordered_set<std::string> analyzedClasses_;

  // Functions already fully analyzed in Pass 2 (used to skip diamond
  // duplicates)
  std::unordered_set<std::string> analyzedFunctions_;

  // Global variables already analyzed in Pass 2 (used to skip diamond
  // duplicates)
  std::unordered_set<std::string> analyzedGlobals_;

  // True when not inside any function scope (i.e. at module/global level)
  bool isAtModuleLevel() const {
    for (auto* s = currentScope; s != nullptr; s = s->parent)
      if (s->type == ScopeType::Function) return false;
    return true;
  }

  // RAII guard to set collectingDeclarations and restore on scope exit
  struct CollectingGuard {
    bool& flag;
    bool prev;
    CollectingGuard(bool& f, bool val) : flag(f), prev(f) { flag = val; }
    ~CollectingGuard() { flag = prev; }
    CollectingGuard(const CollectingGuard&) = delete;
    CollectingGuard& operator=(const CollectingGuard&) = delete;
  };

 public:
  explicit SemanticAnalyzer(std::shared_ptr<sun::TypeRegistry> registry)
      : typeRegistry(std::move(registry)) {
    registerBuiltinFunctions();
  }

  // Main entry point: analyze a top-level expression/statement
  void analyze(ExprAST& expr);

  // Pass 1: Pre-register declarations (functions, classes, interfaces, enums)
  // in the current scope without analyzing bodies. Enables forward references.
  void collectDeclarations(ExprAST& expr);

  // Extract function signature info (param types, captures, explicit return
  // type) Sets captures on the prototype and handles auto-ref conversion for
  // params. Does NOT register the function - caller is responsible for that.
  // Returns FunctionInfo with returnType set if explicit, nullptr if needs
  // inference.
  FunctionInfo getFunctionInfo(FunctionAST& func);
  FunctionInfo getLambdaInfo(LambdaAST& lambda);

  // Analyze a function body. Call getFunctionInfo first to get signature info.
  // If return type was not explicit, this infers it and updates the prototype.
  // Does NOT register the function - caller is responsible for that.
  void analyzeFunction(FunctionAST& func);
  void analyzeLambda(LambdaAST& lambda);

  // Validate that a type parameter exists when the type is a TypeParameterType.
  // Throws an error with source location if the type parameter is not found.
  void validateTypeParameter(const sun::TypePtr& type, const ExprAST& node);

  // Lazy parse and analyze a method body from source text.
  // This is a unified helper for precompiled generic class methods:
  // 1. Parses the body from sourceText if the body is empty
  // 2. Runs semantic analysis with 'this' bound to the given class type
  // 3. Updates the FunctionAST in place with the parsed/analyzed body
  // @param methodFunc The method to parse/analyze (must have sourceText set)
  // @param classType The class type for 'this' parameter binding
  // @param typeParams Type parameter names to bind
  // @param typeArgs Type argument values for the type parameters
  void lazyParseAndAnalyzeMethod(FunctionAST& methodFunc,
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

  void registerGlobal(const std::string& name, sun::TypePtr type);

  // Register a function prototype (key = name + param types for overloads)
  void registerFunction(const std::string& name, const FunctionInfo& info);

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

  // Generic class support
  void registerGenericClass(const std::string& name,
                            const GenericClassInfo& info,
                            std::optional<Position> loc = std::nullopt);
  const GenericClassInfo* lookupGenericClass(const std::string& name) const;
  std::shared_ptr<sun::ClassType> instantiateGenericClass(
      const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs);

  // Generic function support
  // Lookup a generic function by name. Tries direct name first, then falls back
  // to enclosing function prefix + name (for nested generic functions).
  const GenericFunctionInfo* lookupGenericFunction(
      const std::string& name) const;
  std::optional<SpecializedFunctionInfo> instantiateGenericFunction(
      const FunctionAST* genericFunc,
      const std::vector<sun::TypePtr>& typeArgs);

  // Generic method support
  // Instantiates a generic method on a class with specific type arguments.
  // Stores the specialization on the generic method's FunctionAST.
  // Returns the specialized FunctionAST for codegen lookup.
  std::shared_ptr<FunctionAST> instantiateGenericMethod(
      std::shared_ptr<sun::ClassType> classType, const std::string& methodName,
      const std::vector<sun::TypePtr>& methodTypeArgs);

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
  void registerNamespacedVariable(const std::string& qualifiedName,
                                  sun::TypePtr type);

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

  // Check if a type can be assigned to another type.
  // Handles interface assignability: class C can be assigned to interface I
  // if C implements I. Also handles ref unwrapping and exact equality.
  static bool isAssignableTo(const sun::TypePtr& from, const sun::TypePtr& to);

  // Validate parameter names and resolve their types from prototype
  // Throws if any parameter name is reserved; applies auto-ref conversion
  // Returns the resolved param types and sets them on the prototype
  std::vector<sun::TypePtr> validateAndResolveParamTypes(
      PrototypeAST& proto, std::optional<Position> loc = std::nullopt);

  // Register built-in functions (print, println, file I/O, etc.)
  void registerBuiltinFunctions();

  // Scope management - typed scopes
  void enterScope(ScopeType type = ScopeType::Block);
  void enterModuleScope(const std::string& moduleName);
  // Enter a function scope with the function's signature for nested function
  // qualified names. The signature should be "funcName(paramType1,paramType2)".
  void enterFunctionScope(const std::string& funcSig);
  // Enter an import scope for an imported .sun file.
  // Uses a hash of the source file path for deduplication.
  void enterImportScope(const std::string& sourceFile);
  void exitScope();

  // Get the current module prefix for name mangling (e.g., "sun_")
  // Returns empty string if not inside any module scope
  std::string getCurrentModulePrefix() const;

  // Get the current module path in display form (dot-separated)
  // e.g., inside "module A { module B { } }", returns "A.B"
  std::string getCurrentModulePath() const;

  // Create a QualifiedName for a symbol in the current module scope
  // Preserves module path in display form for proper error messages
  sun::QualifiedName makeQualifiedName(const std::string& baseName) const;

  // Get the fully qualified name for a symbol in current scope
  // e.g., inside "module sun { }", qualifyName("Vec") returns "sun_Vec"
  std::string qualifyNameInCurrentModule(const std::string& name) const;

  // Get the function context for nested function names from enclosing scopes.
  // Returns empty string if not inside any function scope.
  // Example: inside "outer(i32)" and "middle(f64)" -> "outer(i32)::middle(f64)"
  std::string getCurrentFunctionContext() const;

  // Module name registration for qualified name resolution (mod_x.mod_y.var)
  void registerModule(const std::string& modulePath);

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
  SymbolMatch findSymbolInModule(
      const std::string& modulePath, const std::string& name,
      SymbolKind filterKind = SymbolKind::None) const;

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
  void analyzeExpr(ExprAST& expr);
  void analyzeCall(CallExprAST& callExpr);
};
