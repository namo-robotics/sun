// semantic_analyzer.h — Pre-codegen semantic analysis pass

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ast.h"
#include "qualified_name.h"
#include "types.h"

// Alias for use in this header and semantic analyzer implementations
using QualifiedName = sun::QualifiedName;

// Information about a variable in the symbol table
struct VariableInfo {
  sun::TypePtr type;
  int scopeDepth;        // Which scope level declared it
  bool isFunctionParam;  // Is it a parameter vs let binding
  bool isMoved = false;  // Has ownership been transferred (move semantics)
};

// Information about a declared function
struct FunctionInfo {
  sun::TypePtr returnType;
  std::vector<sun::TypePtr> paramTypes;
  std::vector<Capture> captures;
};

// Type of scope in the scope stack
enum class ScopeType {
  Global,    // Top-level (program) scope
  Module,    // Module scope (has optional name for qualified names)
  Class,     // Class definition scope
  Function,  // Function or lambda body scope
  Block      // Block scope (if, while, for, etc.)
};

// Alias import from a using statement (legacy — being replaced by
// ImportBinding)
struct UsingImport {
  std::string namespacePath;  // "sun" or "sun.nested"
  std::string target;         // "Vec", "*" for wildcard, or "Mat*" for prefix
  bool isWildcard;            // true if target == "*"
  bool isPrefixWildcard;      // true if target ends with '*' (e.g., "Mat*")
  std::string prefix;         // For prefix wildcards, the prefix without '*'

  UsingImport(std::string nsPath, std::string t)
      : namespacePath(std::move(nsPath)),
        target(std::move(t)),
        isWildcard(target == "*"),
        isPrefixWildcard(false) {
    if (!isWildcard && target.size() > 1 && target.back() == '*') {
      isPrefixWildcard = true;
      prefix = target.substr(0, target.size() - 1);
    }
  }
};

// Forward declaration for scope pointer in ImportBinding
struct SemanticScope;

// Scope-based import binding — a reference to a symbol in another scope
struct ImportBinding {
  std::string localName;       // How the symbol is referred to locally ("Vec")
  SemanticScope* sourceScope;  // Pointer to the scope it was imported from
  std::string sourceName;      // Name in the source scope ("Vec")
  bool isWildcard;  // true for "using sun.*" (localName/sourceName unused)

  ImportBinding() : sourceScope(nullptr), isWildcard(false) {}
  ImportBinding(std::string local, SemanticScope* src, std::string srcName)
      : localName(std::move(local)),
        sourceScope(src),
        sourceName(std::move(srcName)),
        isWildcard(false) {}
  static ImportBinding wildcard(SemanticScope* src) {
    ImportBinding b;
    b.sourceScope = src;
    b.isWildcard = true;
    return b;
  }
};

// Information about a generic class definition (template)
struct GenericClassInfo {
  const ClassDefinitionAST* AST;            // Original AST node
  std::vector<std::string> typeParameters;  // ["T", "U", etc.]
};

// Information about a generic interface definition (template)
struct GenericInterfaceInfo {
  const InterfaceDefinitionAST* AST;        // Original AST node
  std::vector<std::string> typeParameters;  // ["T", "U", etc.]
};

// Information about a generic function definition (template)
struct GenericFunctionInfo {
  const FunctionAST* AST;                    // Original AST node
  std::vector<std::string> typeParameters;   // ["T", "U", etc.]
  std::optional<TypeAnnotation> returnType;  // Return type annotation
  std::vector<std::pair<std::string, TypeAnnotation>> params;  // Parameters
};

// Information about a specialized (monomorphized) generic function
struct SpecializedFunctionInfo {
  sun::TypePtr returnType;
  std::vector<sun::TypePtr> paramTypes;
  std::vector<Capture> captures;  // Captures with substituted types
  std::shared_ptr<FunctionAST> specializedAST;  // The analyzed clone
};

// Scope object containing variables, type parameter bindings, and type aliases
// All are lexically scoped just like variables
struct SemanticScope {
  ScopeType type = ScopeType::Global;
  std::string moduleName;  // For module scopes: the module name (e.g., "sun")
  // For function scopes: the function signature (e.g., "outer(i32)")
  // Used to create unique qualified names for nested functions in generic
  // instantiations
  std::string functionSignature;

  // ===== Symbol tables (persistent — serialized to .moon for module scopes)
  // =====

