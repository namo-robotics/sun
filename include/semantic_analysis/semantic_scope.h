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

#include "semantic_analysis/access_checker.h"
#include "ast.h"
#include "semantic_analysis/qualified_name.h"
#include "semantic_analysis/types.h"

// Information about a variable in the symbol table
struct VariableInfo {
  sun::TypePtr type;
  bool isGlobal;         // Declared at module level (not inside a function)
  bool isFunctionParam;  // Is it a parameter vs let binding
  bool isMoved = false;  // Has ownership been transferred (move semantics)
  bool isCapture = false;       // Declared as a lambda/function capture
  bool isByRefCapture = false;  // Captured via [ref x] - mutable through env
  bool isConst = false;  // `const x`: the binding and its value never change
  sun::QualifiedName qualifiedName;  // Full qualified name (empty for locals)
  sun::Visibility visibility = sun::Visibility::Private;  // globals only
};

// Information about a declared function
struct FunctionInfo {
  sun::TypePtr returnType;
  std::vector<sun::TypePtr> paramTypes;
  std::vector<Capture> captures;
  sun::QualifiedName qualifiedName;  // Full qualified name
  bool canThrow = false;  // Whether this function can throw (declared with ,
                          // IError)
  // C-style trailing varargs (`extern function printf(fmt: ..., ...)`).
  // Calls may supply more arguments than paramTypes lists.
  bool isCVariadic = false;
  // Declared with `extern function` — calling it leaves Sun's checked world,
  // so call sites are gated on `unsafe`.
  bool isCExtern = false;
  sun::Visibility visibility = sun::Visibility::Private;
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
  GenericFunction,
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
  const GenericFunctionInfo* genericFunctionInfo = nullptr;
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

// Generic templates record the scope they were declared in
// (`definitionScope`, weak to avoid cycles; scopes are never freed). Their
// bodies are analyzed inside that scope, wherever the instantiation is
// requested from, so name resolution and access control come from the scope
// stack. Set by the register* functions.

// Information about a generic class definition (template)
struct GenericClassInfo {
  const ClassDefinitionAST* AST;                     // Original AST node
  std::vector<std::string> typeParameters;           // ["T", "U", etc.]
  std::weak_ptr<SemanticScopeBase> definitionScope;
  sun::QualifiedName qualifiedName;                  // Captured at registration
};

// Information about a generic interface definition (template)
struct GenericInterfaceInfo {
  const InterfaceDefinitionAST* AST;        // Original AST node
  std::vector<std::string> typeParameters;  // ["T", "U", etc.]
  sun::QualifiedName qualifiedName;         // Captured at registration
  std::weak_ptr<SemanticScopeBase> definitionScope;
};

// Information about a generic enum definition (template)
struct GenericEnumInfo {
  const EnumDefinitionAST* AST;             // Original AST node
  std::vector<std::string> typeParameters;  // ["T", "U", etc.]
  sun::QualifiedName qualifiedName;         // Captured at registration
  std::weak_ptr<SemanticScopeBase> definitionScope;
};

// Information about a generic function definition (template)
struct GenericFunctionInfo {
  const FunctionAST* AST;                    // Original AST node
  std::vector<std::string> typeParameters;   // ["T", "U", etc.]
  std::optional<TypeAnnotation> returnType;  // Return type annotation
  std::vector<std::pair<std::string, TypeAnnotation>> params;  // Parameters
  sun::QualifiedName qualifiedName;          // Captured at registration
  std::weak_ptr<SemanticScopeBase> definitionScope;
};

// Information about a specialized (monomorphized) generic function
struct SpecializedFunctionInfo {
  sun::TypePtr returnType;
  std::vector<sun::TypePtr> paramTypes;
  std::vector<Capture> captures;  // Captures with substituted types
  std::shared_ptr<FunctionAST> specializedAST;  // The analyzed clone
  // Name this specialization is emitted under: the template's qualified name
  // with the type arguments appended. Call sites carry it rather than
  // recomputing, so one specialization keeps one symbol no matter where the
  // call sits relative to the definition.
  sun::QualifiedName qualifiedName;

  // Whether the specialization's signature carries ', IError'
  bool canThrow() const {
    return specializedAST && specializedAST->getProto().canThrow();
  }

