// semantic_context.cpp — The shared state and scope machinery of a semantic
// analysis run (see semantic_context.h)

#include "semantic_analysis/semantic_context.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/symbol_names.h"
#include "support/error.h"

using sun::unwrapRef;
using sun::access::fieldRef;
using sun::access::methodRef;
using sun::access::moduleRef;
using sun::names::getFunctionSignature;
using sun::names::isReservedIdentifier;

// isLibraryScope() and mangleModulePath() are provided by semantic_scope.h

SemanticContext::SemanticContext(std::shared_ptr<sun::TypeRegistry> registry)
    : typeRegistry_(std::move(registry)) {
  rootScope_->accessContext = this;  // lookups filter by visibility
  registerBuiltinFunctions();
}

std::optional<Position> SemanticContext::currentLocation() const {
  if (locationStack_.empty()) return std::nullopt;
  return *locationStack_.back();
}

// -------------------------------------------------------------------
// Scope navigation
// -------------------------------------------------------------------

void SemanticContext::enterTypeParamScope(
    const std::vector<std::string>& params,
    const std::vector<sun::TypePtr>& args) {
  enterScope(ScopeType::TypeParams);
  addTypeParameterBindings(params, args);
}

void SemanticContext::enterScope(ScopeType type) {
  std::shared_ptr<SemanticScopeBase> child;
  switch (type) {
    case ScopeType::Global:
      child = std::make_shared<GlobalScope>();
      break;
    case ScopeType::Module:
      child = std::make_shared<ModuleScope>();
      break;
    case ScopeType::Import:
      child = std::make_shared<ImportScope>();
      break;
    case ScopeType::Function:
      child = std::make_shared<FunctionScope>();
      break;
    case ScopeType::Class:
      child = std::make_shared<ClassScope>();
      break;
    case ScopeType::Interface:
      child = std::make_shared<InterfaceScope>();
      break;
    case ScopeType::Block: {
      auto block = std::make_shared<BlockScope>();
      // Inherit unsafe context only for block scopes
      block->inUnsafeContext = currentScope_->inUnsafeContext;
      child = block;
      break;
    }
    case ScopeType::TypeParams:
      child = std::make_shared<TypeParamsScope>();
      break;
  }
  child->parent = currentScope_;
  currentScope_->children.push_back(child);
  currentScope_ = child.get();
}

void SemanticContext::enterModuleScope(const std::string& moduleName) {
  // Create or reuse a child module scope in the current scope's tree
  auto& child = currentScope_->childModules[moduleName];
  if (!child) {
    auto modScope = std::make_shared<ModuleScope>();
    modScope->scopeName = moduleName;
    modScope->parent = currentScope_;
    // Compute full scope path by extending parent's path
    modScope->scopePath = currentScope_->scopePath;
    modScope->scopePath.push_back(moduleName);
    modScope->qualifiedName = sun::QualifiedName(
        currentScope_->scopePath, moduleName, currentScope_->scopePath);
    child = modScope;
  }
  currentScope_ = child.get();
}

void SemanticContext::declareModule(const ModuleAST& module) {
  enterModuleScope(module.getName());
  auto* scope = static_cast<ModuleScope*>(currentScope_);
  if (scope->visibilityDeclared &&
      scope->visibility != module.getVisibility()) {
    logSemanticError(
        "module '" + module.getName() + "' was previously declared " +
            sun::visibilityKeyword(scope->visibility) +
            "; all declarations of a module must agree on its visibility",
        module.getLocation());
  }
  scope->visibility = module.getVisibility();
  scope->visibilityDeclared = true;
}

void SemanticContext::enterClassScope(const sun::QualifiedName& className) {
  auto classScope = std::make_shared<ClassScope>();
  classScope->classBaseName = className.baseName;
  classScope->classMangledName = className.mangled();
  // Use the class's scope path directly
  classScope->scopePath = className.scopePath;
  classScope->scopePath.push_back(className.baseName);
  classScope->parent = currentScope_;
  currentScope_->children.push_back(classScope);
  currentScope_ = classScope.get();
}

void SemanticContext::enterInterfaceScope(
    const sun::QualifiedName& interfaceName) {
  auto ifaceScope = std::make_shared<InterfaceScope>();
  ifaceScope->interfaceBaseName = interfaceName.baseName;
  ifaceScope->interfaceMangledName = interfaceName.mangled();
  // Use the interface's scope path directly
  ifaceScope->scopePath = interfaceName.scopePath;
  ifaceScope->scopePath.push_back(interfaceName.baseName);
  ifaceScope->parent = currentScope_;
  currentScope_->children.push_back(ifaceScope);
  currentScope_ = ifaceScope.get();
}

void SemanticContext::enterFunctionScope(const std::string& funcSig,
                                         const sun::QualifiedName& funcName,
                                         bool canThrow,
                                         sun::TypePtr returnType) {
  auto funcScope = std::make_shared<FunctionScope>();
  funcScope->functionSignature = funcSig;
  funcScope->functionName = funcName;
  funcScope->functionCanThrow = canThrow;
  funcScope->functionReturnType = std::move(returnType);
  funcScope->parent = currentScope_;

  // Set scopePath to include the function's mangled name so nested functions
  // get unique qualified names (e.g., inner inside outer_i32 ->
  // outer_i32_inner)
  funcScope->scopePath = {funcName.mangled()};

  currentScope_->children.push_back(funcScope);
  currentScope_ = funcScope.get();
}

sun::TypePtr SemanticContext::currentFunctionReturnType() const {
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    if (s->getType() == ScopeType::Function) {
      return static_cast<const FunctionScope*>(s)->functionReturnType;
    }
  }
  return nullptr;
}

void SemanticContext::exitScope() {
  if (currentScope_->parent) {
    auto* parent = currentScope_->parent;
    // Keep all scopes in the tree for debugging/visualization.
    // Symbol lookups already don't descend into Function scopes.
    currentScope_ = parent;
  }
}

std::string SemanticContext::getCurrentModulePrefix() const {
  // Get module path from scope and mangle it for symbol prefixing
  auto scopePath = getCurrentScopePath();
  if (scopePath.empty()) return "";

  // Join path segments with underscores for mangled name prefix
  std::string result;
  for (const auto& seg : scopePath) {
    if (!result.empty()) result += "_";
    result += seg;
  }
  return result + "_";
}

