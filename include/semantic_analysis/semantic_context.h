// semantic_context.h — The state every part of semantic analysis shares
//
// One scope tree, one type registry, one "which class am I inside", one stack
// of source locations. The analyzer and its collaborators (declaration
// collection, type inference, generic specialization) all hold a reference to
// the same SemanticContext, so "where am I and what is in scope here" has a
// single owner instead of being a field on whichever class grew it first.
//
// The context answers only positional questions: what scope is current, what
// name is registered where, what module is asking. It never analyzes an
// expression or resolves a type annotation — those need the analyzer, which
// depends on this and not the other way round.

#pragma once

/** Provides shared diagnostics, source tracking, and compiler utilities. */
namespace sun::support {
struct Position;
}

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "semantic_analysis/access_checker.h"
#include "semantic_analysis/analysis_results.h"
#include "semantic_analysis/declaration_state.h"
#include "semantic_analysis/semantic_scope.h"
#include "semantic_analysis/type_registry.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::support::SourceFileId;

}

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/**
 * The shared state and scope machinery of a semantic analysis run. Implements
 * AccessContext so the scope lookups can filter by visibility: "which module
 * is asking" is the nearest module scope on the stack.
 */
class SemanticContext : public AccessContext {
  SourceFileId sourceFileId_ = 0;
  DeclarationState declarations_;
  size_t declarationCollectionDepth_ = 0;

 public:
  /** A registered interface and the scope where its annotations resolve. */
  struct InterfaceDefinition {
    sun::ast::InterfaceDefinitionAST *node;
    SemanticScope *scope;
  };
  std::map<DeclarationId, InterfaceDefinition> interfaceDefinitions;
  std::set<DeclarationId> resolvingInterfaceParents;
  /** Whether type declarations are being collected before body analysis. */
  bool isCollectingDeclarations() const {
    return declarationCollectionDepth_ != 0;
  }

  /** Track nested declaration collection independently of specialization jobs.
   */
  class DeclarationCollectionGuard {
   public:
    /** Enter declaration collection for this context. */
    explicit DeclarationCollectionGuard(SemanticContext &context)
        : context_(context) {
      ++context_.declarationCollectionDepth_;
    }
    /** Restore collection depth on success or failure. */
    ~DeclarationCollectionGuard() { --context_.declarationCollectionDepth_; }
    /** Keep each collection interval owned by one guard. */
    DeclarationCollectionGuard(const DeclarationCollectionGuard &) = delete;
    /** Prevent duplicate collection interval ownership. */
    DeclarationCollectionGuard &operator=(const DeclarationCollectionGuard &) =
        delete;

   private:
    SemanticContext &context_;
  };

  /** Start with an empty global scope holding the builtin functions. */
  explicit SemanticContext(
      std::shared_ptr<sun::semantic_analysis::AnalysisResults> results);

  /** Declaration identities, registered shapes, and pending extensions. */
  DeclarationState &declarations() { return declarations_; }

  // ---- Shared state ------------------------------------------------------

  /** Class and interface types, shared with codegen. */
  const std::shared_ptr<sun::semantic_analysis::TypeRegistry> &types() const {
    return results_->types;
  }

  /**
   * What analysis concludes about the whole program. Complete once the
   * analysis pipeline has run; later stages only read it.
   */
  AnalysisResults &results() { return *results_; }
  /** Read-only view of the program-wide analysis results. */
  const AnalysisResults &results() const { return *results_; }

  /** The scope currently being analyzed. */
  SemanticScope *scope() const { return currentScope_; }

  /** Access the scope currently being analyzed. */
  SemanticScope &currentScope() { return *currentScope_; }
  /** Returns the lexical scope used for current semantic lookups. */
  const SemanticScope &currentScope() const { return *currentScope_; }

  /** The global scope, for debugging and visualization. */
  SemanticScope &rootScope() { return *rootScope_; }
  /** Returns the outermost scope containing the analyzed program. */
  const SemanticScope &rootScope() const { return *rootScope_; }

