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
#include "semantic_analysis/access_checker.h"
#include "semantic_analysis/callable_signature.h"
#include "semantic_analysis/qualified_name.h"
#include "semantic_analysis/types.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::ast::ClassDefinitionAST;
using sun::ast::FunctionAST;
using sun::support::SourceFileId;

/** An argument's preferred type and other types its value can adopt. */
struct FunctionArgumentType {
  sun::semantic_analysis::TypePtr preferred;
  std::vector<sun::semantic_analysis::TypePtr> alternatives;
};

/**
 * Information about a variable in the symbol table
 */
struct VariableInfo {
  sun::semantic_analysis::TypePtr type;
  bool isGlobal;         // Declared at module level (not inside a function)
  bool isFunctionParam;  // Is it a parameter vs let binding
  bool isMoved = false;  // Has ownership been transferred (move semantics)
  // Set when the name is a lambda/function capture rather than an ordinary
  // local, and says how it was taken. Empty for everything else.
  std::optional<sun::ast::CaptureKind> captureKind;
  bool isConst = false;  // `const x`: the binding and its value never change
  sun::semantic_analysis::QualifiedName
      qualifiedName;  // Full qualified name (empty for locals)
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;  // globals only
  bool isCExtern = false;  // Native code owns this global's storage
  sun::semantic_analysis::DeclarationId declarationId;
};

/**
 * Information about a declared function
 */
struct FunctionInfo {
  sun::semantic_analysis::TypePtr returnType;
  std::vector<sun::semantic_analysis::TypePtr> paramTypes;
  std::vector<sun::ast::Capture> captures;
  sun::semantic_analysis::QualifiedName qualifiedName;  // Full qualified name
  bool canThrow = false;  // Whether this function can throw (declared with ,
                          // IError)
  // C-style trailing varargs (`extern function printf(fmt: ..., ...)`).
  // Calls may supply more arguments than paramTypes lists.
  bool isCVariadic = false;
  // Declared with `extern function` — calling it leaves Sun's checked world,
  // so call sites are gated on `unsafe`.
  bool isCExtern = false;
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;
  sun::semantic_analysis::DeclarationId declarationId;
  bool isForwardDeclaration = false;
};

/**
 * Indexed function table: O(1) name-based overload lookup + O(1) exact sig
 * lookup. Replaces std::map<string, FunctionInfo> which required O(n) prefix
 * scans to find overloads by name.
 */
class FunctionTable {
 public:
  /** An iterator that permits updating entries in the scope collection. */
  using iterator = std::unordered_map<
      sun::semantic_analysis::CallableSignature, FunctionInfo,
      sun::semantic_analysis::CallableSignatureHash>::iterator;
  /** An iterator that reads entries in the scope collection. */
  using const_iterator = std::unordered_map<
      sun::semantic_analysis::CallableSignature, FunctionInfo,
      sun::semantic_analysis::CallableSignatureHash>::const_iterator;

  /** Creates an instance with its default state. */
  FunctionTable() = default;

  /**
   * Copy constructor - rebuild byName_ with valid pointers
   */
  FunctionTable(const FunctionTable& other) : bySig_(other.bySig_) {
    rebuildByName();
  }

  /**
   * Copy assignment - rebuild byName_ with valid pointers
   */
  FunctionTable& operator=(const FunctionTable& other) {
    if (this != &other) {
      bySig_ = other.bySig_;
      rebuildByName();
    }
    return *this;
  }

  /**
   * Move operations can use defaults
   */
  FunctionTable(FunctionTable&&) = default;
  /** Transfers the stored state from another instance during move assignment. */
  FunctionTable& operator=(FunctionTable&&) = default;

  /** Provides indexed access to the stored elements. */
  FunctionInfo& operator[](
      const sun::semantic_analysis::CallableSignature& sig) {
    auto [it, inserted] = bySig_.emplace(sig, FunctionInfo{});
    if (inserted) {
      const auto& name = sig.name;
      byName_[name].push_back(&it->second);
    }
    return it->second;
  }

  /** Reports whether the scope contains an overload with the supplied signature. */
  bool contains(const sun::semantic_analysis::CallableSignature& sig) const {
    return bySig_.count(sig) > 0;
  }

  /** Looks up a visible  by name in the accessible scopes. */
  const_iterator find(
      const sun::semantic_analysis::CallableSignature& sig) const {
    return bySig_.find(sig);
  }