  // The specialization seen as an ordinary resolved function — what a call
  // site that named the template ends up calling.
  FunctionInfo asFunctionInfo() const {
    return FunctionInfo{returnType,    paramTypes, captures,
                        qualifiedName, canThrow()};
  }

  // Type of the call itself, for the callee expression
  sun::TypePtr functionType() const {
    return sun::Types::Function(returnType, paramTypes, canThrow());
  }
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
// Access control hooks for symbol lookup
//
// Lookups filter out symbols the asking code may not see (see
// include/semantic_analysis/visibility.h). The analyzer installs an AccessContext on the root
// scope; it answers "which module is asking" and reports a denial. Without a
// context (no analyzer attached) lookups are unfiltered.
// ===================================================================
struct AccessContext {
  virtual ~AccessContext() = default;
  virtual sun::ModulePath currentModulePath() const = 0;
  [[noreturn]] virtual void denyAccess(
      const sun::access::ItemRef& item) const = 0;
};

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
  std::vector<std::string> scopePath;  // Scope path segments for qualified names

  // ===== Symbol tables (used by persistent scopes - Global/Module/Import)
  // =====
  FunctionTable functions;
  std::map<std::string, std::shared_ptr<sun::ClassType>> classes;
  std::map<std::string, ClassDefinitionAST*> classDefinitions;
  std::map<std::string, GenericClassInfo> genericClasses;
  std::map<std::string, std::shared_ptr<sun::InterfaceType>> interfaces;
  std::map<std::string, GenericInterfaceInfo> genericInterfaces;
  std::map<std::string, std::shared_ptr<sun::EnumType>> enums;
  std::map<std::string, GenericEnumInfo> genericEnums;
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

  // Set on the root scope by the analyzer; consulted by every lookup.
  const AccessContext* accessContext = nullptr;
  const AccessContext* accessCtx() const {
    auto* s = this;
    while (s->parent) s = s->parent;
    return s->accessContext;
  }

  // True if this scope was loaded from an external .moon file
  bool isExternal = false;

  // ===== Try/unsafe block tracking (used on any scope) =====
  int tryBlockDepth = 0;
  int unsafeBlockDepth = 0;
  bool inUnsafeContext = false;

  // ===== Symbol lookup methods (delegate to persistent scope impl) =====
  bool hasSymbol(const std::string& name) const;
  // Like hasSymbol, but only counts symbols the filter admits (private ones
  // are recorded on the filter as denied candidates).
  bool hasAccessibleSymbol(const std::string& name,
                           class AccessFilter& filter) const;
  std::shared_ptr<sun::ClassType> findClass(const std::string& name) const;
  const GenericClassInfo* findGenericClass(const std::string& name) const;
  std::shared_ptr<sun::InterfaceType> findInterface(
      const std::string& name) const;
  const GenericInterfaceInfo* findGenericInterface(
      const std::string& name) const;
  std::shared_ptr<sun::EnumType> findEnum(const std::string& name) const;
  const GenericEnumInfo* findGenericEnum(const std::string& name) const;
  void collectFunctions(const std::string& prefix,
                        std::vector<FunctionInfo>& results) const;

  // Clone symbol tables (for diamond import handling)
  std::shared_ptr<SemanticScopeBase> cloneSymbols(
      SemanticScopeBase* newParent) const;

  // ===== Scope-chain lookup methods =====
  // These traverse the parent chain, import scopes,
  // and import bindings to find symbols.

  // Generic scope-chain traversal: calls finder(scope) at each scope in chain
  template <typename ResultT, typename Finder>
  ResultT lookupInChain(Finder finder) const;

  // Lookup a class by name in the scope chain
  std::shared_ptr<sun::ClassType> lookupClass(const std::string& name) const;

  // Lookup a generic class by name in the scope chain
  const GenericClassInfo* lookupGenericClass(const std::string& name) const;
  // Lookup by qualified name: walk to the scope path, then find the base name
  const GenericClassInfo* lookupGenericClass(
      const sun::QualifiedName& qualifiedName) const;

  // Lookup an interface by name in the scope chain
  std::shared_ptr<sun::InterfaceType> lookupInterface(
      const std::string& name) const;

  // Lookup a generic interface by name in the scope chain
  const GenericInterfaceInfo* lookupGenericInterface(
      const std::string& name) const;

  // Lookup an enum by name in the scope chain
  std::shared_ptr<sun::EnumType> lookupEnum(const std::string& name) const;

