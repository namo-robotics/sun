// semantic_scope.h — Scope structures and symbol lookup types

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "qualified_name.h"
#include "types.h"

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

// -------------------------------------------------------------------
// Unified Symbol Lookup System
// Library scopes ($hash$) are transparent - symbols are found through them
// Ambiguity (same name in multiple libraries) causes a compilation error
// -------------------------------------------------------------------

// Kind of symbol found during lookup
enum class SymbolKind {
  None,
  Module,
  Class,
  GenericClass,
  Interface,
  GenericInterface,
  Enum,
  Function,
  Variable
};

// Forward declarations for SymbolMatch
struct GenericClassInfo;
struct GenericInterfaceInfo;
struct GenericFunctionInfo;

// Result of a symbol lookup - contains the symbol and where it was found
struct SymbolMatch {
  SymbolKind kind = SymbolKind::None;
  std::string name;        // The symbol name as registered (may be mangled)
  std::string modulePath;  // Full dot-separated path including library hashes
  std::string
      libraryHash;  // The library scope hash (empty if not from library)

  // Type information (one of these will be set based on kind)
  std::shared_ptr<sun::ClassType> classType;
  std::shared_ptr<sun::InterfaceType> interfaceType;
  std::shared_ptr<sun::EnumType> enumType;
  const GenericClassInfo* genericClassInfo = nullptr;
  const GenericInterfaceInfo* genericInterfaceInfo = nullptr;
  const FunctionInfo* functionInfo = nullptr;
  const VariableInfo* variableInfo = nullptr;

  bool empty() const { return kind == SymbolKind::None; }
  explicit operator bool() const { return kind != SymbolKind::None; }

  // Get mangled name for codegen (modulePath with dots→underscores + "_" +
  // name)
  std::string mangled() const {
    if (modulePath.empty()) return name;
    std::string result = modulePath;
    for (char& c : result) {
      if (c == '.') c = '_';
    }
    return result + "_" + name;
  }

  // Get display name for error messages (hides library hashes)
  std::string display() const {
    if (modulePath.empty()) return name;
    // Filter out $hash$ segments from module path
    std::string displayPath;
    std::istringstream stream(modulePath);
    std::string segment;
    while (std::getline(stream, segment, '.')) {
      if (segment.size() >= 2 && segment.front() == '$' &&
          segment.back() == '$') {
        continue;  // Skip library hash
      }
      if (!displayPath.empty()) displayPath += ".";
      displayPath += segment;
    }
    if (displayPath.empty()) return name;
    return displayPath + "." + name;
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
  std::string moduleName;  // For module scopes: the local name (e.g., "sun")
  std::string modulePath;  // Full dot-separated path (e.g., "sun.matrix")
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
  // Find functions matching a name prefix in this scope or child module scopes
  void collectFunctions(const std::string& prefix,
                        std::vector<FunctionInfo>& results) const;
};

// Helper to check if a module name represents a library scope ($hash$)
inline bool isLibraryScope(const std::string& name) {
  return name.size() >= 2 && name.front() == '$' && name.back() == '$';
}

// Mangle a dot-separated module path to underscore-separated
// e.g., "$hash$.b" -> "$hash$_b"
inline std::string mangleModulePath(const std::string& dotPath) {
  std::string result = dotPath;
  for (char& c : result) {
    if (c == '.') c = '_';
  }
  return result;
}