  /** Returns the iterator marking the end of the stored entries. */
  const_iterator end() const { return bySig_.end(); }
  /** Returns an iterator to the first stored entry. */
  const_iterator begin() const { return bySig_.begin(); }
  /** Returns the iterator marking the end of the stored entries. */
  iterator end() { return bySig_.end(); }
  /** Returns an iterator to the first stored entry. */
  iterator begin() { return bySig_.begin(); }
  /** Reports whether this object contains no entries. */
  bool empty() const { return bySig_.empty(); }
  /** Returns the number of stored entries. */
  size_t size() const { return bySig_.size(); }

  /**
   * Check if any function with this base name exists (O(1))
   */
  bool hasName(const std::string& name) const {
    return byName_.count(name) > 0;
  }

  /**
   * Check if any function with this base name exists, also trying qualified
   */
  bool hasNameOrQualified(const std::string& name,
                          const std::string& qualifiedName) const {
    if (byName_.count(name) > 0) return true;
    if (!qualifiedName.empty() && byName_.count(qualifiedName) > 0) return true;
    return false;
  }

  /**
   * Get all overloads for a given base name (O(1) lookup)
   */
  const std::vector<FunctionInfo*>* getOverloads(
      const std::string& name) const {
    auto it = byName_.find(name);
    if (it != byName_.end()) return &it->second;
    return nullptr;
  }

 private:
  std::unordered_map<sun::semantic_analysis::CallableSignature, FunctionInfo,
                     sun::semantic_analysis::CallableSignatureHash>
      bySig_;
  std::unordered_map<std::string, std::vector<FunctionInfo*>> byName_;

  /**
   * Rebuild byName_ index from bySig_ (used after copy)
   */
  void rebuildByName() {
    byName_.clear();
    for (auto& [sig, info] : bySig_) {
      const auto& name = sig.name;
      byName_[name].push_back(&info);
    }
  }

};

/**
 * Type of scope in the scope tree
 */
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

/**
 * Alias import from a using statement (legacy — being replaced by
 * ImportBinding)
 */
struct UsingImport {
  SourceFileId sourceFileId = 0;
  std::string namespacePath;  // "std" or "std.nested"
  std::string target;         // "Vec" for specific, "*" for wildcard
  bool isWildcard;            // true if target == "*" (using std;)

  /** Records the imported scope path and the name selected from it. */
  UsingImport(std::string nsPath, std::string t)
      : namespacePath(std::move(nsPath)),
        target(std::move(t)),
        isWildcard(target == "*") {}
};

// Forward declaration for scope pointer in ImportBinding
struct SemanticScopeBase;

/**
 * Scope-based import binding — a reference to a symbol in another scope
 */
struct ImportBinding {
  SourceFileId sourceFileId = 0;
  std::string localName;  // How the symbol is referred to locally ("Vec")
  SemanticScopeBase* sourceScope;  // Pointer to the scope it was imported from
  std::string sourceName;          // Name in the source scope ("Vec")
  bool isWildcard;  // true for "using std;" (localName/sourceName unused)

  /** Creates a binding from a local import name to its source declaration. */
  ImportBinding() : sourceScope(nullptr), isWildcard(false) {}
  /** Creates a binding from a local import name to its source declaration. */
  ImportBinding(std::string local, SemanticScopeBase* src, std::string srcName)
      : localName(std::move(local)),
        sourceScope(src),
        sourceName(std::move(srcName)),
        isWildcard(false) {}
  /** Creates an import binding that exposes names from the supplied scope. */
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

/**
 * Kind of symbol found during lookup
 */
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

/**
 * Result of a symbol lookup - contains the symbol and where it was found
 */
struct SymbolMatch {
  SymbolKind kind = SymbolKind::None;
  std::string name;        // The source symbol name
  std::string modulePath;  // Full dot-separated path including library hashes
  std::string
      libraryHash;  // The library scope hash (empty if not from library)

  // Type information (one of these will be set based on kind)
  std::shared_ptr<sun::semantic_analysis::ClassType> classType;
  std::shared_ptr<sun::semantic_analysis::InterfaceType> interfaceType;
  std::shared_ptr<sun::semantic_analysis::EnumType> enumType;
  const GenericClassInfo* genericClassInfo = nullptr;
  const GenericInterfaceInfo* genericInterfaceInfo = nullptr;
  const GenericFunctionInfo* genericFunctionInfo = nullptr;
  const FunctionInfo* functionInfo = nullptr;
  const VariableInfo* variableInfo = nullptr;

  /** Reports whether this object contains no entries. */
  bool empty() const { return kind == SymbolKind::None; }
  /** Reports whether this lookup result refers to a declaration. */
  explicit operator bool() const { return kind != SymbolKind::None; }