std::vector<std::string> SemanticContext::getCurrentScopePath() const {
  // Walk up to find the nearest scope with a scopePath (Module or Import).
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    if (!s->scopePath.empty()) {
      return s->scopePath;
    }
  }
  return {};
}

sun::QualifiedName SemanticContext::makeQualifiedName(
    const std::string& baseName) const {
  return sun::QualifiedName(getCurrentScopePath(), baseName,
                            currentModulePath());
}

std::string SemanticContext::qualifyNameInCurrentModule(
    const std::string& name) const {
  std::string prefix = getCurrentModulePrefix();
  return prefix + name;
}

bool SemanticContext::isInThrowingFunction() const {
  // Find the nearest enclosing function scope and check if it can throw
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    if (auto* funcScope = s->asFunction()) {
      return funcScope->functionCanThrow;
    }
  }
  return false;
}

bool SemanticContext::isInTryBlock() const {
  // Check if any scope in the chain has tryBlockDepth > 0
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    if (s->tryBlockDepth > 0) {
      return true;
    }
    // Stop at function boundary - try blocks don't cross functions
    if (s->getType() == ScopeType::Function) {
      break;
    }
  }
  return false;
}

void SemanticContext::enterTryBlock() {
  if (currentScope_) {
    currentScope_->tryBlockDepth++;
  }
}

void SemanticContext::exitTryBlock() {
  if (currentScope_ && currentScope_->tryBlockDepth > 0) {
    currentScope_->tryBlockDepth--;
  }
}

bool SemanticContext::isInUnsafeBlock() const {
  return currentScope_ && currentScope_->inUnsafeContext;
}

void SemanticContext::enterUnsafeBlock() {
  if (currentScope_) {
    currentScope_->unsafeBlockDepth++;
    currentScope_->inUnsafeContext = true;
  }
}

void SemanticContext::exitUnsafeBlock() {
  if (currentScope_ && currentScope_->unsafeBlockDepth > 0) {
    currentScope_->unsafeBlockDepth--;
    if (currentScope_->unsafeBlockDepth == 0) {
      // Restore based on parent (if we inherited from parent, stay in unsafe)
      // For function/module/import scopes, parent's context doesn't matter
      currentScope_->inUnsafeContext =
          currentScope_->parent &&
          currentScope_->getType() == ScopeType::Block &&
          currentScope_->parent->inUnsafeContext;
    }
  }
}

bool SemanticContext::isModuleName(const std::string& name) const {
  return currentScope_->isModuleName(name);
}

SemanticScope* SemanticContext::lookupModuleScope(
    const std::string& dotPath) const {
  if (!currentScope_) return nullptr;
  return currentScope_->lookupModuleScope(dotPath);
}

std::vector<UsingImport> SemanticContext::getActiveUsingImports() const {
  return currentScope_->getActiveUsingImports();
}

// -------------------------------------------------------------------
// Unified Symbol Lookup
// Library scopes ($hash$) are transparent - we look through them
// Throws on ambiguity (same name in multiple library scopes)
// -------------------------------------------------------------------

// Helper: collect ALL module scopes matching a path across import scopes
// and using statements. This handles the case where two .sun imports define
// the same module name, or where a module is brought in via `using`.
// Also collects parent module scopes (e.g., for path "A.B", also collects "A").
static std::vector<SemanticScope*> collectAllModuleScopes(
    const SemanticScope* startScope, const std::string& dotPath) {
  std::vector<SemanticScope*> results;
  if (dotPath.empty() || !startScope) return results;

  // Helper to add a scope if not already present
  auto addUnique = [&results](SemanticScope* scope) {
    if (std::find(results.begin(), results.end(), scope) == results.end()) {
      results.push_back(scope);
    }
  };

  // Track visited scopes to prevent infinite recursion
  std::set<const SemanticScope*> visitedScopes;

  // Helper lambda to find ALL scopes for a segment (not just the first)
  std::function<void(const SemanticScope&, const std::string&,
                     std::vector<SemanticScope*>&)>
      findAllInScope = [&](const SemanticScope& scope,
                           const std::string& segment,
                           std::vector<SemanticScope*>& out) {
        // Skip function signatures (segments with parentheses)
        if (segment.find('(') != std::string::npos) return;
        // Prevent infinite recursion by tracking visited scopes
        if (visitedScopes.count(&scope)) return;
        visitedScopes.insert(&scope);

        // Direct child lookup
        auto it = scope.childModules.find(segment);
        if (it != scope.childModules.end() && !isLibraryScope(segment)) {
          out.push_back(it->second.get());
        }

        // Search inside library/import scopes
        for (const auto& [modName, child] : scope.childModules) {
          if (!child || !isLibraryScope(modName)) continue;
          auto childIt = child->childModules.find(segment);
          if (childIt != child->childModules.end()) {
            out.push_back(childIt->second.get());
          }
          // Recursively check nested library scopes
          findAllInScope(*child, segment, out);
        }
      };

  // Helper to collect scopes for a given path
  auto collectForPath = [&](const std::string& path) {
    std::vector<SemanticScope*> currentScopes;

    // Start from each ancestor scope
    for (auto* s = startScope; s != nullptr; s = s->parent) {
      std::string segment;
      std::istringstream stream(path);
      bool firstSegment = true;
      std::vector<SemanticScope*> segScopes;

      while (std::getline(stream, segment, '.')) {
        segScopes.clear();
        if (firstSegment) {
          findAllInScope(*s, segment, segScopes);
          firstSegment = false;
        } else {
          // For subsequent segments, search within previously found scopes
          std::vector<SemanticScope*> nextScopes;
          for (auto* prevScope : currentScopes) {
            auto it2 = prevScope->childModules.find(segment);
            if (it2 != prevScope->childModules.end()) {
              nextScopes.push_back(it2->second.get());
            }
          }
          segScopes = std::move(nextScopes);
        }
        currentScopes = segScopes;
      }

      if (!currentScopes.empty()) {
        for (auto* scope : currentScopes) {
          addUnique(scope);
        }
      }

      // Also check ImportBindings for matching module scopes (from using
      // statements) Compare the binding's source scope's scopeName against
      // path
      for (const auto& binding : s->importBindings) {
        if (!binding.sourceScope || !binding.isWildcard) continue;
        // scopeName is the simple name like "sun", not the full path with
        // import prefixes
        if (binding.sourceScope->scopeName == path) {
          addUnique(binding.sourceScope);
        }
      }
    }
  };

  // Collect scopes for the full path
  collectForPath(dotPath);

  // Also collect parent module scopes (e.g., for "A.B", also collect "A")
  // This handles nested modules where a symbol is defined in a parent module
  std::string parentPath = dotPath;
  size_t lastDot = parentPath.rfind('.');
  while (lastDot != std::string::npos) {
    parentPath = parentPath.substr(0, lastDot);
    collectForPath(parentPath);
    lastDot = parentPath.rfind('.');
  }

  return results;
}

