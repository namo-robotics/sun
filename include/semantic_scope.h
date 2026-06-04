// semantic_scope.h — Scope structures and symbol lookup types

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "qualified_name.h"
#include "types.h"

// Information about a variable in the symbol table
struct VariableInfo {
  sun::TypePtr type;
  bool isGlobal;         // Declared at module level (not inside a function)
  bool isFunctionParam;  // Is it a parameter vs let binding
  bool isMoved = false;  // Has ownership been transferred (move semantics)
  std::string qualifiedName;  // Mangled name for codegen (e.g., "sun_PI").
                              // Empty for locals.
  std::string baseName;       // User-written name (e.g., "PI"). For debugging.
};

// Information about a declared function
struct FunctionInfo {
  sun::TypePtr returnType;
  std::vector<sun::TypePtr> paramTypes;
  std::vector<Capture> captures;
  sun::QualifiedName qualifiedNameInfo;  // Full qualified name object
  std::string qualifiedName;  // Mangled name for codegen (e.g., "sun_square").
                              // Empty for builtins.
  std::string baseName;   // User-written name (e.g., "square"). For debugging.
  bool canThrow = false;  // Whether this function can throw (declared with ,
                          // IError)
};

// Indexed function table: O(1) name-based overload lookup + O(1) exact sig
// lookup. Replaces std::map<string, FunctionInfo> which required O(n) prefix
// scans to find overloads by name.
class FunctionTable {
 public:
  using iterator = std::unordered_map<std::string, FunctionInfo>::iterator;
  using const_iterator =
      std::unordered_map<std::string, FunctionInfo>::const_iterator;

  FunctionTable() = default;

  // Copy constructor - rebuild byName_ with valid pointers
  FunctionTable(const FunctionTable& other) : bySig_(other.bySig_) {
    rebuildByName();
  }

  // Copy assignment - rebuild byName_ with valid pointers
  FunctionTable& operator=(const FunctionTable& other) {
    if (this != &other) {
      bySig_ = other.bySig_;
      rebuildByName();
    }
    return *this;
  }

  // Move operations can use defaults
  FunctionTable(FunctionTable&&) = default;
  FunctionTable& operator=(FunctionTable&&) = default;

  FunctionInfo& operator[](const std::string& sig) {
    auto [it, inserted] = bySig_.emplace(sig, FunctionInfo{});
    if (inserted) {
      std::string name = extractName(sig);
      byName_[name].push_back(&it->second);
    }
    return it->second;
  }

  bool contains(const std::string& sig) const { return bySig_.count(sig) > 0; }

  const_iterator find(const std::string& sig) const { return bySig_.find(sig); }

  const_iterator end() const { return bySig_.end(); }
  const_iterator begin() const { return bySig_.begin(); }
  iterator end() { return bySig_.end(); }
  iterator begin() { return bySig_.begin(); }
  bool empty() const { return bySig_.empty(); }
  size_t size() const { return bySig_.size(); }

  // Check if any function with this base name exists (O(1))
  bool hasName(const std::string& name) const {
    return byName_.count(name) > 0;
  }

  // Check if any function with this base name exists, also trying qualified
  bool hasNameOrQualified(const std::string& name,
                          const std::string& qualifiedName) const {
    if (byName_.count(name) > 0) return true;
    if (!qualifiedName.empty() && byName_.count(qualifiedName) > 0) return true;
    return false;
  }

  // Get all overloads for a given base name (O(1) lookup)
  const std::vector<FunctionInfo*>* getOverloads(
      const std::string& name) const {
    auto it = byName_.find(name);
    if (it != byName_.end()) return &it->second;
    return nullptr;
  }

 private:
  std::unordered_map<std::string, FunctionInfo> bySig_;
  std::unordered_map<std::string, std::vector<FunctionInfo*>> byName_;

  // Rebuild byName_ index from bySig_ (used after copy)
  void rebuildByName() {
    byName_.clear();
    for (auto& [sig, info] : bySig_) {
      std::string name = extractName(sig);
      byName_[name].push_back(&info);
    }
  }

  // Extract function name from signature "name(type1,type2)"
  static std::string extractName(const std::string& sig) {
    auto paren = sig.find('(');
    return paren != std::string::npos ? sig.substr(0, paren) : sig;
  }
};

// Type of scope in the scope tree
enum class ScopeType {
  Global,     // Top-level (program) scope
  Module,     // Module scope (has optional name for qualified names)
  Import,     // Import scope (wraps an imported .sun file's declarations)
  Class,      // Class definition scope
  Interface,  // Interface definition scope
  Function,   // Function or lambda body scope
  Block,      // Block scope (if, while, for, etc.)
  TypeParams  // Type parameter binding scope (for generic instantiation)
};