  /**
   * Get display name for error messages (hides library hashes)
   */
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

/**
 * Information about a generic class definition (template)
 */
struct GenericClassInfo {
  const ClassDefinitionAST* AST;                        // Original AST node
  std::vector<sun::ast::TypeParameter> typeParameters;  // <T, U: Trait>
  std::weak_ptr<SemanticScopeBase> definitionScope;
  sun::semantic_analysis::QualifiedName
      qualifiedName;  // Captured at registration
};

/**
 * Information about a generic interface definition (template)
 */
struct GenericInterfaceInfo {
  const sun::ast::InterfaceDefinitionAST* AST;          // Original AST node
  std::vector<sun::ast::TypeParameter> typeParameters;  // <T, U: Trait>
  sun::semantic_analysis::QualifiedName
      qualifiedName;  // Captured at registration
  std::weak_ptr<SemanticScopeBase> definitionScope;
};

/**
 * Information about a generic enum definition (template)
 */
struct GenericEnumInfo {
  const sun::ast::EnumDefinitionAST* AST;               // Original AST node
  std::vector<sun::ast::TypeParameter> typeParameters;  // <T, U: Trait>
  sun::semantic_analysis::QualifiedName
      qualifiedName;  // Captured at registration
  std::weak_ptr<SemanticScopeBase> definitionScope;
};

/**
 * Information about a generic function definition (template)
 */
struct GenericFunctionInfo {
  const FunctionAST* AST;                               // Original AST node
  std::vector<sun::ast::TypeParameter> typeParameters;  // <T, U: Trait>
  std::optional<sun::ast::TypeAnnotation> returnType;  // Return type annotation
  std::vector<std::pair<std::string, sun::ast::TypeAnnotation>>
      params;  // Parameters
  sun::semantic_analysis::QualifiedName
      qualifiedName;  // Captured at registration
  std::weak_ptr<SemanticScopeBase> definitionScope;
};

/**
 * Information about a specialized (monomorphized) generic function
 */
struct SpecializedFunctionInfo {
  sun::semantic_analysis::TypePtr returnType;
  std::vector<sun::semantic_analysis::TypePtr> paramTypes;
  std::vector<sun::ast::Capture> captures;  // Captures with substituted types
  std::shared_ptr<FunctionAST> specializedAST;  // The analyzed clone
  // Name this specialization is emitted under: the template's qualified name
  // with the type arguments appended. Call sites carry it rather than
  // recomputing, so one specialization keeps one symbol no matter where the
  // call sits relative to the definition.
  sun::semantic_analysis::QualifiedName qualifiedName;

  /**
   * Whether the specialization's signature carries 'throws IError'
   */
  bool canThrow() const {
    return specializedAST && specializedAST->getProto().canThrow();
  }

  /**
   * The specialization seen as an ordinary resolved function — what a call
   * site that named the template ends up calling.
   */
  FunctionInfo asFunctionInfo() const {
    FunctionInfo info{returnType, paramTypes, captures, qualifiedName,
                      canThrow()};
    if (specializedAST) info.declarationId = specializedAST->getDeclarationId();
    return info;
  }