  /** Set the class whose body is being analyzed, so `this` resolves to it. */
  void setCurrentClass(std::shared_ptr<sun::types::ClassType> classType);

  /** The class whose body is being analyzed, or null outside one. */
  std::shared_ptr<sun::types::ClassType> getCurrentClass() const;

  // ---- Scope navigation --------------------------------------------------

  /** Push a new scope of the given kind and make it current. */
  void enterScope(ScopeType type = ScopeType::Block);

  /** Make an existing scope current; exitScope returns to its parent. */
  void enterScope(SemanticScope &scope) { currentScope_ = &scope; }

  /**
   * Enter a type parameter scope with bindings (combines enterScope +
   * currentScope().declareTypeParameters).
   */
  void enterTypeParamScope(const std::vector<std::string> &params,
                           const std::vector<sun::types::TypePtr> &args);

  /**
   * Enter a module's scope, creating it if this is the first time the module
   * is opened. Re-opening a module returns to the same scope.
   */
  void enterModuleScope(const std::string &moduleName);

  /** Enter a class scope with qualified name for proper scope path. */
  void enterClassScope(const sun::semantic_analysis::QualifiedName &className);

  /** Enter an interface scope with qualified name for proper scope path. */
  void enterInterfaceScope(
      const sun::semantic_analysis::QualifiedName &interfaceName);

  /**
   * Enter a function scope with the function's signature for nested function
   * qualified names. The signature should be "funcName(paramType1,paramType2)".
   * funcName is the qualified name of the function.
   */
  void enterFunctionScope(const std::string &funcSig,
                          const sun::semantic_analysis::QualifiedName &funcName,
                          bool canThrow = false,
                          sun::types::TypePtr returnType = nullptr);

  /**
   * Make the parent scope current again. The scope itself stays in the tree
   * for debugging and visualization.
   */
  void exitScope();

  /** True when not inside any function scope (i.e. at module/global level). */
  bool isAtModuleLevel() const { return currentScope().isAtModuleLevel(); }

  /** Nearest enclosing function scope, or nullptr at module/global level. */
  FunctionScope *currentFunctionScope() const {
    for (auto *s = currentScope_; s != nullptr; s = s->parent)
      if (s->getType() == ScopeType::Function)
        return static_cast<FunctionScope *>(s);
    return nullptr;
  }

  /**
   * Return type of the nearest enclosing function scope (null outside
   * functions or when unresolved); used for return-position inference.
   */
  sun::types::TypePtr currentFunctionReturnType() const;

  // ---- Names and module paths -------------------------------------------

  /**
   * Get the current scope path as a vector of segments.
   * e.g., inside "module A { module B { } }", returns {"A", "B"}.
   */
  std::vector<std::string> getCurrentScopePath() const;

  /**
   * True when the name refers to a module, so `x.y` can be read as a
   * qualified name rather than a member access on a value.
   */
  bool isModuleName(const std::string &name) const;

  /**
   * Traverse childModules from global scope to find a module by dot-separated
   * path (e.g., "std" or "std.collections"). Returns nullptr if not found.
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
      const std::vector<sun::types::TypePtr> *argTypes = nullptr) const;

  /**
   * Resolve a bare name against the `using` imports in scope, giving the
   * qualified name it refers to (the name itself when nothing matches).
   */
  sun::semantic_analysis::QualifiedName resolveNameWithUsings(
      const std::string &name) const;

  /** Add a using import (legacy string-based). */
  void addUsingImport(const UsingImport &import);

  /** Add a scope-based import binding. */
  void addImportBinding(const ImportBinding &binding);

  /** Get all active using imports (from all enclosing scopes). */
  std::vector<UsingImport> getActiveUsingImports() const;

  // ---- Throwing, try and unsafe context ----------------------------------