SymbolMatch SemanticContext::findSymbolInModule(
    const std::string& modulePath, const std::string& name,
    SymbolKind filterKind, const std::vector<sun::TypePtr>* argTypes) const {
  // Helper to extract visible module path by stripping $...$ prefixes
  auto getVisibleModulePath = [](const std::string& path) -> std::string {
    std::string result;
    size_t pos = 0;
    while (pos < path.size()) {
      size_t dot = path.find('.', pos);
      std::string segment = (dot == std::string::npos)
                                ? path.substr(pos)
                                : path.substr(pos, dot - pos);
      // Skip segments starting with $ (import scopes, library hashes)
      if (!segment.empty() && segment[0] != '$') {
        if (!result.empty()) result += ".";
        result += segment;
      }
      pos = (dot == std::string::npos) ? path.size() : dot + 1;
    }
    return result;
  };

  // Get visible module path for searching across all matching scopes
  std::string visiblePath = getVisibleModulePath(modulePath);

  // Access filtering: private symbols of other modules are skipped; if that
  // leaves nothing, the denial is reported instead of "unknown member".
  AccessFilter accessFilter(currentScope_);

  // Helper to search a single scope for all symbol types
  auto searchInScope = [&](SemanticScope* scope) -> std::optional<SymbolMatch> {
    std::string fullPath = sun::QualifiedName::joinPath(scope->scopePath);
    std::string libHash;
    if (!scope->scopePath.empty() && scope->scopePath[0].size() >= 2 &&
        scope->scopePath[0].front() == '$') {
      libHash = scope->scopePath[0];
    }

    auto matchesFilter = [filterKind](SymbolKind kind) {
      return filterKind == SymbolKind::None || filterKind == kind;
    };

    // Check classes
    if (matchesFilter(SymbolKind::Class)) {
      auto classIt = scope->classes.find(name);
      if (classIt != scope->classes.end()) {
        SymbolMatch match;
        match.kind = SymbolKind::Class;
        match.name = name;
        match.modulePath = fullPath;
        match.libraryHash = libHash;
        match.classType = classIt->second;
        if (!accessFilter.admit(classIt->second)) return std::nullopt;
        return match;
      }
    }

    // Check generic classes
    if (matchesFilter(SymbolKind::GenericClass)) {
      auto genClassIt = scope->genericClasses.find(name);
      if (genClassIt != scope->genericClasses.end()) {
        SymbolMatch match;
        match.kind = SymbolKind::GenericClass;
        match.name = name;
        match.modulePath = fullPath;
        match.libraryHash = libHash;
        match.genericClassInfo = &genClassIt->second;
        if (!accessFilter.admit(&genClassIt->second)) return std::nullopt;
        return match;
      }
    }

    // Check interfaces
    if (matchesFilter(SymbolKind::Interface)) {
      auto ifaceIt = scope->interfaces.find(name);
      if (ifaceIt != scope->interfaces.end()) {
        SymbolMatch match;
        match.kind = SymbolKind::Interface;
        match.name = name;
        match.modulePath = fullPath;
        match.libraryHash = libHash;
        match.interfaceType = ifaceIt->second;
        if (!accessFilter.admit(ifaceIt->second)) return std::nullopt;
        return match;
      }
    }

    // Check generic interfaces
    if (matchesFilter(SymbolKind::GenericInterface)) {
      auto genIfaceIt = scope->genericInterfaces.find(name);
      if (genIfaceIt != scope->genericInterfaces.end()) {
        SymbolMatch match;
        match.kind = SymbolKind::GenericInterface;
        match.name = name;
        match.modulePath = fullPath;
        match.libraryHash = libHash;
        match.genericInterfaceInfo = &genIfaceIt->second;
        if (!accessFilter.admit(&genIfaceIt->second)) return std::nullopt;
        return match;
      }
    }

    // Check enums
    if (matchesFilter(SymbolKind::Enum)) {
      auto enumIt = scope->enums.find(name);
      if (enumIt != scope->enums.end()) {
        SymbolMatch match;
        match.kind = SymbolKind::Enum;
        match.name = name;
        match.modulePath = fullPath;
        match.libraryHash = libHash;
        match.enumType = enumIt->second;
        if (!accessFilter.admit(enumIt->second)) return std::nullopt;
        return match;
      }
    }

    // Check functions
    if (matchesFilter(SymbolKind::Function)) {
      if (auto* overloads = scope->functions.getOverloads(name)) {
        if (!overloads->empty()) {
          const FunctionInfo* info = overloads->front();
          // When the caller knows the argument types, pick the overload that
          // actually matches rather than whichever was registered first.
          if (argTypes) {
            auto resolved =
                scope->lookupFunctionLocal(name, *argTypes, &accessFilter);
            if (!resolved) return std::nullopt;
            info = nullptr;
            for (const auto* candidate : *overloads) {
              if (candidate->qualifiedName == resolved->qualifiedName) {
                info = candidate;
                break;
              }
            }
            if (!info) return std::nullopt;
          }
          if (!argTypes) {
            // First accessible overload
            info = nullptr;
            for (const auto* candidate : *overloads) {
              if (accessFilter.admit(candidate)) {
                info = candidate;
                break;
              }
            }
            if (!info) return std::nullopt;
          }
          SymbolMatch match;
          match.kind = SymbolKind::Function;
          match.name = name;
          match.modulePath = fullPath;
          match.libraryHash = libHash;
          match.functionInfo = info;
          return match;
        }
      }
    }

    // Check generic functions (templates, keyed by qualified name)
    if (matchesFilter(SymbolKind::GenericFunction)) {
      auto genFuncIt = scope->genericFunctions.find({scope->scopePath, name});
      if (genFuncIt != scope->genericFunctions.end()) {
        SymbolMatch match;
        match.kind = SymbolKind::GenericFunction;
        match.name = name;
        match.modulePath = fullPath;
        match.libraryHash = libHash;
        match.genericFunctionInfo = &genFuncIt->second;
        if (!accessFilter.admit(&genFuncIt->second)) return std::nullopt;
        return match;
      }
    }

    // Check namespaced variables
    if (matchesFilter(SymbolKind::Variable)) {
      std::string mangledPath = mangleModulePath(fullPath);
      std::string qualifiedVarName = mangledPath + "_" + name;
      auto varIt = scope->namespacedVariables.find(qualifiedVarName);
      if (varIt != scope->namespacedVariables.end()) {
        SymbolMatch match;
        match.kind = SymbolKind::Variable;
        match.name = name;
        match.modulePath = fullPath;
        match.libraryHash = libHash;
        match.variableInfo = &varIt->second;
        if (!accessFilter.admit(&varIt->second)) return std::nullopt;
        return match;
      }
    }

    return std::nullopt;
  };

  // Search ALL module scopes with the same visible name
  // This handles same-named modules in different import scopes
  // Track all matches to detect version conflicts (same symbol in multiple
  // scopes)
  std::vector<SymbolMatch> allMatches;
  if (!visiblePath.empty()) {
    auto allScopes = collectAllModuleScopes(currentScope_, visiblePath);
    for (auto* scope : allScopes) {
      if (auto match = searchInScope(scope)) {
        allMatches.push_back(*match);
      }
    }
  }

  // Also check the specific full path (for direct lookups)
  std::string fullPath = getFullModulePath(modulePath);
  SemanticScope* modScope = lookupModuleScope(fullPath);
  if (modScope) {
    if (auto match = searchInScope(modScope)) {
      // Only add if not already found (avoid duplicate from same scope)
      bool alreadyFound = false;
      for (const auto& m : allMatches) {
        if (m.modulePath == match->modulePath) {
          alreadyFound = true;
          break;
        }
      }
      if (!alreadyFound) {
        allMatches.push_back(*match);
      }
    }
  }

  // Check for ambiguity: same symbol found in multiple different scopes
  if (allMatches.size() > 1) {
    std::string paths;
    for (const auto& m : allMatches) {
      if (!paths.empty()) paths += " or ";
      paths += getVisibleModulePath(m.modulePath);
    }
    logAndThrowError("Ambiguous reference to '" + visiblePath + "." + name +
                     "'. Could be: " + paths);
  }

  // Return single match if found
  if (allMatches.size() == 1) {
    return allMatches[0];
  }

  accessFilter.finish();
  return SymbolMatch{};
}