  // Lookup a generic enum by name in the scope chain
  const GenericEnumInfo* lookupGenericEnum(const std::string& name) const;

  // Lookup a variable by name in the scope chain
  VariableInfo* lookupVariable(const std::string& name);

  // Lookup a generic function by name in the scope chain
  const GenericFunctionInfo* lookupGenericFunction(
      const std::string& name) const;

  // Get all function overloads with the given name
  std::vector<FunctionInfo> getAllFunctions(const std::string& name) const;

  // Lookup function by name and exact argument types (overload resolution)
  std::optional<FunctionInfo> lookupFunction(
      const std::string& name,
      const std::vector<sun::TypePtr>& argTypes) const;

  // Same overload resolution, but restricted to this scope's own function
  // table — no walk to parents. Used for module-qualified calls, where the
  // callee's scope is already known.
  // Inaccessible overloads are skipped and recorded on `filter` (if given)
  // so the caller can report them once nothing else matched.
  std::optional<FunctionInfo> lookupFunctionLocal(
      const std::string& name, const std::vector<sun::TypePtr>& argTypes,
      class AccessFilter* filter = nullptr) const;

  // Lookup module scope by dot-separated path
  SemanticScopeBase* lookupModuleScope(const std::string& dotPath) const;

  // Get all active using imports from all enclosing scopes
  std::vector<UsingImport> getActiveUsingImports() const;

  // Resolve a name considering using statements and module scopes
  sun::QualifiedName resolveNameWithUsings(const std::string& name) const;

  // Lookup a namespaced variable (module-qualified)
  VariableInfo* lookupQualifiedVariable(const std::string& qualifiedName);

  // Lookup a namespaced function (module-qualified)
  const FunctionInfo* lookupQualifiedFunction(
      const std::string& qualifiedName) const;

  // Check if a name refers to a module
  bool isModuleName(const std::string& name) const;

  // Get the current scope path as a vector of segments
  std::vector<std::string> getCurrentScopePath() const;

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
  // Structured name: owner() is the parent module path
  sun::QualifiedName qualifiedName;
  sun::Visibility visibility = sun::Visibility::Private;
  bool visibilityDeclared = false;  // A source declaration set `visibility`
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

  std::string functionSignature;  // e.g., "outer(i32)"
  sun::QualifiedName functionName;
  bool functionCanThrow = false;
  sun::TypePtr functionReturnType;  // for return-position type inference

  // Variadic parameter pack for this function, when it is a specialized
  // variadic body. Holds the pack's name (e.g. "args") and the resolved type of
  // each expanded element. Used to expand `args...` into concrete, typed
  // argument nodes during call analysis.
  std::optional<std::pair<std::string, std::vector<sun::TypePtr>>> variadicParam;
};

// ===================================================================
// ClassScope - Class definition scope (for method analysis)
// ===================================================================
struct ClassScope : SemanticScopeBase {
  ScopeType getType() const override { return ScopeType::Class; }

  std::string classBaseName;     // Display name (e.g., "Vec")
  std::string classMangledName;  // Full mangled name (e.g., "sun_Vec_i32")
};

// ===================================================================
// InterfaceScope - Interface definition scope
// ===================================================================
struct InterfaceScope : SemanticScopeBase {
  ScopeType getType() const override { return ScopeType::Interface; }