  /**
   * Check if we're currently inside a function declared with "throws IError".
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
   * Are we analyzing the stubs a moon import carries? A stub keeps only its
   * signature — the body is stripped to an empty block — so checks about what
   * a body does (such as returning on every path) do not apply there.
   */
  bool isInMoonScope() const { return moonScopeDepth_ > 0; }

  /** Enter a moon import's stub scope. */
  void enterMoonScope() { ++moonScopeDepth_; }

  /** Leave a moon import's stub scope. */
  void exitMoonScope() {
    if (moonScopeDepth_ > 0) --moonScopeDepth_;
  }

  // ---- Variables ---------------------------------------------------------

  /**
   * Find a variable by its dotted name (`a.b.x`). An undotted name is an
   * ordinary variable lookup.
   */
  VariableInfo *lookupQualifiedVariable(const std::string &qualifiedName);

  /**
   * Narrow a variable's type for the rest of the current scope, after an
   * `_is<T>` guard proved it holds a T.
   */
  void narrowVariable(const std::string &varName,
                      sun::types::TypePtr narrowedType);

  /**
   * The narrowed type in effect for a variable, or its original type when no
   * guard applies. The more specific of the two wins (class over interface).
   */
  sun::types::TypePtr getNarrowedType(const std::string &varName,
                                      sun::types::TypePtr originalType) const;

  // ---- Functions ---------------------------------------------------------

  /**
   * Select an overload by preferred argument types, then by their alternatives.
   * Returns nullopt when no overload matches.
   */
  std::optional<FunctionInfo> lookupFunction(
      const std::string &name,
      const std::vector<FunctionArgumentType> &argTypes,
      std::optional<sun::support::Position> loc = std::nullopt) const;

  /** Every overload declared under the given name. */
  std::vector<FunctionInfo> getAllFunctions(const std::string &name) const;

  /** Find a function by its dotted name (`a.b.f`); undotted names never match.
   */
  const FunctionInfo *lookupQualifiedFunction(
      const std::string &qualifiedName) const;

  /**
   * Look up a generic function by name. Tries the direct name first, then
   * falls back to enclosing function prefix + name (for nested generic
   * functions).
   */
  const GenericFunctionInfo *lookupGenericFunction(
      const std::string &name) const;

  // ---- Types: classes, interfaces, enums and their templates -------------

  /** Find a class by name in the scope chain (null when there is none). */
  std::shared_ptr<sun::types::ClassType> lookupClass(
      const std::string &name) const;

  /** Find a generic class template by name in the scope chain. */
  const GenericClassInfo *lookupGenericClass(const std::string &name) const;

  /** Retrieve an already selected template from the retained scope tree. */
  const GenericClassInfo *lookupGenericClass(
      sun::semantic_analysis::DeclarationId id) const;

  /**
   * Find an interface by name in the scope chain, falling back to the builtin
   * interfaces (IError).
   */
  std::shared_ptr<sun::types::InterfaceType> lookupInterface(
      const std::string &name) const;

  /** Find a generic interface template by name in the scope chain. */
  const GenericInterfaceInfo *lookupGenericInterface(
      const std::string &name) const;

  /** Retrieve an already selected template from the retained scope tree. */
  const GenericInterfaceInfo *lookupGenericInterface(
      sun::semantic_analysis::DeclarationId id) const;

  /** Find an enum by name in the scope chain (null when there is none). */
  std::shared_ptr<sun::types::EnumType> lookupEnum(
      const std::string &name) const;

  /** Find a generic enum template by name in the scope chain. */
  const GenericEnumInfo *lookupGenericEnum(const std::string &name) const;

  /** Retrieve an already selected template from the retained scope tree. */
  const GenericEnumInfo *lookupGenericEnum(
      sun::semantic_analysis::DeclarationId id) const;

  /** Retrieve a module already selected by its declaration identity. */
  SemanticScopeBase *lookupModuleScope(
      sun::semantic_analysis::DeclarationId id) const;