// -------------------------------------------------------------------
// Variable management
// -------------------------------------------------------------------

void SemanticContext::declareVariable(const std::string& name,
                                      sun::TypePtr type, bool isParam,
                                      bool isConst) {
  // Block user-defined identifiers starting with underscore
  if (isReservedIdentifier(name)) {
    logAndThrowError(
        "Identifier '" + name +
        "' is invalid: names starting with '_' are reserved for builtins");
  }
  // Check for shadowing of global/module variables
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    if (s->getType() == ScopeType::Global ||
        s->getType() == ScopeType::Module) {
      if (s->variables.contains(name)) {
        logAndThrowError("Cannot shadow " +
                         std::string(s->getType() == ScopeType::Global
                                         ? "global"
                                         : "module") +
                         " variable '" + name + "'");
      }
    }
  }
  VariableInfo info{type, isAtModuleLevel(), isParam, false};
  info.isConst = isConst;
  currentScope_->variables[name] = info;
}

VariableInfo* SemanticContext::lookupVariable(const std::string& name) {
  return currentScope_->lookupVariable(name);
}

// -------------------------------------------------------------------
// Type narrowing (from _is<T> type guards)
// -------------------------------------------------------------------

void SemanticContext::narrowVariable(const std::string& varName,
                                     sun::TypePtr narrowedType) {
  currentScope_->narrowedTypes[varName] = std::move(narrowedType);
}

sun::TypePtr SemanticContext::getNarrowedType(const std::string& varName,
                                              sun::TypePtr originalType) const {
  // Search from innermost to outermost scope
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    auto found = s->narrowedTypes.find(varName);
    if (found != s->narrowedTypes.end()) {
      sun::TypePtr narrowedType = found->second;

      // Return the MORE SPECIFIC type between originalType and narrowedType.
      // Specificity order: Class > Interface > TypeParameter
      if (!originalType) {
        return narrowedType;
      }

      // TypeParameter -> anything concrete is more specific
      if (originalType->isTypeParameter()) {
        return narrowedType;
      }

      // Interface -> Class that implements it is more specific
      if (originalType->isInterface() && narrowedType->isClass()) {
        auto* classType = static_cast<sun::ClassType*>(narrowedType.get());
        auto* ifaceType = static_cast<sun::InterfaceType*>(originalType.get());
        // Check if class implements the interface
        for (const auto& impl : classType->getImplementedInterfaces()) {
          if (impl == ifaceType->getName() ||
              impl.rfind(ifaceType->getName() + "_", 0) == 0) {
            return narrowedType;  // Class is more specific
          }
        }
      }

      // Class -> Interface: Class is more specific, return the class
      if (originalType->isClass() && narrowedType->isInterface()) {
        auto* classType = static_cast<sun::ClassType*>(originalType.get());
        auto* ifaceType = static_cast<sun::InterfaceType*>(narrowedType.get());
        // Check if class implements the interface
        for (const auto& impl : classType->getImplementedInterfaces()) {
          if (impl == ifaceType->getName() ||
              impl.rfind(ifaceType->getName() + "_", 0) == 0) {
            return originalType;  // Class is more specific
          }
        }
      }

      // Interface -> more specific Interface (TODO: interface inheritance)
      // For now, don't narrow interface to interface

      // Not a valid narrowing - return nullptr
      return nullptr;
    }
  }
  return nullptr;
}