  /**
   * Type of the call itself, for the callee expression
   */
  sun::semantic_analysis::TypePtr functionType() const {
    return sun::semantic_analysis::Types::Function(returnType, paramTypes,
                                                   canThrow());
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

/**
 * Alias for backward compatibility
 */
using SemanticScope = SemanticScopeBase;

/**
 * ===================================================================
 * Access control hooks for symbol lookup
 *
 * Lookups filter out symbols the asking code may not see (see
 * include/semantic_analysis/visibility.h). The analyzer installs an
 * AccessContext on the root scope; it answers "which module is asking" and
 * reports a denial. Without a context (no analyzer attached) lookups are
 * unfiltered.
 * ===================================================================
 */
struct AccessContext {
  /** Destroys this object and releases its owned members. */
  virtual ~AccessContext() = default;
  /** The module whose source is being analyzed. */
  virtual sun::semantic_analysis::DeclarationId currentModuleId() const = 0;
  /** Declaration ownership for this analysis session. */
  virtual const sun::semantic_analysis::DeclarationTable& declarationTable()
      const = 0;
  /** The source unit performing name lookup. */
  virtual SourceFileId currentSourceFileId() const { return 0; }
  /** Reports why the current scope cannot access the supplied declaration. */
  [[noreturn]] virtual void denyAccess(
      const sun::semantic_analysis::ItemRef& item) const = 0;
};

/**
 * ===================================================================
 * SemanticScopeBase - Base class for all scope types
 * Contains all fields for backward compatibility during incremental refactor.
 * Inherits enable_shared_from_this to allow weak_ptr references from
 * GenericClassInfo without creating circular ownership.
 * ===================================================================
 */
struct SemanticScopeBase
    : public std::enable_shared_from_this<SemanticScopeBase> {
  /** Destroys this object and releases its owned members. */
  virtual ~SemanticScopeBase() = default;

  /**
   * Get the scope type (virtual - each subclass returns its type)
   */
  virtual ScopeType getType() const = 0;

  /** Original module paths and nominal declarations, indexed on the root scope.
   */
  std::map<std::string, SemanticScopeBase*> canonicalModules;

  // ===== Identification (for persistent scopes) =====
  std::string scopeName;  // Display name (module name, source file, etc.)
  std::vector<std::string>
      scopePath;  // Scope path segments for qualified names

  // ===== Symbol tables (used by persistent scopes - Global/Module/Import)
  // =====
  FunctionTable functions;
  std::map<std::string, std::shared_ptr<sun::semantic_analysis::ClassType>>
      classes;
  std::map<std::string, ClassDefinitionAST*> classDefinitions;
  std::map<std::string, GenericClassInfo> genericClasses;
  std::map<std::string, std::shared_ptr<sun::semantic_analysis::InterfaceType>>
      interfaces;
  std::map<std::string, GenericInterfaceInfo> genericInterfaces;
  std::map<std::string, std::shared_ptr<sun::semantic_analysis::EnumType>>
      enums;
  std::map<std::string, GenericEnumInfo> genericEnums;
  std::map<std::string, GenericFunctionInfo> genericFunctions;
  std::map<std::string, std::shared_ptr<SemanticScopeBase>> childModules;
  std::map<std::string, VariableInfo> namespacedVariables;

  // ===== Declaration registration =====
  /** Create or reuse a child module without entering it. */
  ModuleScope& declareModule(const std::string& name);

  /** Record a module declaration and validate visibility on reopening. */
  ModuleScope& declareModule(const sun::ast::ModuleAST& module);

  /** Record the source class body associated with a name. */
  void declareClassDefinition(const std::string& name,
                              ClassDefinitionAST& definition);

  /** Record a type alias, rejecting a duplicate in this scope. */
  void declareTypeAlias(
      const std::string& name, sun::semantic_analysis::TypePtr type,
      std::optional<sun::support::Position> loc = std::nullopt);

  /** Register a function prototype (key = name + param types for overloads). */
  void declareFunction(
      const std::string& name, const FunctionInfo& info,
      std::optional<sun::support::Position> loc = std::nullopt);

  /**
   * Record a generic function template and its declaration scope. Repeated
   * registration of the same declaration is allowed; another template with
   * the same name in this scope is rejected.
   */
  void declareGenericFunction(FunctionAST& func);

  /**
   * Record a module-level variable by source name with its visibility.
   */
  void declareModuleVariable(
      const sun::semantic_analysis::QualifiedName& qualifiedName,
      sun::semantic_analysis::TypePtr type,
      sun::semantic_analysis::Visibility visibility, bool isConst = false,
      bool isCExtern = false,
      sun::semantic_analysis::DeclarationId declarationId = {});

  /**
   * Record a class in the current scope. A repeated registration of the same
   * name is ignored, which is what a diamond import produces.
   */
  void declareClass(
      const std::string& name,
      std::shared_ptr<sun::semantic_analysis::ClassType> classType,
      std::optional<sun::support::Position> loc = std::nullopt);

  /**
   * Record a generic class template in the current scope, along with the
   * scope it was declared in, so its bodies resolve names as written there.
   */
  void declareGenericClass(
      const std::string& name, const GenericClassInfo& info,
      std::optional<sun::support::Position> loc = std::nullopt);

  /**
   * Record an interface in the current scope. A repeated registration of the
   * same name is ignored, which is what a diamond import produces.
   */
  void declareInterface(
      const std::string& name,
      std::shared_ptr<sun::semantic_analysis::InterfaceType> interfaceType,
      std::optional<sun::support::Position> loc = std::nullopt);

  /** Record a generic interface template in the current scope. */
  void declareGenericInterface(
      const std::string& name, const GenericInterfaceInfo& info,
      std::optional<sun::support::Position> loc = std::nullopt);

  /** Record an enum in the current scope. */
  void declareEnum(const std::string& name,
                   std::shared_ptr<sun::semantic_analysis::EnumType> enumType);

  /**
   * Record a generic enum template in the current scope, along with the scope
   * it was declared in.
   */
  void declareGenericEnum(const std::string& name, GenericEnumInfo info);

  /** Bind type parameter names to concrete types in the current scope. */
  void declareTypeParameters(
      const std::vector<std::string>& params,
      const std::vector<sun::semantic_analysis::TypePtr>& args);

  /** Record a variable here, checking reserved names and global shadowing. */
  void declareVariable(
      const std::string& name, sun::semantic_analysis::TypePtr type,
      bool isParam = false, bool isConst = false,
      sun::semantic_analysis::DeclarationId declarationId = {});

  /** Report whether this scope is outside every function body. */
  bool isAtModuleLevel() const {
    for (auto* scope = this; scope; scope = scope->parent)
      if (scope->getType() == ScopeType::Function) return false;
    return true;
  }

  // ===== Transient state (all scopes) =====
  std::map<std::string, VariableInfo> variables;
  std::map<std::string, sun::semantic_analysis::TypePtr> typeParameters;
  std::map<std::string, sun::semantic_analysis::TypePtr> typeAliases;
  std::map<std::string, sun::semantic_analysis::TypePtr> narrowedTypes;
  std::vector<UsingImport> usingImports;
  std::vector<ImportBinding> importBindings;

  // Parent scope pointer (tree structure)
  SemanticScopeBase* parent = nullptr;
  // Non-module child scopes (for scope tree traversal)
  std::vector<std::shared_ptr<SemanticScopeBase>> children;

  // Set on the root scope by the analyzer; consulted by every lookup.
  const AccessContext* accessContext = nullptr;
  /** Returns the scope context used to check declaration visibility. */
  const AccessContext* accessCtx() const {
    auto* s = this;
    while (s->parent) s = s->parent;
    return s->accessContext;
  }

  /** Whether an import belongs to the source unit performing lookup. */
  bool admitsImport(SourceFileId sourceFile) const {
    const auto* context = accessCtx();
    return sourceFile == (context ? context->currentSourceFileId() : 0);
  }

  // True if this scope was loaded from an external .moon file
  bool isExternal = false;

  // ===== Try/unsafe block tracking (used on any scope) =====
  int tryBlockDepth = 0;
  int unsafeBlockDepth = 0;
  bool inUnsafeContext = false;

  /**
   * ===== Symbol lookup methods (delegate to persistent scope impl) =====
   */
  bool hasSymbol(const std::string& name) const;
  /**
   * Like hasSymbol, but only counts symbols the filter admits (private ones
   * are recorded on the filter as denied candidates).
   */
  bool hasAccessibleSymbol(const std::string& name,
                           class AccessFilter& filter) const;
  /** Looks up a visible class by name in the accessible scopes. */
  std::shared_ptr<sun::semantic_analysis::ClassType> findClass(
      const std::string& name) const;
  /** Looks up a visible generic class by name in the accessible scopes. */
  const GenericClassInfo* findGenericClass(const std::string& name) const;
  /** Looks up a visible interface by name in the accessible scopes. */
  std::shared_ptr<sun::semantic_analysis::InterfaceType> findInterface(
      const std::string& name) const;
  /** Looks up a visible generic interface by name in the accessible scopes. */
  const GenericInterfaceInfo* findGenericInterface(
      const std::string& name) const;
  /** Looks up a visible enum by name in the accessible scopes. */
  std::shared_ptr<sun::semantic_analysis::EnumType> findEnum(
      const std::string& name) const;
  /** Looks up a visible generic enum by name in the accessible scopes. */
  const GenericEnumInfo* findGenericEnum(const std::string& name) const;
  /** Appends the visible overloads matching the requested function name. */
  void collectFunctions(const std::string& name,
                        std::vector<FunctionInfo>& results) const;

  /**
   * Clone symbol tables (for diamond import handling)
   */
  std::shared_ptr<SemanticScopeBase> cloneSymbols(
      SemanticScopeBase* newParent) const;

  // ===== Scope-chain lookup methods =====
  // These traverse the parent chain, import scopes,
  // and import bindings to find symbols.

  /**
   * Generic scope-chain traversal: calls finder(scope) at each scope in chain
   */
  template <typename ResultT, typename Finder>
  ResultT lookupInChain(const std::string& name, Finder finder) const;

  /**
   * Lookup a class by name in the scope chain
   */
  std::shared_ptr<sun::semantic_analysis::ClassType> lookupClass(
      const std::string& name) const;

  /**
   * Lookup a generic class by name in the scope chain
   */
  const GenericClassInfo* lookupGenericClass(const std::string& name) const;

  /**
   * Lookup an interface by name in the scope chain
   */
  std::shared_ptr<sun::semantic_analysis::InterfaceType> lookupInterface(
      const std::string& name) const;

  /**
   * Lookup a generic interface by name in the scope chain
   */
  const GenericInterfaceInfo* lookupGenericInterface(
      const std::string& name) const;

  /**
   * Lookup an enum by name in the scope chain
   */
  std::shared_ptr<sun::semantic_analysis::EnumType> lookupEnum(
      const std::string& name) const;

  /**
   * Lookup a generic enum by name in the scope chain
   */
  const GenericEnumInfo* lookupGenericEnum(const std::string& name) const;

  /**
   * Lookup a variable by name in the scope chain
   */
  VariableInfo* lookupVariable(const std::string& name);

  /**
   * Lookup a generic function by name in the scope chain
   */
  const GenericFunctionInfo* lookupGenericFunction(
      const std::string& name) const;

  /**
   * Get all function overloads with the given name
   */
  std::vector<FunctionInfo> getAllFunctions(const std::string& name) const;

  /** Select an overload by types, then by supplied alternative types. */
  std::optional<FunctionInfo> lookupFunction(
      const std::string& name,
      const std::vector<FunctionArgumentType>& argTypes,
      std::optional<sun::support::Position> loc = std::nullopt) const;

  /**
   * Select an overload from this scope's own function table. Record inaccessible
   * candidates on the filter. Setting matchAlternatives enables the fallback
   * pass, which lookupFunction runs only after ordinary lookup finds no match.
   */
  std::optional<FunctionInfo> lookupFunctionLocal(
      const std::string& name,
      const std::vector<FunctionArgumentType>& argTypes,
      class AccessFilter* filter = nullptr, bool matchAlternatives = false,
      std::optional<sun::support::Position> loc = std::nullopt) const;

  /**
   * Lookup module scope by dot-separated path
   */
  SemanticScopeBase* lookupModuleScope(const std::string& dotPath) const;

  /**
   * Get all active using imports from all enclosing scopes
   */
  std::vector<UsingImport> getActiveUsingImports() const;

  /**
   * Resolve a name considering using statements and module scopes
   */
  sun::semantic_analysis::QualifiedName resolveNameWithUsings(
      const std::string& name) const;

  /**
   * Lookup a namespaced variable (module-qualified)
   */
  VariableInfo* lookupQualifiedVariable(const std::string& qualifiedName);

  /**
   * Lookup a namespaced function (module-qualified)
   */
  const FunctionInfo* lookupQualifiedFunction(
      const std::string& qualifiedName) const;

  /**
   * Check if a name refers to a module
   */
  bool isModuleName(const std::string& name) const;

  /**
   * Get the current scope path as a vector of segments
   */
  std::vector<std::string> getCurrentScopePath() const;

  /**
   * ===== Downcasting helpers =====
   */
  FunctionScope* asFunction();
  /** Accesses this scope as a function scope when its kind matches. */
  const FunctionScope* asFunction() const;
  /** Accesses this scope as a class scope when its kind matches. */
  ClassScope* asClass();
  /** Accesses this scope as a class scope when its kind matches. */
  const ClassScope* asClass() const;
  /** Accesses this scope as a interface scope when its kind matches. */
  InterfaceScope* asInterface();
  /** Accesses this scope as a interface scope when its kind matches. */
  const InterfaceScope* asInterface() const;
  /** Accesses this scope as a block scope when its kind matches. */
  BlockScope* asBlock();
  /** Accesses this scope as a block scope when its kind matches. */
  const BlockScope* asBlock() const;

  /**
   * Check if this is a persistent scope type
   */
  bool isPersistent() const {
    auto t = getType();
    return t == ScopeType::Global || t == ScopeType::Module ||
           t == ScopeType::Import;
  }
};

/**
 * ===================================================================
 * GlobalScope - Top-level program scope
 * ===================================================================
 */
struct GlobalScope : SemanticScopeBase {
  /** Returns the lexical scope category used for scope-specific lookup. */
  ScopeType getType() const override { return ScopeType::Global; }
};

/**
 * ===================================================================
 * ModuleScope - Module/namespace scope
 * ===================================================================
 */
struct ModuleScope : SemanticScopeBase {
  sun::semantic_analysis::DeclarationId declarationId;
  /** Returns the lexical scope category used for scope-specific lookup. */
  ScopeType getType() const override { return ScopeType::Module; }
  // Source spelling; the declaration record identifies the parent module.
  sun::semantic_analysis::QualifiedName qualifiedName;
  sun::semantic_analysis::Visibility visibility =
      sun::semantic_analysis::Visibility::Private;
  bool visibilityDeclared = false;  // A source declaration set `visibility`
};

/**
 * ===================================================================
 * ImportScope - Wraps an imported file's declarations
 * ===================================================================
 */
struct ImportScope : SemanticScopeBase {
  /** Returns the lexical scope category used for scope-specific lookup. */
  ScopeType getType() const override { return ScopeType::Import; }
};

/**
 * ===================================================================
 * FunctionScope - Function or lambda body scope
 * ===================================================================
 */
struct FunctionScope : SemanticScopeBase {
  /** Returns the lexical scope category used for scope-specific lookup. */
  ScopeType getType() const override { return ScopeType::Function; }

  std::string functionSignature;  // e.g., "outer(i32)"
  sun::semantic_analysis::QualifiedName functionName;
  bool functionCanThrow = false;
  sun::semantic_analysis::TypePtr
      functionReturnType;  // for return-position type inference

  // Variadic parameter pack for this function, when it is a specialized
  // variadic body. Holds the pack's name (e.g. "args") and the resolved type of
  // each expanded element. Used to expand `args...` into concrete, typed
  // argument nodes during call analysis.
  std::optional<
      std::pair<std::string, std::vector<sun::semantic_analysis::TypePtr>>>
      variadicParam;
};

/**
 * ===================================================================
 * ClassScope - Class definition scope (for method analysis)
 * ===================================================================
 */
struct ClassScope : SemanticScopeBase {
  /** Returns the lexical scope category used for scope-specific lookup. */
  ScopeType getType() const override { return ScopeType::Class; }

  std::string classBaseName;     // Display name (e.g., "Vec")
};

/**
 * ===================================================================
 * InterfaceScope - Interface definition scope
 * ===================================================================
 */
struct InterfaceScope : SemanticScopeBase {
  /** Returns the lexical scope category used for scope-specific lookup. */
  ScopeType getType() const override { return ScopeType::Interface; }

  std::string interfaceBaseName;     // Display name (e.g., "IShape")
};

/**
 * ===================================================================
 * BlockScope - Block scope (if, while, for, etc.)
 * ===================================================================
 */
struct BlockScope : SemanticScopeBase {
  /** Returns the lexical scope category used for scope-specific lookup. */
  ScopeType getType() const override { return ScopeType::Block; }
};

/**
 * ===================================================================
 * TypeParamsScope - Type parameter binding scope (for generic instantiation)
 * ===================================================================
 */
struct TypeParamsScope : SemanticScopeBase {
  /** Returns the lexical scope category used for scope-specific lookup. */
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

/**
 * Helper to check if a module name represents a library scope ($hash$)
 */
inline bool isLibraryScope(const std::string& name) {
  return name.size() >= 2 && name.front() == '$' && name.back() == '$';
}

/**
 * Helper to check if a module name represents an import scope ($import_...$)
 */
inline bool isImportScope(const std::string& name) {
  return name.size() > 9 && name.substr(0, 8) == "$import_" &&
         name.back() == '$';
}

/**
 * -------------------------------------------------------------------
 * accessItem — describe a lookup result for the access predicate
 * -------------------------------------------------------------------
 * Visibility comes from the record (or its AST for generic templates); the
 * declaration ID selects the owning module from the declaration table.
 */
inline sun::semantic_analysis::ItemRef accessItem(
    const std::shared_ptr<sun::semantic_analysis::ClassType>& c) {
  return {"class", c->getDisplayName(), "", c->visibility,
          c->getDeclarationId()};
}
/** Wraps a declaration in the common representation used for visibility checks. */
inline sun::semantic_analysis::ItemRef accessItem(const GenericClassInfo* g) {
  return {"class", g->qualifiedName.baseName, "",
          g->AST ? g->AST->getVisibility()
                 : sun::semantic_analysis::Visibility::Private,
          g->AST ? g->AST->getDeclarationId()
                 : sun::semantic_analysis::DeclarationId{}};
}
/** Wraps a declaration in the common representation used for visibility checks. */
inline sun::semantic_analysis::ItemRef accessItem(
    const std::shared_ptr<sun::semantic_analysis::InterfaceType>& i) {
  return {"interface", i->getBaseName(), "", i->visibility,
          i->getDeclarationId()};
}
/** Wraps a declaration in the common representation used for visibility checks. */
inline sun::semantic_analysis::ItemRef accessItem(
    const GenericInterfaceInfo* g) {
  return {"interface", g->qualifiedName.baseName, "",
          g->AST ? g->AST->getVisibility()
                 : sun::semantic_analysis::Visibility::Private,
          g->AST ? g->AST->getDeclarationId()
                 : sun::semantic_analysis::DeclarationId{}};
}
/** Wraps a declaration in the common representation used for visibility checks. */
inline sun::semantic_analysis::ItemRef accessItem(
    const std::shared_ptr<sun::semantic_analysis::EnumType>& e) {
  return {"enum", e->getBaseName(), "", e->visibility, e->getDeclarationId()};
}
/** Wraps a declaration in the common representation used for visibility checks. */
inline sun::semantic_analysis::ItemRef accessItem(const GenericEnumInfo* g) {
  return {"enum", g->qualifiedName.baseName, "",
          g->AST ? g->AST->getVisibility()
                 : sun::semantic_analysis::Visibility::Private,
          g->AST ? g->AST->getDeclarationId()
                 : sun::semantic_analysis::DeclarationId{}};
}
/** Wraps a declaration in the common representation used for visibility checks. */
inline sun::semantic_analysis::ItemRef accessItem(
    const GenericFunctionInfo* g) {
  return {"function", g->qualifiedName.baseName, "",
          g->AST ? g->AST->getVisibility()
                 : sun::semantic_analysis::Visibility::Private,
          g->AST ? g->AST->getDeclarationId()
                 : sun::semantic_analysis::DeclarationId{}};
}
/** Wraps a declaration in the common representation used for visibility checks. */
inline sun::semantic_analysis::ItemRef accessItem(const FunctionInfo& f) {
  return {"function", f.qualifiedName.baseName, "", f.visibility,
          f.declarationId};
}
/** Wraps a declaration in the common representation used for visibility checks. */
inline sun::semantic_analysis::ItemRef accessItem(const FunctionInfo* f) {
  return accessItem(*f);
}
/** Wraps a declaration in the common representation used for visibility checks. */
inline sun::semantic_analysis::ItemRef accessItem(const VariableInfo* v) {
  return {"variable", v->qualifiedName.baseName, "", v->visibility,
          v->declarationId};
}
/** Wraps a declaration in the common representation used for visibility checks. */
inline sun::semantic_analysis::ItemRef accessItem(const ModuleScope& m) {
  return {"module", m.qualifiedName.baseName, "", m.visibility,
          m.declarationId};
}

/**
 * -------------------------------------------------------------------
 * AccessFilter — admits lookup results the asking module may see and
 * remembers the first private one it skipped, so a lookup that finds only
 * private candidates reports "is private to ..." instead of "unknown".
 * -------------------------------------------------------------------
 */
class AccessFilter {
  const AccessContext* ctx_ = nullptr;
  sun::semantic_analysis::DeclarationId from_;
  std::optional<sun::semantic_analysis::ItemRef> denied_;

 public:
  /** Creates a visibility filter for lookups from the supplied scope. */
  explicit AccessFilter(const SemanticScopeBase* scope)
      : ctx_(scope ? scope->accessCtx() : nullptr) {
    if (ctx_) from_ = ctx_->currentModuleId();
  }

  /** Reports whether lookups are subject to visibility filtering. */
  bool enabled() const { return ctx_ != nullptr; }
  /** Returns the scope from which declaration access is being checked. */
  sun::semantic_analysis::DeclarationId from() const { return from_; }

  /** Accepts a declaration only if the current scope may access it. */
  bool admitItem(sun::semantic_analysis::ItemRef item) {
    if (!ctx_ || sun::semantic_analysis::isAccessible(from_, item,
                                                      ctx_->declarationTable()))
      return true;
    if (!denied_) denied_ = std::move(item);
    return false;
  }
  /** Filters a lookup result according to the current scope's access rights. */
  template <typename T>
  bool admit(const T& result) {
    return admitItem(accessItem(result));
  }

  /** Reports whether this object has denied. */
  bool hasDenied() const { return denied_.has_value(); }
  /**
   * Throws the recorded denial, if any.
   */
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
ResultT SemanticScopeBase::lookupInChain(const std::string& name,
                                         Finder finder) const {
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
      if (!s->admitsImport(binding.sourceFileId)) continue;
      if (!binding.sourceScope) continue;
      if (binding.isWildcard || binding.localName == name) {
        result = probe(binding.sourceScope);
        if (result) return result;
      }
    }
  }
  filter.finish();
  return ResultT{};
}

}  // namespace sun::semantic_analysis