  /** Require the original nominal declaration, without source-name fallback. */
  sun::semantic_analysis::DeclarationId requireDeclaration(
      const sun::semantic_analysis::PortableDeclarationKey &key,
      const std::string &exporter = "",
      std::optional<sun::types::Type::Kind> expectedKind = std::nullopt,
      const std::string &displayName = "") const;

  // ---- Type parameters and aliases ---------------------------------------

  /** The type a type parameter is bound to, searching outwards (null if none).
   */
  sun::types::TypePtr findTypeParameter(const std::string &name) const;

  /** The type a `type` alias names, searching outwards (null if none). */
  sun::types::TypePtr findTypeAlias(const std::string &name) const;

  /** The file whose imports are visible during the current operation. */
  SourceFileId currentSourceFileId() const override { return sourceFileId_; }

  /** Restore source context after declaration analysis or specialization. */
  struct SourceFileGuard {
    SemanticContext &ctx;
    SourceFileId saved;
    /** Changes the active source file and saves the previous source context. */
    SourceFileGuard(SemanticContext &c, SourceFileId id)
        : ctx(c), saved(c.sourceFileId_) {
      if (id) ctx.sourceFileId_ = id;
    }
    /** Restores the source file used to resolve imports and report errors. */
    ~SourceFileGuard() { ctx.sourceFileId_ = saved; }
    /** Changes the active source file and saves the previous source context. */
    SourceFileGuard(const SourceFileGuard &) = delete;
    /** Disallows assignment so ownership and object identity cannot be
     * duplicated. */
    SourceFileGuard &operator=(const SourceFileGuard &) = delete;
  };

  // ---- Scope and location guards -----------------------------------------

  /**
   * Make `target` the current scope for the enclosed block (restored on
   * exit, including by exception). Generic instantiation uses it to analyze
   * a template's body in the scope the template was declared in.
   */
  struct ScopeSwitchGuard {
    SemanticContext &ctx;
    SemanticScope *saved;
    /** Temporarily switches semantic lookup to another scope. */
    ScopeSwitchGuard(SemanticContext &c, SemanticScope *target)
        : ctx(c), saved(c.currentScope_) {
      if (target) ctx.currentScope_ = target;
    }
    /** Restores the semantic scope active before the guard was created. */
    ~ScopeSwitchGuard() { ctx.currentScope_ = saved; }
    /** Temporarily switches semantic lookup to another scope. */
    ScopeSwitchGuard(const ScopeSwitchGuard &) = delete;
    /** Disallows assignment so ownership and object identity cannot be
     * duplicated. */
    ScopeSwitchGuard &operator=(const ScopeSwitchGuard &) = delete;
  };

  /**
   * Record the expression being analyzed for the enclosed block, so an access
   * denial raised deep inside a lookup can still point at source.
   */
  struct LocationGuard {
    SemanticContext &ctx;
    /** Changes the diagnostic source position and saves the previous position.
     */
    LocationGuard(SemanticContext &c, const sun::support::Position &loc)
        : ctx(c) {
      ctx.locationStack_.push_back(&loc);
    }
    /** Restores the source position used for subsequent diagnostics. */
    ~LocationGuard() { ctx.locationStack_.pop_back(); }
    /** Changes the diagnostic source position and saves the previous position.
     */
    LocationGuard(const LocationGuard &) = delete;
    /** Disallows assignment so ownership and object identity cannot be
     * duplicated. */
    LocationGuard &operator=(const LocationGuard &) = delete;
  };

  /** The innermost location a LocationGuard recorded, if any. */
  std::optional<sun::support::Position> currentLocation() const;

  /**
   * The scope a generic template was declared in (nullptr if unknown, in
   * which case ScopeSwitchGuard keeps the current scope).
   */
  template <typename GenericInfo>
  static SemanticScope *definitionScopeOf(const GenericInfo &info) {
    return info.definitionScope.lock().get();
  }