// -------------------------------------------------------------------
// Function registration
// -------------------------------------------------------------------

void SemanticContext::registerFunctionInCurrentScope(const std::string& name,
                                                     const FunctionInfo& info) {
  // Functions are registered in their enclosing scope. For nested functions,
  // this is the parent function's scope - the scope hierarchy naturally
  // disambiguates between different generic instantiations.
  std::string sig = getFunctionSignature(name, info.paramTypes);
  // Always overwrite: the declaration pre-pass registers with minimal info
  // (no captures), and the normal pass overwrites with complete info.
  // This also handles diamond import re-registration gracefully.
  currentScope_->functions[sig] = info;
}

void SemanticContext::registerGenericFunctionInCurrentScope(FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  GenericFunctionInfo genInfo;
  genInfo.AST = &func;
  genInfo.typeParameters = proto.getTypeParameters();
  if (proto.hasReturnType()) {
    genInfo.returnType = *proto.getReturnType();
  }
  genInfo.params = proto.getArgs();

  sun::QualifiedName qname(getCurrentScopePath(), proto.getName(),
                           currentModulePath());
  genInfo.qualifiedName = qname;
  genInfo.definitionScope = currentScope_->shared_from_this();
  currentScope_->genericFunctions[qname] = genInfo;
}

const GenericFunctionInfo* SemanticContext::lookupGenericFunction(
    const std::string& name) const {
  return currentScope_->lookupGenericFunction(name);
}

std::vector<FunctionInfo> SemanticContext::getAllFunctions(
    const std::string& name) const {
  return currentScope_->getAllFunctions(name);
}

std::optional<FunctionInfo> SemanticContext::lookupFunction(
    const std::string& name, const std::vector<sun::TypePtr>& argTypes) const {
  return currentScope_->lookupFunction(name, argTypes);
}

// -------------------------------------------------------------------
// Builtin function registration
// -------------------------------------------------------------------

