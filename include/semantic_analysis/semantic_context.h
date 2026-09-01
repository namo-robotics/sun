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

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "semantic_analysis/access_checker.h"
#include "semantic_analysis/semantic_scope.h"

struct Position;

/**
 * The shared state and scope machinery of a semantic analysis run. Implements
 * AccessContext so the scope lookups can filter by visibility: "which module
 * is asking" is the nearest module scope on the stack.
 */
class SemanticContext : public AccessContext {
 public:
  /** Start with an empty global scope holding the builtin functions. */
  explicit SemanticContext(std::shared_ptr<sun::TypeRegistry> registry);

  // ---- Shared state ------------------------------------------------------

  /** Class and interface types, shared with codegen. */
  const std::shared_ptr<sun::TypeRegistry> &types() const {
    return typeRegistry_;
  }

  /** The scope currently being analyzed. */
  SemanticScope *scope() const { return currentScope_; }

  /** The global scope, for debugging and visualization. */
  const SemanticScope &rootScope() const { return *rootScope_; }

  /** Set the class whose body is being analyzed, so `this` resolves to it. */
  void setCurrentClass(std::shared_ptr<sun::ClassType> classType);

  /** The class whose body is being analyzed, or null outside one. */
  std::shared_ptr<sun::ClassType> getCurrentClass() const;

  // ---- Scope navigation --------------------------------------------------

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

  /**
   * Enter (creating or reusing) a module scope and record its visibility;
   * re-openings must agree.
   */
  void declareModule(const ModuleAST &module);

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
   * Make the parent scope current again. The scope itself stays in the tree
   * for debugging and visualization.
   */
  void exitScope();

  /** True when not inside any function scope (i.e. at module/global level). */
  bool isAtModuleLevel() const {
    for (auto *s = currentScope_; s != nullptr; s = s->parent)
      if (s->getType() == ScopeType::Function) return false;
    return true;
  }

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
  sun::TypePtr currentFunctionReturnType() const;