  std::string interfaceBaseName;     // Display name (e.g., "IShape")
  std::string interfaceMangledName;  // Full mangled name
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

// -------------------------------------------------------------------
// accessItem — describe a lookup result for the access predicate
// -------------------------------------------------------------------
// Visibility comes from the record (or its AST for generic templates); the
// owner is the record's QualifiedName::owner().
inline sun::access::ItemRef accessItem(
    const std::shared_ptr<sun::ClassType>& c) {
  return {"class", c->getDisplayName(), "", c->visibility,
          c->getQualifiedName().owner()};
}
inline sun::access::ItemRef accessItem(const GenericClassInfo* g) {
  return {"class", g->qualifiedName.baseName, "",
          g->AST ? g->AST->getVisibility() : sun::Visibility::Private,
          g->qualifiedName.owner()};
}
inline sun::access::ItemRef accessItem(
    const std::shared_ptr<sun::InterfaceType>& i) {
  return {"interface", i->getBaseName(), "", i->visibility,
          i->getQualifiedName().owner()};
}
inline sun::access::ItemRef accessItem(const GenericInterfaceInfo* g) {
  return {"interface", g->qualifiedName.baseName, "",
          g->AST ? g->AST->getVisibility() : sun::Visibility::Private,
          g->qualifiedName.owner()};
}
inline sun::access::ItemRef accessItem(const std::shared_ptr<sun::EnumType>& e) {
  return {"enum", e->getBaseName(), "", e->visibility,
          e->getQualifiedName().owner()};
}
inline sun::access::ItemRef accessItem(const GenericEnumInfo* g) {
  return {"enum", g->qualifiedName.baseName, "",
          g->AST ? g->AST->getVisibility() : sun::Visibility::Private,
          g->qualifiedName.owner()};
}
inline sun::access::ItemRef accessItem(const GenericFunctionInfo* g) {
  return {"function", g->qualifiedName.baseName, "",
          g->AST ? g->AST->getVisibility() : sun::Visibility::Private,
          g->qualifiedName.owner()};
}
inline sun::access::ItemRef accessItem(const FunctionInfo& f) {
  return {"function", f.qualifiedName.baseName, "", f.visibility,
          f.qualifiedName.owner()};
}
inline sun::access::ItemRef accessItem(const FunctionInfo* f) {
  return accessItem(*f);
}
inline sun::access::ItemRef accessItem(const VariableInfo* v) {
  return {"variable", v->qualifiedName.baseName, "", v->visibility,
          v->qualifiedName.owner()};
}
inline sun::access::ItemRef accessItem(const ModuleScope& m) {
  return {"module", m.qualifiedName.baseName, "", m.visibility,
          m.qualifiedName.owner()};
}

// -------------------------------------------------------------------
// AccessFilter — admits lookup results the asking module may see and
// remembers the first private one it skipped, so a lookup that finds only
// private candidates reports "is private to ..." instead of "unknown".
// -------------------------------------------------------------------
class AccessFilter {
  const AccessContext* ctx_ = nullptr;
  sun::ModulePath from_;
  std::optional<sun::access::ItemRef> denied_;

 public:
  explicit AccessFilter(const SemanticScopeBase* scope)
      : ctx_(scope ? scope->accessCtx() : nullptr) {
    if (ctx_) from_ = ctx_->currentModulePath();
  }

  bool enabled() const { return ctx_ != nullptr; }
  const sun::ModulePath& from() const { return from_; }

  bool admitItem(sun::access::ItemRef item) {
    if (!ctx_ || sun::access::isAccessible(from_, item)) return true;
    if (!denied_) denied_ = std::move(item);
    return false;
  }
  template <typename T>
  bool admit(const T& result) {
    return admitItem(accessItem(result));
  }

  bool hasDenied() const { return denied_.has_value(); }
  // Throws the recorded denial, if any.
  void finish() const {
    if (denied_) ctx_->denyAccess(*denied_);
  }
};

// -------------------------------------------------------------------
// Template implementation: lookupInChain
// Traverses the scope chain (parent + import children + import bindings)
// calling finder(scope) at each node.
// finder signature: ResultT finder(const SemanticScopeBase* scope)
// -------------------------------------------------------------------
template <typename ResultT, typename Finder>
ResultT SemanticScopeBase::lookupInChain(Finder finder) const {
  AccessFilter filter(this);
  auto probe = [&](const SemanticScopeBase* s) -> ResultT {
    auto r = finder(s);
    if (r && filter.admit(r)) return r;
    return ResultT{};
  };
  for (auto* s = this; s != nullptr; s = s->parent) {
    auto result = probe(s);
    if (result) return result;
    // Search direct import-scope children (one level of transparency)
    for (const auto& [childName, child] : s->childModules) {
      if (child && child->getType() == ScopeType::Import) {
        result = probe(child.get());
        if (result) return result;
        for (const auto& [modName, modChild] : child->childModules) {
          if (modChild && modChild->getType() == ScopeType::Module) {
            result = probe(modChild.get());
            if (result) return result;
          }
        }
      }
    }
    // Search import bindings from using statements
    for (const auto& binding : s->importBindings) {
      if (!binding.sourceScope) continue;
      if (binding.isWildcard) {
        result = probe(binding.sourceScope);
        if (result) return result;
      }
    }
  }
  filter.finish();
  return ResultT{};
}