void SemanticContext::registerBuiltinFunctions() {
  using sun::Types;

  // Low-level print intrinsics (used by stdlib print functions)
  registerFunctionInCurrentScope("_print_i32",
                                 {Types::Void(), {Types::Int32()}, {}});
  registerFunctionInCurrentScope("_print_i64",
                                 {Types::Void(), {Types::Int64()}, {}});
  registerFunctionInCurrentScope("_print_f64",
                                 {Types::Void(), {Types::Float64()}, {}});
  registerFunctionInCurrentScope("_print_newline", {Types::Void(), {}, {}});
  // _print_char intrinsic: write one char as UTF-8
  registerFunctionInCurrentScope("_print_char",
                                 {Types::Void(), {Types::Char()}, {}});
  // _print_bytes intrinsic: write raw bytes to stdout
  registerFunctionInCurrentScope(
      "_print_bytes",
      {Types::Void(), {Types::RawPointer(Types::Int8()), Types::Int64()}, {}});
  // _println_str: print string literal with newline
  registerFunctionInCurrentScope("_println_str",
                                 {Types::Void(), {Types::String()}, {}});
  registerFunctionInCurrentScope(
      "_println_str", {Types::Void(), {Types::RawPointer(Types::UInt8())}, {}});

  // File I/O intrinsics
  registerFunctionInCurrentScope(
      "__file_open", {Types::Int32(), {Types::String(), Types::Int32()}, {}});
  registerFunctionInCurrentScope("__file_close",
                                 {Types::Int32(), {Types::Int32()}, {}});
  registerFunctionInCurrentScope(
      "__file_write", {Types::Int32(), {Types::Int32(), Types::String()}, {}});
  registerFunctionInCurrentScope(
      "__file_read",
      {Types::RawPointer(Types::Int8()), {Types::Int32(), Types::Int32()}, {}});

  // Extended file I/O intrinsics
  registerFunctionInCurrentScope(
      "__lseek",
      {Types::Int64(), {Types::Int32(), Types::Int64(), Types::Int32()}, {}});
  registerFunctionInCurrentScope(
      "__fstat",
      {Types::Int32(), {Types::Int32(), Types::RawPointer(Types::Int8())}, {}});
  registerFunctionInCurrentScope("__fsync",
                                 {Types::Int32(), {Types::Int32()}, {}});
  registerFunctionInCurrentScope(
      "__ftruncate", {Types::Int32(), {Types::Int32(), Types::Int64()}, {}});
  registerFunctionInCurrentScope("__unlink",
                                 {Types::Int32(), {Types::String()}, {}});
  registerFunctionInCurrentScope(
      "__rename", {Types::Int32(), {Types::String(), Types::String()}, {}});
  registerFunctionInCurrentScope(
      "__mkdir", {Types::Int32(), {Types::String(), Types::Int32()}, {}});
  registerFunctionInCurrentScope("__rmdir",
                                 {Types::Int32(), {Types::String()}, {}});
  registerFunctionInCurrentScope(
      "__write",
      {Types::Int64(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int64()},
       {}});
  registerFunctionInCurrentScope(
      "__read",
      {Types::Int64(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int64()},
       {}});

  // Low-level memory access intrinsics
  // _load_i64(ptr, index) - load i64 from ptr at byte offset index*8
  registerFunctionInCurrentScope(
      "_load_i64",
      {Types::Int64(), {Types::RawPointer(Types::Int8()), Types::Int64()}, {}});
  // _store_i64(ptr, index, value) - store i64 to ptr at byte offset index*8
  registerFunctionInCurrentScope(
      "_store_i64",
      {Types::Void(),
       {Types::RawPointer(Types::Int8()), Types::Int64(), Types::Int64()},
       {}});

  // Memory allocation intrinsics
  // _malloc(size) - allocate size bytes, returns raw_ptr<i8>
  registerFunctionInCurrentScope(
      "_malloc", {Types::RawPointer(Types::Int8()), {Types::Int64()}, {}});
  // _free(ptr) - free previously allocated memory
  registerFunctionInCurrentScope(
      "_free", {Types::Void(), {Types::RawPointer(Types::Int8())}, {}});
  // _memcpy(dst, src, len) - copy len bytes from src to dst
  registerFunctionInCurrentScope(
      "_memcpy", {Types::Void(),
                  {Types::RawPointer(Types::UInt8()),
                   Types::RawPointer(Types::UInt8()), Types::Int64()},
                  {}});
  // _memset(dst, value, len) - set len bytes at dst to value
  registerFunctionInCurrentScope("_memset", {Types::Void(),
                                             {Types::RawPointer(Types::UInt8()),
                                              Types::Int32(), Types::Int64()},
                                             {}});
  // _ptr_offset(ptr, byte_offset) - offset a pointer by byte_offset bytes
  registerFunctionInCurrentScope(
      "_ptr_offset", {Types::RawPointer(Types::UInt8()),
                      {Types::RawPointer(Types::UInt8()), Types::Int64()},
                      {}});

  // Atomic intrinsics
  // _atomic_cmpxchg_i32(ptr, expected, desired) - atomic compare-and-swap
  registerFunctionInCurrentScope(
      "_atomic_cmpxchg_i32",
      {Types::Int32(),
       {Types::RawPointer(Types::Int32()), Types::Int32(), Types::Int32()},
       {}});
  // _atomic_store_i32(ptr, value) - atomic store with release ordering
  registerFunctionInCurrentScope(
      "_atomic_store_i32",
      {Types::Void(), {Types::RawPointer(Types::Int32()), Types::Int32()}, {}});
  // _atomic_load_i32(ptr) - atomic load with acquire ordering
  registerFunctionInCurrentScope(
      "_atomic_load_i32",
      {Types::Int32(), {Types::RawPointer(Types::Int32())}, {}});
  // _atomic_fetch_add_i32(ptr, delta) - atomic add, returns the old value
  registerFunctionInCurrentScope(
      "_atomic_fetch_add_i32",
      {Types::Int32(),
       {Types::RawPointer(Types::Int32()), Types::Int32()},
       {}});
  // _atomic_fetch_sub_i32(ptr, delta) - atomic subtract, returns the old value
  registerFunctionInCurrentScope(
      "_atomic_fetch_sub_i32",
      {Types::Int32(),
       {Types::RawPointer(Types::Int32()), Types::Int32()},
       {}});

  // Bit intrinsics
  // _mul_hi_u64(a, b) - high 64 bits of the 128-bit product a * b
  registerFunctionInCurrentScope(
      "_mul_hi_u64", {Types::UInt64(), {Types::UInt64(), Types::UInt64()}, {}});
  // _ctlz_u64(x) / _cttz_u64(x) - leading / trailing zero bit count (64 for 0)
  registerFunctionInCurrentScope("_ctlz_u64",
                                 {Types::UInt64(), {Types::UInt64()}, {}});
  registerFunctionInCurrentScope("_cttz_u64",
                                 {Types::UInt64(), {Types::UInt64()}, {}});

  // Target intrinsics
  // _target_is("macos") - compile-time check of the compilation target's
  // operating system; codegen folds it to a constant and keeps only the live
  // side of a branch on it
  registerFunctionInCurrentScope("_target_is",
                                 {Types::Bool(), {Types::String()}, {}});

  // Futex intrinsics (Linux-specific thread synchronization)
  // _futex_wait(ptr, expected) - block if *ptr == expected
  registerFunctionInCurrentScope(
      "_futex_wait",
      {Types::Void(), {Types::RawPointer(Types::Int32()), Types::Int32()}, {}});
  // _futex_wake(ptr) - wake one waiter
  registerFunctionInCurrentScope(
      "_futex_wake", {Types::Void(), {Types::RawPointer(Types::Int32())}, {}});

  // Network socket intrinsics (libc sockets)
  // __socket(domain, type, protocol) -> fd
  registerFunctionInCurrentScope(
      "__socket",
      {Types::Int32(), {Types::Int32(), Types::Int32(), Types::Int32()}, {}});
  // __bind(fd, addr, addrlen) -> result
  registerFunctionInCurrentScope(
      "__bind",
      {Types::Int32(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int32()},
       {}});
  // __listen(fd, backlog) -> result
  registerFunctionInCurrentScope(
      "__listen", {Types::Int32(), {Types::Int32(), Types::Int32()}, {}});
  // __accept(fd, addr, addrlen_ptr) -> new_fd
  registerFunctionInCurrentScope(
      "__accept", {Types::Int32(),
                   {Types::Int32(), Types::RawPointer(Types::UInt8()),
                    Types::RawPointer(Types::Int32())},
                   {}});
  // __connect(fd, addr, addrlen) -> result
  registerFunctionInCurrentScope(
      "__connect",
      {Types::Int32(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int32()},
       {}});
  // __send(fd, buf, len, flags) -> bytes_sent
  registerFunctionInCurrentScope(
      "__send", {Types::Int64(),
                 {Types::Int32(), Types::RawPointer(Types::UInt8()),
                  Types::Int64(), Types::Int32()},
                 {}});
  // __recv(fd, buf, len, flags) -> bytes_received
  registerFunctionInCurrentScope(
      "__recv", {Types::Int64(),
                 {Types::Int32(), Types::RawPointer(Types::UInt8()),
                  Types::Int64(), Types::Int32()},
                 {}});
  // __shutdown(fd, how) -> result
  registerFunctionInCurrentScope(
      "__shutdown", {Types::Int32(), {Types::Int32(), Types::Int32()}, {}});
  // __setsockopt(fd, level, optname, optval, optlen) -> result
  registerFunctionInCurrentScope(
      "__setsockopt", {Types::Int32(),
                       {Types::Int32(), Types::Int32(), Types::Int32(),
                        Types::RawPointer(Types::UInt8()), Types::Int32()},
                       {}});
  // __getsockopt(fd, level, optname, optval, optlen_ptr) -> result
  registerFunctionInCurrentScope(
      "__getsockopt",
      {Types::Int32(),
       {Types::Int32(), Types::Int32(), Types::Int32(),
        Types::RawPointer(Types::UInt8()), Types::RawPointer(Types::Int32())},
       {}});

  // High-level IPv4 socket intrinsics (build sockaddr_in internally)
  // __bind_ipv4(fd, ip, port) -> result
  registerFunctionInCurrentScope(
      "__bind_ipv4",
      {Types::Int32(), {Types::Int32(), Types::Int32(), Types::Int32()}, {}});
  // __connect_ipv4(fd, ip, port) -> result
  registerFunctionInCurrentScope(
      "__connect_ipv4",
      {Types::Int32(), {Types::Int32(), Types::Int32(), Types::Int32()}, {}});
  // __accept_fd(fd) -> new_fd
  registerFunctionInCurrentScope("__accept_fd",
                                 {Types::Int32(), {Types::Int32()}, {}});
}