  // ---- Names and module paths -------------------------------------------

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
   * e.g., inside "module std { }", qualifyName("Vec") returns "sun_Vec".
   */
  std::string qualifyNameInCurrentModule(const std::string &name) const;

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
      const std::vector<sun::TypePtr> *argTypes = nullptr) const;

  /**
   * Resolve a bare name against the `using` imports in scope, giving the
   * qualified name it refers to (the name itself when nothing matches).
   */
  sun::QualifiedName resolveNameWithUsings(const std::string &name) const;

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

  /** Record a variable in the current scope under the given type. */
  void declareVariable(const std::string &name, sun::TypePtr type,
                       bool isParam = false, bool isConst = false);

  /** Find a variable by name in the scope chain. */
  VariableInfo *lookupVariable(const std::string &name);

  /**
   * Record a module-level variable under both its plain and its qualified
   * name, so it can be reached from inside the module and from outside it.
   */
  void registerModuleVariable(const std::string &baseName,
                              const std::string &qualifiedName,
                              sun::TypePtr type, sun::Visibility visibility,
                              bool isConst = false,
                              bool isCExtern = false);

  /**
   * Find a variable by its dotted name (`a.b.x`). An undotted name is an
   * ordinary variable lookup.
   */
  VariableInfo *lookupQualifiedVariable(const std::string &qualifiedName);

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

  // ---- Functions ---------------------------------------------------------

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

  /** Find a function by its dotted name (`a.b.f`); undotted names never match.
   */
  const FunctionInfo *lookupQualifiedFunction(
      const std::string &qualifiedName) const;

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

  // ---- Types: classes, interfaces, enums and their templates -------------

  /**
   * Record a class in the current scope. A repeated registration of the same
   * name is ignored, which is what a diamond import produces.
   */
  void registerClass(const std::string &name,
                     std::shared_ptr<sun::ClassType> classType,
                     std::optional<Position> loc = std::nullopt);

  /** Find a class by name in the scope chain (null when there is none). */
  std::shared_ptr<sun::ClassType> lookupClass(const std::string &name) const;

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

  /** Record an enum in the current scope. */
  void registerEnum(const std::string &name,
                    std::shared_ptr<sun::EnumType> enumType);

  /** Find an enum by name in the scope chain (null when there is none). */
  std::shared_ptr<sun::EnumType> lookupEnum(const std::string &name) const;

  /**
   * Record a generic enum template in the current scope, along with the scope
   * it was declared in.
   */
  void registerGenericEnum(const std::string &name, GenericEnumInfo info);

  /** Find a generic enum template by name in the scope chain. */
  const GenericEnumInfo *lookupGenericEnum(const std::string &name) const;

  // ---- Type parameters and aliases ---------------------------------------

  /** Bind type parameter names to concrete types in the current scope. */
  void addTypeParameterBindings(const std::vector<std::string> &params,
                                const std::vector<sun::TypePtr> &args);

  /** The type a type parameter is bound to, searching outwards (null if none).
   */
  sun::TypePtr findTypeParameter(const std::string &name) const;

  /** The type a `type` alias names, searching outwards (null if none). */
  sun::TypePtr findTypeAlias(const std::string &name) const;

  // ---- Scope and location guards -----------------------------------------

  /**
   * Make `target` the current scope for the enclosed block (restored on
   * exit, including by exception). Generic instantiation uses it to analyze
   * a template's body in the scope the template was declared in.
   */
  struct ScopeSwitchGuard {
    SemanticContext &ctx;
    SemanticScope *saved;
    ScopeSwitchGuard(SemanticContext &c, SemanticScope *target)
        : ctx(c), saved(c.currentScope_) {
      if (target) ctx.currentScope_ = target;
    }
    ~ScopeSwitchGuard() { ctx.currentScope_ = saved; }
    ScopeSwitchGuard(const ScopeSwitchGuard &) = delete;
    ScopeSwitchGuard &operator=(const ScopeSwitchGuard &) = delete;
  };

  /**
   * Record the expression being analyzed for the enclosed block, so an access
   * denial raised deep inside a lookup can still point at source.
   */
  struct LocationGuard {
    SemanticContext &ctx;
    LocationGuard(SemanticContext &c, const Position &loc) : ctx(c) {
      ctx.locationStack_.push_back(&loc);
    }
    ~LocationGuard() { ctx.locationStack_.pop_back(); }
    LocationGuard(const LocationGuard &) = delete;
    LocationGuard &operator=(const LocationGuard &) = delete;
  };

  /** The innermost location a LocationGuard recorded, if any. */
  std::optional<Position> currentLocation() const;

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

  // ---- Access control ----------------------------------------------------
  //
  // Module-level items are filtered inside the scope lookups (AccessFilter in
  // semantic_scope.h) via the AccessContext this class implements; class and
  // interface members are checked where they are resolved on the type.
  // "Which module is asking" is always the nearest Module scope on the stack:
  // generic bodies are analyzed inside their definition scope (see
  // ScopeSwitchGuard), so no override is needed.

  /** The module asking for access: the nearest module scope on the stack. */
  sun::ModulePath currentModulePath() const override;

  /** Report that `item` is not reachable from here, pointing at source. */
  [[noreturn]] void denyAccess(const sun::access::ItemRef &item) const override;

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

 private:
  /** Register built-in functions (print, println, file I/O, etc.). */
  void registerBuiltinFunctions();

  // Type registry for class/interface types (shared with codegen)
  std::shared_ptr<sun::TypeRegistry> typeRegistry_;

  // Scope tree — rootScope_ is the global scope, currentScope_ walks the tree
  std::shared_ptr<GlobalScope> rootScope_ = std::make_shared<GlobalScope>();
  SemanticScope *currentScope_ = rootScope_.get();

  // Current class being analyzed (for 'this' resolution)
  std::shared_ptr<sun::ClassType> currentClass_ = nullptr;

  // Locations of the expressions being analyzed (innermost last), so denials
  // raised inside lookups can still point at source.
  std::vector<const Position *> locationStack_;

  // How many MoonScopeAST wrappers we are inside (see isInMoonScope)
  int moonScopeDepth_ = 0;
};
