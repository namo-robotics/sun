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

  // True when not inside any function scope (i.e. at module/global level)
  bool isAtModuleLevel() const {
    for (auto* s = currentScope; s != nullptr; s = s->parent)
      if (s->getType() == ScopeType::Function) return false;
    return true;
  }

 public:
  explicit SemanticAnalyzer(std::shared_ptr<sun::TypeRegistry> registry)
      : typeRegistry(std::move(registry)) {
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

  // Generic class support
  void registerGenericClass(const std::string& name,
                            const GenericClassInfo& info,
                            std::optional<Position> loc = std::nullopt);
  const GenericClassInfo* lookupGenericClass(const std::string& name) const;
  std::shared_ptr<sun::ClassType> instantiateGenericClass(
      const std::string& baseName, const std::vector<sun::TypePtr>& typeArgs);

  // Generic function support
  // Register a generic function template in current scope
  void registerGenericFunctionInCurrentScope(FunctionAST& func);
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
  void registerModuleVariable(const std::string& baseName,
                              const std::string& qualifiedName,
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

  // Helper template for generic class/interface lookup
  // Finder takes (SemanticScope*, const std::string&) and returns ResultPtr
  template <typename ResultPtr, typename Finder>
  ResultPtr lookupGenericSymbol(const std::string& name, Finder finder) const;

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

  // Extract type guard pattern from condition (_is<T>(var)).
  // Returns (varName, narrowedType) if matched.
  std::optional<std::pair<std::string, sun::TypePtr>> extractTypeGuard(
      const ExprAST& cond);

  // Validate parameter names and resolve their types from prototype
  // Throws if any parameter name is reserved; applies auto-ref conversion
  // Returns the resolved param types and sets them on the prototype
  std::vector<sun::TypePtr> validateAndResolveParamTypes(
      PrototypeAST& proto, std::optional<Position> loc = std::nullopt);

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
                          bool canThrow = false);
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
  void analyzeExpr(ExprAST& expr);
  void analyzeCall(CallExprAST& callExpr);

  // Generic call analysis helpers
  void analyzeIntrinsicCall(GenericCallAST& genericCall);
  void analyzeGenericFunctionCall(GenericCallAST& genericCall);
  void analyzeGenericClassConstruction(GenericCallAST& genericCall);
};

// -------------------------------------------------------------------
// Template implementation: lookupGenericSymbol
// Finder signature: ResultPtr finder(SemanticScope*, const std::string&)
// -------------------------------------------------------------------

template <typename ResultPtr, typename Finder>
ResultPtr SemanticAnalyzer::lookupGenericSymbol(const std::string& name,
                                                Finder finder) const {
  // Handle module-qualified names like "Test.Inner"
  size_t dotPos = name.find('.');
  if (dotPos != std::string::npos) {
    std::string moduleName = name.substr(0, dotPos);
    std::string symbolName = name.substr(dotPos + 1);

    // Search for the module scope and look up the symbol there
    for (auto* s = currentScope; s != nullptr; s = s->parent) {
      // Check child modules of current scope
      auto modIt = s->childModules.find(moduleName);
      if (modIt != s->childModules.end() && modIt->second) {
        auto result = finder(modIt->second.get(), symbolName);
        if (result) return result;
      }
      // Also check inside import scopes for module definitions
      for (const auto& [childName, child] : s->childModules) {
        if (child && child->getType() == ScopeType::Import) {
          auto innerModIt = child->childModules.find(moduleName);
          if (innerModIt != child->childModules.end() && innerModIt->second) {
            auto result = finder(innerModIt->second.get(), symbolName);
            if (result) return result;
          }
        }
      }
    }
    // Fall through to regular lookup if module not found
  }

  // Walk scope chain from innermost to outermost
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    auto result = finder(s, name);
    if (result) return result;
    // Search direct import-scope children (one level of transparency)
    for (const auto& [childName, child] : s->childModules) {
      if (child && child->getType() == ScopeType::Import) {
        result = finder(child.get(), name);
        if (result) return result;
        for (const auto& [modName, modChild] : child->childModules) {
          if (modChild && modChild->getType() == ScopeType::Module) {
            result = finder(modChild.get(), name);
            if (result) return result;
          }
        }
      }
    }
    // Search the __definition__ scope (for transitive deps during generic
    // instantiation). Walk up the definition scope's parent chain to reach
    // imports at the file scope level.
    auto defIt = s->childModules.find("__definition__");
    if (defIt != s->childModules.end() && defIt->second) {
      for (auto* defS = defIt->second.get(); defS != nullptr;
           defS = defS->parent) {
        result = finder(defS, name);
        if (result) return result;
        // Search its import-scope children
        for (const auto& [childName, child] : defS->childModules) {
          if (child && child->getType() == ScopeType::Import) {
            result = finder(child.get(), name);
            if (result) return result;
            for (const auto& [modName, modChild] : child->childModules) {
              if (modChild && modChild->getType() == ScopeType::Module) {
                result = finder(modChild.get(), name);
                if (result) return result;
              }
            }
          }
        }
      }
    }
    // Search import bindings from using statements
    for (const auto& binding : s->importBindings) {
      if (!binding.sourceScope) continue;
      if (binding.isWildcard) {
        auto result2 = finder(binding.sourceScope, name);
        if (result2) return result2;
      } else if (binding.localName == name) {
        auto result2 = finder(binding.sourceScope, binding.sourceName);
        if (result2) return result2;
      }
    }
  }
  return ResultPtr{};
}