// -------------------------------------------------------------------
// Namespace-qualified symbols (separate from scope-based lookup)
// -------------------------------------------------------------------

void SemanticContext::registerModuleVariable(const std::string& baseName,
                                             const std::string& qualifiedName,
                                             sun::TypePtr type,
                                             sun::Visibility visibility,
                                             bool isConst) {
  VariableInfo info{type, true, false};
  info.visibility = visibility;
  info.isConst = isConst;
  info.qualifiedName = makeQualifiedName(baseName);
  // Store with qualified name for codegen lookup
  rootScope_->namespacedVariables[qualifiedName] = info;
  if (currentScope_ != rootScope_.get()) {
    currentScope_->namespacedVariables[qualifiedName] = info;
  }
  // Also store with plain name in current scope for hasSymbol lookup
  currentScope_->namespacedVariables[baseName] = info;
  // The plain-name entry created by declareVariable during body analysis
  if (auto it = currentScope_->variables.find(baseName);
      it != currentScope_->variables.end()) {
    it->second.visibility = visibility;
    it->second.isConst = isConst;
    if (it->second.qualifiedName.empty())
      it->second.qualifiedName = info.qualifiedName;
  }
}

VariableInfo* SemanticContext::lookupQualifiedVariable(
    const std::string& qualifiedName) {
  return currentScope_->lookupQualifiedVariable(qualifiedName);
}

// Helper: Get the full module path including library scope hashes
// e.g., "b" -> "$hash$.b" if b is inside a library scope
std::string SemanticContext::getFullModulePath(
    const std::string& visiblePath) const {
  if (visiblePath.empty() || !currentScope_) return visiblePath;

  // Helper to find a segment and return its full path including library scopes
  // Throws on ambiguity (same name found in multiple library scopes)
  // Import scopes are traversed transparently (not included in path)
  std::function<std::string(const SemanticScope&, const std::string&,
                            const std::string&)>
      findFullPath = [&](const SemanticScope& scope, const std::string& segment,
                         const std::string& currentPath) -> std::string {
    // Direct child lookup
    auto it = scope.childModules.find(segment);
    if (it != scope.childModules.end() && !isLibraryScope(segment)) {
      return currentPath.empty() ? segment : currentPath + "." + segment;
    }

    // Search inside library scopes, tracking all matches for ambiguity
    std::string found;
    std::string foundInLib;

    for (const auto& [modName, child] : scope.childModules) {
      if (!child || !isLibraryScope(modName)) continue;

      // Include library scope name in the path (including import scopes)
      // Import scopes like $import_xxx$ must be in the full path for codegen
      std::string libPath =
          currentPath.empty() ? modName : currentPath + "." + modName;

      // Check if this library scope has the segment as direct child
      auto childIt = child->childModules.find(segment);
      if (childIt != child->childModules.end()) {
        std::string candidate =
            libPath.empty() ? segment : libPath + "." + segment;
        // Don't throw on ambiguity for modules - same module name in different
        // import scopes should be merged. Symbol lookup will find the right
        // one.
        if (found.empty()) {
          found = candidate;
          foundInLib = modName;
        }
      }  // Recursively check nested library scopes
      auto result = findFullPath(*child, segment, libPath);
      if (!result.empty()) {
        // Keep first match, let symbol lookup resolve actual ambiguity
        if (found.empty()) {
          found = result;
        }
      }
    }
    return found;
  };

  // Search from innermost scope outward (back to front) so that modules
  // registered in function scopes (via scoped imports) are resolved correctly.
  for (auto* scopeIt = currentScope_; scopeIt != nullptr;
       scopeIt = scopeIt->parent) {
    std::string fullPath;
    const SemanticScope* current = scopeIt;

    std::string segment;
    std::istringstream stream(visiblePath);
    bool resolved = true;
    while (std::getline(stream, segment, '.')) {
      // Find full path for this segment
      std::string segmentFullPath = findFullPath(*current, segment, fullPath);
      if (segmentFullPath.empty()) {
        resolved = false;
        break;
      }
      fullPath = segmentFullPath;

      // Navigate to that scope for next segment
      SemanticScope* nextScope = lookupModuleScope(fullPath);
      if (!nextScope) {
        resolved = false;
        break;
      }
      current = nextScope;
    }
    if (resolved && !fullPath.empty()) return fullPath;
  }

  return visiblePath;
}

const FunctionInfo* SemanticContext::lookupQualifiedFunction(
    const std::string& qualifiedName) const {
  return currentScope_->lookupQualifiedFunction(qualifiedName);
}

sun::QualifiedName SemanticContext::resolveNameWithUsings(
    const std::string& name) const {
  return currentScope_->resolveNameWithUsings(name);
}

void SemanticContext::addUsingImport(const UsingImport& import) {
  // Skip if already present (idempotent for Pass 1 + Pass 2 double-processing)
  for (const auto& existing : currentScope_->usingImports) {
    if (existing.namespacePath == import.namespacePath &&
        existing.target == import.target) {
      return;
    }
  }
  currentScope_->usingImports.push_back(import);
}

void SemanticContext::addImportBinding(const ImportBinding& binding) {
  // Skip if already present (idempotent for Pass 1 + Pass 2 double-processing)
  for (const auto& existing : currentScope_->importBindings) {
    if (existing.sourceScope == binding.sourceScope &&
        existing.isWildcard == binding.isWildcard &&
        existing.localName == binding.localName &&
        existing.sourceName == binding.sourceName) {
      return;
    }
  }
  currentScope_->importBindings.push_back(binding);
}