  // Functions declared in this scope (key = signature string
  // "name(type1,type2)")
  std::map<std::string, FunctionInfo> functions;
  // Classes declared in this scope
  std::map<std::string, std::shared_ptr<sun::ClassType>> classes;
  // Generic class templates declared in this scope
  std::map<std::string, GenericClassInfo> genericClasses;
  // Interfaces declared in this scope
  std::map<std::string, std::shared_ptr<sun::InterfaceType>> interfaces;
  // Generic interface templates declared in this scope
  std::map<std::string, GenericInterfaceInfo> genericInterfaces;
  // Enums declared in this scope
  std::map<std::string, std::shared_ptr<sun::EnumType>> enums;
  // Generic function templates declared in this scope
  std::map<sun::QualifiedName, GenericFunctionInfo> genericFunctions;
  // Child module scopes (for nested modules)
  std::map<std::string, std::shared_ptr<SemanticScope>> childModules;
  // Namespace-qualified variables: "sun_PI" -> VariableInfo
  std::map<std::string, VariableInfo> namespacedVariables;
  // Declared module names for qualified name resolution
  std::set<std::string> declaredModules;

  // ===== Transient state (not serialized) =====

  std::map<std::string, VariableInfo> variables;
  std::map<std::string, sun::TypePtr> typeParameters;
  std::map<std::string, sun::TypePtr> typeAliases;
  // Type narrowing from _is<T>(var) type guards in conditionals
  // Maps variable name to narrowed type (interface or concrete type)
  std::map<std::string, sun::TypePtr> narrowedTypes;
  // Using imports active in this scope (legacy string-based)
  std::vector<UsingImport> usingImports;
  // Scope-based import bindings (new)
  std::vector<ImportBinding> importBindings;
  // Parent scope pointer (not serialized, reconstructed on load)
  SemanticScope* parent = nullptr;
  // True if this scope was loaded from an external .moon file
  bool isExternal = false;

  SemanticScope() = default;
  SemanticScope(ScopeType t) : type(t) {}
  SemanticScope(ScopeType t, std::string name)
      : type(t), moduleName(std::move(name)) {}

  // Check if a symbol with the given name exists in this scope (any kind)
  // Recurses into child module scopes.
  bool hasSymbol(const std::string& name) const;

  // Find a class by name in this scope or child module scopes
  std::shared_ptr<sun::ClassType> findClass(const std::string& name) const;
  // Find a generic class by name in this scope or child module scopes
  const GenericClassInfo* findGenericClass(const std::string& name) const;
  // Find an interface by name in this scope or child module scopes
  std::shared_ptr<sun::InterfaceType> findInterface(
      const std::string& name) const;
  // Find a generic interface by name in this scope or child module scopes
  const GenericInterfaceInfo* findGenericInterface(
      const std::string& name) const;
  // Find an enum by name in this scope or child module scopes
  std::shared_ptr<sun::EnumType> findEnum(const std::string& name) const;
  // Find a function by signature in this scope or child module scopes
  const FunctionInfo* findFunction(const std::string& sig) const;
  // Find functions matching a name prefix in this scope or child module scopes
  void collectFunctions(const std::string& prefix,
                        std::vector<FunctionInfo>& results) const;
};

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

  // Stack of scopes - each scope contains variables, type parameter bindings,
  // and scope type (Global, Module, Class, Function, Block)
  std::vector<SemanticScope> scopeStack = {SemanticScope(ScopeType::Global)};

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
                     std::shared_ptr<sun::ClassType> classType);
  std::shared_ptr<sun::ClassType> lookupClass(const std::string& name) const;
  void setCurrentClass(std::shared_ptr<sun::ClassType> classType);
  std::shared_ptr<sun::ClassType> getCurrentClass() const;

  // Generic class support
  void registerGenericClass(const std::string& name,
                            const GenericClassInfo& info);
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
                         std::shared_ptr<sun::InterfaceType> interfaceType);
  std::shared_ptr<sun::InterfaceType> lookupInterface(
      const std::string& name) const;

  // Generic interface support
  void registerGenericInterface(const std::string& name,
                                const GenericInterfaceInfo& info);
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
  std::vector<sun::TypePtr> validateAndResolveParamTypes(PrototypeAST& proto);

  // Register built-in functions (print, println, file I/O, etc.)
  void registerBuiltinFunctions();

  // Scope management - typed scopes
  void enterScope(ScopeType type = ScopeType::Block);
  void enterModuleScope(const std::string& moduleName);
  // Enter a function scope with the function's signature for nested function
  // qualified names. The signature should be "funcName(paramType1,paramType2)".
  void enterFunctionScope(const std::string& funcSig);
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

  // Check if currently inside a module scope
  bool isInModuleScope() const;

  // Check if currently inside a function scope (for detecting nested functions)
  bool isInFunctionScope() const;

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

  // Get all active using imports (from all enclosing scopes)
  std::vector<UsingImport> getActiveUsingImports() const;

  // Variable management
  void declareVariable(const std::string& name, sun::TypePtr type,
                       bool isParam = false);
  bool isVariableLocal(const std::string& name) const;

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