  // ---- Access control ----------------------------------------------------
  //
  // Module-level items are filtered inside the scope lookups (AccessFilter in
  // semantic_scope.h) via the AccessContext this class implements; class and
  // interface members are checked where they are resolved on the type.
  // "Which module is asking" is always the nearest Module scope on the stack:
  // generic bodies are analyzed inside their definition scope (see
  // ScopeSwitchGuard), so no override is needed.

  /** The module asking for access: the nearest module scope on the stack. */
  sun::semantic_analysis::DeclarationId currentModuleId() const override;

  /** Declaration ownership for visibility checks in this session. */
  const sun::semantic_analysis::DeclarationTable &declarationTable()
      const override {
    return results_->declarations;
  }

  /** Report that `item` is not reachable from here, pointing at source. */
  [[noreturn]] void denyAccess(
      const sun::semantic_analysis::ItemRef &item) const override;

  /** Throw at `loc` unless `item` is reachable from the current module. */
  void requireAccessible(const sun::semantic_analysis::ItemRef &item,
                         const sun::support::Position &loc) const {
    sun::semantic_analysis::requireAccessible(currentModuleId(), item, loc,
                                              declarationTable());
  }

  /** The same, pointing at the innermost recorded location. */
  void requireAccessible(const sun::semantic_analysis::ItemRef &item) const {
    if (!isAccessible(item)) denyAccess(item);
  }

  /** True when `item` is reachable from the current module. */
  bool isAccessible(const sun::semantic_analysis::ItemRef &item) const {
    return sun::semantic_analysis::isAccessible(currentModuleId(), item,
                                                declarationTable());
  }

  /**
   * A class field by name: nullptr when it does not exist; throws when it
   * exists but is not accessible from here.
   */
  const sun::types::ClassField *accessibleField(
      const sun::types::ClassType &cls, const std::string &name,
      const sun::support::Position &loc) const;

  /** The same for a class method, taking the first overload of that name. */
  const sun::types::ClassMethod *accessibleMethod(
      const sun::types::ClassType &cls, const std::string &name,
      const sun::support::Position &loc) const;

  /** The same, picking the overload that matches the argument types. */
  const sun::types::ClassMethod *accessibleMethodForArgs(
      const sun::types::ClassType &cls, const std::string &name,
      const std::vector<sun::types::TypePtr> &argTypes,
      const sun::support::Position &loc) const;

  /** An interface field by name, with the same access rules as a class's. */
  const sun::types::InterfaceField *accessibleField(
      const sun::types::InterfaceType &iface, const std::string &name,
      const sun::support::Position &loc) const;

  /** An interface method by name, with the same access rules as a class's. */
  const sun::types::InterfaceMethod *accessibleMethod(
      const sun::types::InterfaceType &iface, const std::string &name,
      const sun::support::Position &loc) const;

  /**
   * A module named by user code (`a.b`, `using a.b;`, `b.f()`): every
   * module on its path must be visible from here.
   */
  void requireModuleAccessible(const SemanticScopeBase &moduleScope,
                               const sun::support::Position &loc) const;

 private:
  /** Register built-in functions (print, println, file I/O, etc.). */
  void registerBuiltinFunctions();

  // See results(). Shared with codegen, which reads what analysis concludes.
  std::shared_ptr<AnalysisResults> results_;

  // Scope tree — rootScope_ is the global scope, currentScope_ walks the tree
  std::shared_ptr<GlobalScope> rootScope_ = std::make_shared<GlobalScope>();
  SemanticScope *currentScope_ = rootScope_.get();

  // Current class being analyzed (for 'this' resolution)
  std::shared_ptr<sun::types::ClassType> currentClass_ = nullptr;

  // Locations of the expressions being analyzed (innermost last), so denials
  // raised inside lookups can still point at source.
  std::vector<const sun::support::Position *> locationStack_;

  // How many MoonScopeAST wrappers we are inside (see isInMoonScope)
  int moonScopeDepth_ = 0;
};

}  // namespace sun::semantic_analysis