void SemanticContext::registerClass(const std::string& name,
                                    std::shared_ptr<sun::ClassType> classType,
                                    std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (currentScope_->classes.contains(name)) {
    return;
  }
  // Register in current scope
  currentScope_->classes[name] = classType;
}

std::shared_ptr<sun::ClassType> SemanticContext::lookupClass(
    const std::string& name) const {
  return currentScope_->lookupClass(name);
}

void SemanticContext::setCurrentClass(
    std::shared_ptr<sun::ClassType> classType) {
  currentClass_ = std::move(classType);
}

std::shared_ptr<sun::ClassType> SemanticContext::getCurrentClass() const {
  return currentClass_;
}

void SemanticContext::registerGenericClass(const std::string& name,
                                           const GenericClassInfo& info,
                                           std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (currentScope_->genericClasses.contains(name)) {
    return;
  }
  // Register in current scope
  auto& slot = currentScope_->genericClasses[name];
  slot = info;
  slot.definitionScope = currentScope_->shared_from_this();
}

const GenericClassInfo* SemanticContext::lookupGenericClass(
    const std::string& name) const {
  return currentScope_->lookupGenericClass(name);
}

const GenericClassInfo* SemanticContext::lookupGenericClass(
    const sun::QualifiedName& qualifiedName) const {
  return currentScope_->lookupGenericClass(qualifiedName);
}

void SemanticContext::addTypeParameterBindings(
    const std::vector<std::string>& params,
    const std::vector<sun::TypePtr>& args) {
  auto& scope = *currentScope_;
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    scope.typeParameters[params[i]] = args[i];
  }
}

sun::TypePtr SemanticContext::findTypeParameter(const std::string& name) const {
  // Search from innermost to outermost scope
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    auto found = s->typeParameters.find(name);
    if (found != s->typeParameters.end()) {
      return found->second;
    }
  }
  return nullptr;
}

sun::TypePtr SemanticContext::findTypeAlias(const std::string& name) const {
  // Search from innermost to outermost scope
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    auto found = s->typeAliases.find(name);
    if (found != s->typeAliases.end()) {
      return found->second;
    }
  }
  return nullptr;
}

void SemanticContext::registerInterface(
    const std::string& name, std::shared_ptr<sun::InterfaceType> interfaceType,
    std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (currentScope_->interfaces.contains(name)) {
    return;
  }
  // Register in current scope
  currentScope_->interfaces[name] = interfaceType;
}

std::shared_ptr<sun::InterfaceType> SemanticContext::lookupInterface(
    const std::string& name) const {
  auto result = currentScope_->lookupInterface(name);
  if (result) return result;

  // Check builtin interfaces in type registry (IError)
  if (typeRegistry_) {
    auto builtinInterface = typeRegistry_->lookupInterface(name);
    if (builtinInterface) return builtinInterface;
  }

  return nullptr;
}

void SemanticContext::registerGenericInterface(const std::string& name,
                                               const GenericInterfaceInfo& info,
                                               std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (currentScope_->genericInterfaces.contains(name)) {
    return;
  }
  // Register in current scope
  auto& slot = currentScope_->genericInterfaces[name];
  slot = info;
  slot.definitionScope = currentScope_->shared_from_this();
}

const GenericInterfaceInfo* SemanticContext::lookupGenericInterface(
    const std::string& name) const {
  return currentScope_->lookupGenericInterface(name);
}

std::shared_ptr<sun::EnumType> SemanticContext::lookupEnum(
    const std::string& name) const {
  return currentScope_->lookupEnum(name);
}

void SemanticContext::registerEnum(const std::string& name,
                                   std::shared_ptr<sun::EnumType> enumType) {
  // Register in current scope
  currentScope_->enums[name] = enumType;
}

void SemanticContext::registerGenericEnum(const std::string& name,
                                          GenericEnumInfo info) {
  info.definitionScope = currentScope_->shared_from_this();
  currentScope_->genericEnums[name] = std::move(info);
}

const GenericEnumInfo* SemanticContext::lookupGenericEnum(
    const std::string& name) const {
  return currentScope_->lookupGenericEnum(name);
}

sun::ModulePath SemanticContext::currentModulePath() const {
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    if (s->getType() == ScopeType::Module) return s->scopePath;
  }
  return {};
}

void SemanticContext::denyAccess(const sun::access::ItemRef& item) const {
  auto loc = currentLocation();
  logSemanticError(sun::access::denialMessage(item), loc);
}

const sun::ClassField* SemanticContext::accessibleField(
    const sun::ClassType& cls, const std::string& name,
    const Position& loc) const {
  const auto* f = cls.getField(name);
  if (f) requireAccessible(fieldRef(cls, *f), loc);
  return f;
}

const sun::ClassMethod* SemanticContext::accessibleMethod(
    const sun::ClassType& cls, const std::string& name,
    const Position& loc) const {
  const auto* m = cls.getMethod(name);
  if (m) requireAccessible(methodRef(cls, *m), loc);
  return m;
}

const sun::ClassMethod* SemanticContext::accessibleMethodForArgs(
    const sun::ClassType& cls, const std::string& name,
    const std::vector<sun::TypePtr>& argTypes, const Position& loc) const {
  const auto* m = cls.getMethodForArgs(name, argTypes);
  if (m) requireAccessible(methodRef(cls, *m), loc);
  return m;
}

const sun::InterfaceField* SemanticContext::accessibleField(
    const sun::InterfaceType& iface, const std::string& name,
    const Position& loc) const {
  const auto* f = iface.getField(name);
  if (f) requireAccessible(fieldRef(iface, *f), loc);
  return f;
}

const sun::InterfaceMethod* SemanticContext::accessibleMethod(
    const sun::InterfaceType& iface, const std::string& name,
    const Position& loc) const {
  const auto* m = iface.getMethod(name);
  if (m) requireAccessible(methodRef(iface, *m), loc);
  return m;
}

void SemanticContext::requireModuleAccessible(
    const SemanticScopeBase& moduleScope, const Position& loc) const {
  for (auto* s = &moduleScope; s && s->getType() == ScopeType::Module;
       s = s->parent) {
    if (isLibraryScope(s->scopeName))
      continue;  // bundle boundary, not a module
    requireAccessible(moduleRef(static_cast<const ModuleScope&>(*s)), loc);
  }
}