// Alias import from a using statement (legacy — being replaced by
// ImportBinding)
struct UsingImport {
  std::string namespacePath;  // "sun" or "sun.nested"
  std::string target;         // "Vec" for specific, "*" for wildcard
  bool isWildcard;            // true if target == "*" (using sun;)

  UsingImport(std::string nsPath, std::string t)
      : namespacePath(std::move(nsPath)),
        target(std::move(t)),
        isWildcard(target == "*") {}
};

// Forward declaration for scope pointer in ImportBinding
struct SemanticScopeBase;

// Scope-based import binding — a reference to a symbol in another scope
struct ImportBinding {
  std::string localName;  // How the symbol is referred to locally ("Vec")
  SemanticScopeBase* sourceScope;  // Pointer to the scope it was imported from
  std::string sourceName;          // Name in the source scope ("Vec")
  bool isWildcard;  // true for "using sun;" (localName/sourceName unused)

  ImportBinding() : sourceScope(nullptr), isWildcard(false) {}
  ImportBinding(std::string local, SemanticScopeBase* src, std::string srcName)
      : localName(std::move(local)),
        sourceScope(src),
        sourceName(std::move(srcName)),
        isWildcard(false) {}
  static ImportBinding wildcard(SemanticScopeBase* src) {
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
  const ClassDefinitionAST* AST;                     // Original AST node
  std::vector<std::string> typeParameters;           // ["T", "U", etc.]
  std::weak_ptr<SemanticScopeBase> definitionScope;  // Scope where generic was
                                                     // defined (weak to avoid
                                                     // circular refs)
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

// Forward declarations
struct SemanticScopeBase;
struct GlobalScope;
struct ModuleScope;
struct ImportScope;
struct FunctionScope;
struct ClassScope;
struct InterfaceScope;
struct BlockScope;
struct TypeParamsScope;

// Alias for backward compatibility
using SemanticScope = SemanticScopeBase;

// ===================================================================
// SemanticScopeBase - Base class for all scope types
// Contains all fields for backward compatibility during incremental refactor.
// Inherits enable_shared_from_this to allow weak_ptr references from
// GenericClassInfo without creating circular ownership.
// ===================================================================
struct SemanticScopeBase
    : public std::enable_shared_from_this<SemanticScopeBase> {
  virtual ~SemanticScopeBase() = default;

  // Get the scope type (virtual - each subclass returns its type)
  virtual ScopeType getType() const = 0;

  // ===== Identification (for persistent scopes) =====
  std::string scopeName;  // Display name (module name, source file, etc.)
  std::string scopeKey;   // Lookup key / qualified path for symbol isolation

  // ===== Symbol tables (used by persistent scopes - Global/Module/Import)
  // =====
  FunctionTable functions;
  std::map<std::string, std::shared_ptr<sun::ClassType>> classes;
  std::map<std::string, ClassDefinitionAST*> classDefinitions;
  std::map<std::string, GenericClassInfo> genericClasses;
  std::map<std::string, std::shared_ptr<sun::InterfaceType>> interfaces;
  std::map<std::string, GenericInterfaceInfo> genericInterfaces;
  std::map<std::string, std::shared_ptr<sun::EnumType>> enums;
  std::map<sun::QualifiedName, GenericFunctionInfo> genericFunctions;
  std::map<std::string, std::shared_ptr<SemanticScopeBase>> childModules;
  std::map<std::string, VariableInfo> namespacedVariables;

  // ===== Transient state (all scopes) =====
  std::map<std::string, VariableInfo> variables;
  std::map<std::string, sun::TypePtr> typeParameters;
  std::map<std::string, sun::TypePtr> typeAliases;
  std::map<std::string, sun::TypePtr> narrowedTypes;
  std::vector<UsingImport> usingImports;
  std::vector<ImportBinding> importBindings;

  // Parent scope pointer (tree structure)
  SemanticScopeBase* parent = nullptr;
  // Non-module child scopes (for scope tree traversal)
  std::vector<std::shared_ptr<SemanticScopeBase>> children;

  // True if this scope was loaded from an external .moon file
  bool isExternal = false;

  // ===== Function scope fields =====
  std::string functionSignature;  // e.g., "outer(i32)"
  sun::QualifiedName functionName;
  bool functionCanThrow = false;
  int tryBlockDepth = 0;
  int unsafeBlockDepth = 0;
  bool inUnsafeContext = false;

  // ===== Class/Interface scope fields =====
  std::string classBaseName;     // Display name (e.g., "Vec")
  std::string classMangledName;  // Full mangled name (e.g., "sun_Vec_i32")

  // ===== Symbol lookup methods (delegate to persistent scope impl) =====
  bool hasSymbol(const std::string& name) const;
  std::shared_ptr<sun::ClassType> findClass(const std::string& name) const;
  const GenericClassInfo* findGenericClass(const std::string& name) const;
  std::shared_ptr<sun::InterfaceType> findInterface(
      const std::string& name) const;
  const GenericInterfaceInfo* findGenericInterface(
      const std::string& name) const;
  std::shared_ptr<sun::EnumType> findEnum(const std::string& name) const;
  void collectFunctions(const std::string& prefix,
                        std::vector<FunctionInfo>& results) const;

  // Clone symbol tables (for diamond import handling)
  std::shared_ptr<SemanticScopeBase> cloneSymbols(
      SemanticScopeBase* newParent) const;

  // ===== Downcasting helpers =====
  FunctionScope* asFunction();
  const FunctionScope* asFunction() const;
  ClassScope* asClass();
  const ClassScope* asClass() const;
  InterfaceScope* asInterface();
  const InterfaceScope* asInterface() const;
  BlockScope* asBlock();
  const BlockScope* asBlock() const;

  // Check if this is a persistent scope type
  bool isPersistent() const {
    auto t = getType();
    return t == ScopeType::Global || t == ScopeType::Module ||
           t == ScopeType::Import;
  }
};

// ===================================================================
// GlobalScope - Top-level program scope
// ===================================================================
struct GlobalScope : SemanticScopeBase {
  ScopeType getType() const override { return ScopeType::Global; }
};

// ===================================================================
// ModuleScope - Module/namespace scope
// ===================================================================
struct ModuleScope : SemanticScopeBase {
  ScopeType getType() const override { return ScopeType::Module; }
};

// ===================================================================
// ImportScope - Wraps an imported file's declarations
// ===================================================================
struct ImportScope : SemanticScopeBase {
  ScopeType getType() const override { return ScopeType::Import; }
};

// ===================================================================
// FunctionScope - Function or lambda body scope
// ===================================================================
struct FunctionScope : SemanticScopeBase {
  ScopeType getType() const override { return ScopeType::Function; }
};

// ===================================================================
// ClassScope - Class definition scope (for method analysis)
// ===================================================================
struct ClassScope : SemanticScopeBase {
  ScopeType getType() const override { return ScopeType::Class; }
};

// ===================================================================
// InterfaceScope - Interface definition scope
// ===================================================================
struct InterfaceScope : SemanticScopeBase {
  ScopeType getType() const override { return ScopeType::Interface; }
};

// ===================================================================
// BlockScope - Block scope (if, while, for, etc.)
// ===================================================================
struct BlockScope : SemanticScopeBase {
  ScopeType getType() const override { return ScopeType::Block; }
};

// ===================================================================
// TypeParamsScope - Type parameter binding scope (for generic instantiation)
// ===================================================================
struct TypeParamsScope : SemanticScopeBase {
  ScopeType getType() const override { return ScopeType::TypeParams; }
};

// ===================================================================
// Inline downcasting implementations
// ===================================================================
inline FunctionScope* SemanticScopeBase::asFunction() {
  if (getType() == ScopeType::Function)
    return static_cast<FunctionScope*>(this);
  return nullptr;
}
inline const FunctionScope* SemanticScopeBase::asFunction() const {
  if (getType() == ScopeType::Function)
    return static_cast<const FunctionScope*>(this);
  return nullptr;
}
inline ClassScope* SemanticScopeBase::asClass() {
  if (getType() == ScopeType::Class) return static_cast<ClassScope*>(this);
  return nullptr;
}
inline const ClassScope* SemanticScopeBase::asClass() const {
  if (getType() == ScopeType::Class)
    return static_cast<const ClassScope*>(this);
  return nullptr;
}
inline InterfaceScope* SemanticScopeBase::asInterface() {
  if (getType() == ScopeType::Interface)
    return static_cast<InterfaceScope*>(this);
  return nullptr;
}
inline const InterfaceScope* SemanticScopeBase::asInterface() const {
  if (getType() == ScopeType::Interface)
    return static_cast<const InterfaceScope*>(this);
  return nullptr;
}
inline BlockScope* SemanticScopeBase::asBlock() {
  if (getType() == ScopeType::Block) return static_cast<BlockScope*>(this);
  return nullptr;
}
inline const BlockScope* SemanticScopeBase::asBlock() const {
  if (getType() == ScopeType::Block)
    return static_cast<const BlockScope*>(this);
  return nullptr;
}

// Helper to check if a module name represents a library scope ($hash$)
inline bool isLibraryScope(const std::string& name) {
  return name.size() >= 2 && name.front() == '$' && name.back() == '$';
}

// Helper to check if a module name represents an import scope ($import_...$)
inline bool isImportScope(const std::string& name) {
  return name.size() > 9 && name.substr(0, 8) == "$import_" &&
         name.back() == '$';
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
