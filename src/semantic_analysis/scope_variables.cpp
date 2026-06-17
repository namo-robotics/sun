// semantic_analysis/scope_variables.cpp — Scope and variable management

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "error.h"
#include "semantic_analyzer.h"

using sun::unwrapRef;

// isLibraryScope() and mangleModulePath() are provided by semantic_scope.h

// -------------------------------------------------------------------
// SemanticScope helpers
// -------------------------------------------------------------------

bool SemanticScope::hasSymbol(const std::string& name) const {
  if (classes.contains(name)) return true;
  if (genericClasses.contains(name)) return true;
  if (interfaces.contains(name)) return true;
  if (genericInterfaces.contains(name)) return true;
  if (enums.contains(name)) return true;
  if (namespacedVariables.contains(name)) return true;

  // Check functions by name (O(1) via indexed FunctionTable)
  if (functions.hasName(name)) return true;

  return false;
}

std::shared_ptr<sun::ClassType> SemanticScope::findClass(
    const std::string& name) const {
  auto it = classes.find(name);
  if (it != classes.end()) return it->second;
  return nullptr;
}

const GenericClassInfo* SemanticScope::findGenericClass(
    const std::string& name) const {
  auto it = genericClasses.find(name);
  if (it != genericClasses.end()) return &it->second;
  return nullptr;
}

std::shared_ptr<sun::InterfaceType> SemanticScope::findInterface(
    const std::string& name) const {
  auto it = interfaces.find(name);
  if (it != interfaces.end()) return it->second;
  return nullptr;
}

const GenericInterfaceInfo* SemanticScope::findGenericInterface(
    const std::string& name) const {
  auto it = genericInterfaces.find(name);
  if (it != genericInterfaces.end()) return &it->second;
  return nullptr;
}

std::shared_ptr<sun::EnumType> SemanticScope::findEnum(
    const std::string& name) const {
  auto it = enums.find(name);
  if (it != enums.end()) return it->second;
  return nullptr;
}

void SemanticScope::collectFunctions(const std::string& prefix,
                                     std::vector<FunctionInfo>& results) const {
  // prefix is "name(" — extract the name part for indexed lookup
  std::string name =
      prefix.substr(0, prefix.size() - 1);  // remove trailing '('
  if (auto* overloads = functions.getOverloads(name)) {
    for (const auto* info : *overloads) {
      results.push_back(*info);
    }
  }
}

// -------------------------------------------------------------------
// Scope management - tree-based scopes
// -------------------------------------------------------------------

std::shared_ptr<SemanticScopeBase> SemanticScopeBase::cloneSymbols(
    SemanticScopeBase* newParent) const {
  std::shared_ptr<SemanticScopeBase> clone;
  switch (getType()) {
    case ScopeType::Global:
      clone = std::make_shared<GlobalScope>();
      break;
    case ScopeType::Module:
      clone = std::make_shared<ModuleScope>();
      break;
    case ScopeType::Import:
      clone = std::make_shared<ImportScope>();
      break;
    default:
      // Should never happen - only persistent scopes can clone
      return nullptr;
  }
  clone->scopeName = scopeName;
  clone->scopePath = scopePath;
  clone->parent = newParent;
  // Copy all symbol tables (shallow — shares type objects)
  clone->functions = functions;
  clone->classes = classes;
  clone->classDefinitions = classDefinitions;
  clone->genericClasses = genericClasses;
  clone->interfaces = interfaces;
  clone->genericInterfaces = genericInterfaces;
  clone->enums = enums;
  clone->genericFunctions = genericFunctions;
  clone->childModules = childModules;
  clone->namespacedVariables = namespacedVariables;
  clone->variables = variables;
  clone->usingImports = usingImports;
  clone->importBindings = importBindings;
  return clone;
}

void SemanticAnalyzer::enterTypeParamScope(
    const std::vector<std::string>& params,
    const std::vector<sun::TypePtr>& args) {
  enterScope(ScopeType::TypeParams);
  addTypeParameterBindings(params, args);
}

void SemanticAnalyzer::enterScope(ScopeType type) {
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
      block->inUnsafeContext = currentScope->inUnsafeContext;
      child = block;
      break;
    }
    case ScopeType::TypeParams:
      child = std::make_shared<TypeParamsScope>();
      break;
  }
  child->parent = currentScope;
  currentScope->children.push_back(child);
  currentScope = child.get();
}

void SemanticAnalyzer::enterModuleScope(const std::string& moduleName) {
  // Create or reuse a child module scope in the current scope's tree
  auto& child = currentScope->childModules[moduleName];
  if (!child) {
    auto modScope = std::make_shared<ModuleScope>();
    modScope->scopeName = moduleName;
    modScope->parent = currentScope;
    // Compute full scope path by extending parent's path
    modScope->scopePath = currentScope->scopePath;
    modScope->scopePath.push_back(moduleName);
    child = modScope;
  }
  currentScope = child.get();
}

void SemanticAnalyzer::enterClassScope(const sun::QualifiedName& className) {
  auto classScope = std::make_shared<ClassScope>();
  classScope->classBaseName = className.baseName;
  classScope->classMangledName = className.mangled();
  // Use the class's scope path directly
  classScope->scopePath = className.scopePath;
  classScope->scopePath.push_back(className.baseName);
  classScope->parent = currentScope;
  currentScope->children.push_back(classScope);
  currentScope = classScope.get();
}

void SemanticAnalyzer::enterInterfaceScope(
    const sun::QualifiedName& interfaceName) {
  auto ifaceScope = std::make_shared<InterfaceScope>();
  ifaceScope->interfaceBaseName = interfaceName.baseName;
  ifaceScope->interfaceMangledName = interfaceName.mangled();
  // Use the interface's scope path directly
  ifaceScope->scopePath = interfaceName.scopePath;
  ifaceScope->scopePath.push_back(interfaceName.baseName);
  ifaceScope->parent = currentScope;
  currentScope->children.push_back(ifaceScope);
  currentScope = ifaceScope.get();
}

void SemanticAnalyzer::enterFunctionScope(const std::string& funcSig,
                                          const sun::QualifiedName& funcName,
                                          bool canThrow) {
  auto funcScope = std::make_shared<FunctionScope>();
  funcScope->functionSignature = funcSig;
  funcScope->functionName = funcName;
  funcScope->functionCanThrow = canThrow;
  funcScope->parent = currentScope;

  // Set scopePath to include the function's mangled name so nested functions
  // get unique qualified names (e.g., inner inside outer_i32 ->
  // outer_i32_inner)
  funcScope->scopePath = {funcName.mangled()};

  currentScope->children.push_back(funcScope);
  currentScope = funcScope.get();
}

void SemanticAnalyzer::exitScope() {
  if (currentScope->parent) {
    auto* parent = currentScope->parent;
    // Keep all scopes in the tree for debugging/visualization.
    // Symbol lookups already don't descend into Function scopes.
    currentScope = parent;
  }
}

std::string SemanticAnalyzer::getCurrentModulePrefix() const {
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

std::vector<std::string> SemanticAnalyzer::getCurrentScopePath() const {
  // Walk up to find the nearest scope with a scopePath (Module or Import).
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    if (!s->scopePath.empty()) {
      return s->scopePath;
    }
  }
  return {};
}

sun::QualifiedName SemanticAnalyzer::makeQualifiedName(
    const std::string& baseName) const {
  return sun::QualifiedName(getCurrentScopePath(), baseName);
}

std::string SemanticAnalyzer::qualifyNameInCurrentModule(
    const std::string& name) const {
  std::string prefix = getCurrentModulePrefix();
  return prefix + name;
}

bool SemanticAnalyzer::isInThrowingFunction() const {
  // Find the nearest enclosing function scope and check if it can throw
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    if (auto* funcScope = s->asFunction()) {
      return funcScope->functionCanThrow;
    }
  }
  return false;
}

bool SemanticAnalyzer::isInTryBlock() const {
  // Check if any scope in the chain has tryBlockDepth > 0
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
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

void SemanticAnalyzer::enterTryBlock() {
  if (currentScope) {
    currentScope->tryBlockDepth++;
  }
}

void SemanticAnalyzer::exitTryBlock() {
  if (currentScope && currentScope->tryBlockDepth > 0) {
    currentScope->tryBlockDepth--;
  }
}

bool SemanticAnalyzer::isInUnsafeBlock() const {
  return currentScope && currentScope->inUnsafeContext;
}

void SemanticAnalyzer::enterUnsafeBlock() {
  if (currentScope) {
    currentScope->unsafeBlockDepth++;
    currentScope->inUnsafeContext = true;
  }
}

void SemanticAnalyzer::exitUnsafeBlock() {
  if (currentScope && currentScope->unsafeBlockDepth > 0) {
    currentScope->unsafeBlockDepth--;
    if (currentScope->unsafeBlockDepth == 0) {
      // Restore based on parent (if we inherited from parent, stay in unsafe)
      // For function/module/import scopes, parent's context doesn't matter
      currentScope->inUnsafeContext =
          currentScope->parent && currentScope->getType() == ScopeType::Block &&
          currentScope->parent->inUnsafeContext;
    }
  }
}

bool SemanticAnalyzer::isModuleName(const std::string& name) const {
  return currentScope->isModuleName(name);
}

SemanticScope* SemanticAnalyzer::lookupModuleScope(
    const std::string& dotPath) const {
  if (!currentScope) return nullptr;
  return currentScope->lookupModuleScope(dotPath);
}

std::vector<UsingImport> SemanticAnalyzer::getActiveUsingImports() const {
  return currentScope->getActiveUsingImports();
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

SymbolMatch SemanticAnalyzer::findSymbolInModule(const std::string& modulePath,
                                                 const std::string& name,
                                                 SymbolKind filterKind) const {
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
        return match;
      }
    }

    // Check functions
    if (matchesFilter(SymbolKind::Function)) {
      if (auto* overloads = scope->functions.getOverloads(name)) {
        if (!overloads->empty()) {
          const auto* info = overloads->front();
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
    auto allScopes = collectAllModuleScopes(currentScope, visiblePath);
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

  return SymbolMatch{};
}

// -------------------------------------------------------------------
// Variable management
// -------------------------------------------------------------------

void SemanticAnalyzer::declareVariable(const std::string& name,
                                       sun::TypePtr type, bool isParam) {
  // Block user-defined identifiers starting with underscore
  if (isReservedIdentifier(name)) {
    logAndThrowError(
        "Identifier '" + name +
        "' is invalid: names starting with '_' are reserved for builtins");
  }
  // Check for shadowing of global/module variables
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
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
  currentScope->variables[name] = {type, isAtModuleLevel(), isParam, false};
}

VariableInfo* SemanticAnalyzer::lookupVariable(const std::string& name) {
  return currentScope->lookupVariable(name);
}

// -------------------------------------------------------------------
// Type narrowing (from _is<T> type guards)
// -------------------------------------------------------------------

void SemanticAnalyzer::narrowVariable(const std::string& varName,
                                      sun::TypePtr narrowedType) {
  currentScope->narrowedTypes[varName] = std::move(narrowedType);
}

sun::TypePtr SemanticAnalyzer::getNarrowedType(
    const std::string& varName, sun::TypePtr originalType) const {
  // Search from innermost to outermost scope
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
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

std::string SemanticAnalyzer::getFunctionSignature(
    const std::string& name, const std::vector<sun::TypePtr>& paramTypes) {
  std::string sig = name + "(";
  for (size_t i = 0; i < paramTypes.size(); ++i) {
    if (i > 0) sig += ",";
    sig += paramTypes[i] ? paramTypes[i]->toString() : "?";
  }
  sig += ")";
  return sig;
}

void SemanticAnalyzer::registernFunctionInCurrentScope(
    const std::string& name, const FunctionInfo& info) {
  // Functions are registered in their enclosing scope. For nested functions,
  // this is the parent function's scope - the scope hierarchy naturally
  // disambiguates between different generic instantiations.
  std::string sig = getFunctionSignature(name, info.paramTypes);
  // Always overwrite: the declaration pre-pass registers with minimal info
  // (no captures), and the normal pass overwrites with complete info.
  // This also handles diamond import re-registration gracefully.
  currentScope->functions[sig] = info;
}

void SemanticAnalyzer::registerGenericFunctionInCurrentScope(
    FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  GenericFunctionInfo genInfo;
  genInfo.AST = &func;
  genInfo.typeParameters = proto.getTypeParameters();
  if (proto.hasReturnType()) {
    genInfo.returnType = *proto.getReturnType();
  }
  genInfo.params = proto.getArgs();

  sun::QualifiedName qname(getCurrentScopePath(), proto.getName());
  currentScope->genericFunctions[qname] = genInfo;
}

const GenericFunctionInfo* SemanticAnalyzer::lookupGenericFunction(
    const std::string& name) const {
  return currentScope->lookupGenericFunction(name);
}

std::vector<FunctionInfo> SemanticAnalyzer::getAllFunctions(
    const std::string& name) const {
  return currentScope->getAllFunctions(name);
}

std::optional<FunctionInfo> SemanticAnalyzer::lookupFunction(
    const std::string& name, const std::vector<sun::TypePtr>& argTypes) const {
  return currentScope->lookupFunction(name, argTypes);
}

void SemanticAnalyzer::registerGlobal(const std::string& name,
                                      sun::TypePtr type) {
  rootScope->variables[name] = {type, true, false, false};
}

// -------------------------------------------------------------------
// Builtin function registration
// -------------------------------------------------------------------

void SemanticAnalyzer::registerBuiltinFunctions() {
  using sun::Types;

  // Low-level print intrinsics (used by stdlib print functions)
  registernFunctionInCurrentScope("_print_i32",
                                  {Types::Void(), {Types::Int32()}, {}});
  registernFunctionInCurrentScope("_print_i64",
                                  {Types::Void(), {Types::Int64()}, {}});
  registernFunctionInCurrentScope("_print_f64",
                                  {Types::Void(), {Types::Float64()}, {}});
  registernFunctionInCurrentScope("_print_newline", {Types::Void(), {}, {}});
  // _print_bytes intrinsic: write raw bytes to stdout
  registernFunctionInCurrentScope(
      "_print_bytes",
      {Types::Void(), {Types::RawPointer(Types::Int8()), Types::Int64()}, {}});
  // _println_str: print string literal with newline
  registernFunctionInCurrentScope("_println_str",
                                  {Types::Void(), {Types::String()}, {}});
  registernFunctionInCurrentScope(
      "_println_str", {Types::Void(), {Types::RawPointer(Types::UInt8())}, {}});

  // File I/O intrinsics
  registernFunctionInCurrentScope(
      "__file_open", {Types::Int32(), {Types::String(), Types::Int32()}, {}});
  registernFunctionInCurrentScope("__file_close",
                                  {Types::Int32(), {Types::Int32()}, {}});
  registernFunctionInCurrentScope(
      "__file_write", {Types::Int32(), {Types::Int32(), Types::String()}, {}});
  registernFunctionInCurrentScope(
      "__file_read",
      {Types::RawPointer(Types::Int8()), {Types::Int32(), Types::Int32()}, {}});

  // Extended file I/O intrinsics
  registernFunctionInCurrentScope(
      "__lseek",
      {Types::Int64(), {Types::Int32(), Types::Int64(), Types::Int32()}, {}});
  registernFunctionInCurrentScope(
      "__fstat",
      {Types::Int32(), {Types::Int32(), Types::RawPointer(Types::Int8())}, {}});
  registernFunctionInCurrentScope("__fsync",
                                  {Types::Int32(), {Types::Int32()}, {}});
  registernFunctionInCurrentScope(
      "__ftruncate", {Types::Int32(), {Types::Int32(), Types::Int64()}, {}});
  registernFunctionInCurrentScope("__unlink",
                                  {Types::Int32(), {Types::String()}, {}});
  registernFunctionInCurrentScope(
      "__rename", {Types::Int32(), {Types::String(), Types::String()}, {}});
  registernFunctionInCurrentScope(
      "__mkdir", {Types::Int32(), {Types::String(), Types::Int32()}, {}});
  registernFunctionInCurrentScope("__rmdir",
                                  {Types::Int32(), {Types::String()}, {}});
  registernFunctionInCurrentScope(
      "__write",
      {Types::Int64(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int64()},
       {}});
  registernFunctionInCurrentScope(
      "__read",
      {Types::Int64(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int64()},
       {}});

  // Low-level memory access intrinsics
  // _load_i64(ptr, index) - load i64 from ptr at byte offset index*8
  registernFunctionInCurrentScope(
      "_load_i64",
      {Types::Int64(), {Types::RawPointer(Types::Int8()), Types::Int64()}, {}});
  // _store_i64(ptr, index, value) - store i64 to ptr at byte offset index*8
  registernFunctionInCurrentScope(
      "_store_i64",
      {Types::Void(),
       {Types::RawPointer(Types::Int8()), Types::Int64(), Types::Int64()},
       {}});

  // Memory allocation intrinsics
  // _malloc(size) - allocate size bytes, returns raw_ptr<i8>
  registernFunctionInCurrentScope(
      "_malloc", {Types::RawPointer(Types::Int8()), {Types::Int64()}, {}});
  // _free(ptr) - free previously allocated memory
  registernFunctionInCurrentScope(
      "_free", {Types::Void(), {Types::RawPointer(Types::Int8())}, {}});

  // Atomic intrinsics
  // _atomic_cmpxchg_i32(ptr, expected, desired) - atomic compare-and-swap
  registernFunctionInCurrentScope(
      "_atomic_cmpxchg_i32",
      {Types::Int32(),
       {Types::RawPointer(Types::Int32()), Types::Int32(), Types::Int32()},
       {}});
  // _atomic_store_i32(ptr, value) - atomic store with release ordering
  registernFunctionInCurrentScope(
      "_atomic_store_i32",
      {Types::Void(), {Types::RawPointer(Types::Int32()), Types::Int32()}, {}});
  // _atomic_load_i32(ptr) - atomic load with acquire ordering
  registernFunctionInCurrentScope(
      "_atomic_load_i32",
      {Types::Int32(), {Types::RawPointer(Types::Int32())}, {}});

  // Futex intrinsics (Linux-specific thread synchronization)
  // _futex_wait(ptr, expected) - block if *ptr == expected
  registernFunctionInCurrentScope(
      "_futex_wait",
      {Types::Void(), {Types::RawPointer(Types::Int32()), Types::Int32()}, {}});
  // _futex_wake(ptr) - wake one waiter
  registernFunctionInCurrentScope(
      "_futex_wake", {Types::Void(), {Types::RawPointer(Types::Int32())}, {}});

  // Network socket intrinsics (Linux-specific raw syscalls)
  // __socket(domain, type, protocol) -> fd
  registernFunctionInCurrentScope(
      "__socket",
      {Types::Int32(), {Types::Int32(), Types::Int32(), Types::Int32()}, {}});
  // __bind(fd, addr, addrlen) -> result
  registernFunctionInCurrentScope(
      "__bind",
      {Types::Int32(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int32()},
       {}});
  // __listen(fd, backlog) -> result
  registernFunctionInCurrentScope(
      "__listen", {Types::Int32(), {Types::Int32(), Types::Int32()}, {}});
  // __accept(fd, addr, addrlen_ptr) -> new_fd
  registernFunctionInCurrentScope(
      "__accept",
      {Types::Int32(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()),
        Types::RawPointer(Types::Int32())},
       {}});
  // __connect(fd, addr, addrlen) -> result
  registernFunctionInCurrentScope(
      "__connect",
      {Types::Int32(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int32()},
       {}});
  // __send(fd, buf, len, flags) -> bytes_sent
  registernFunctionInCurrentScope(
      "__send",
      {Types::Int64(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int64(),
        Types::Int32()},
       {}});
  // __recv(fd, buf, len, flags) -> bytes_received
  registernFunctionInCurrentScope(
      "__recv",
      {Types::Int64(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int64(),
        Types::Int32()},
       {}});
  // __shutdown(fd, how) -> result
  registernFunctionInCurrentScope(
      "__shutdown", {Types::Int32(), {Types::Int32(), Types::Int32()}, {}});
  // __setsockopt(fd, level, optname, optval, optlen) -> result
  registernFunctionInCurrentScope(
      "__setsockopt",
      {Types::Int32(),
       {Types::Int32(), Types::Int32(), Types::Int32(),
        Types::RawPointer(Types::UInt8()), Types::Int32()},
       {}});
  // __getsockopt(fd, level, optname, optval, optlen_ptr) -> result
  registernFunctionInCurrentScope(
      "__getsockopt",
      {Types::Int32(),
       {Types::Int32(), Types::Int32(), Types::Int32(),
        Types::RawPointer(Types::UInt8()), Types::RawPointer(Types::Int32())},
       {}});
}

// -------------------------------------------------------------------
// Namespace-qualified symbols (separate from scope-based lookup)
// -------------------------------------------------------------------

void SemanticAnalyzer::registerModuleVariable(const std::string& baseName,
                                              const std::string& qualifiedName,
                                              sun::TypePtr type) {
  // Store with qualified name for codegen lookup
  rootScope->namespacedVariables[qualifiedName] = {type, true, false};
  if (currentScope != rootScope.get()) {
    currentScope->namespacedVariables[qualifiedName] = {type, true, false};
  }
  // Also store with plain name in current scope for hasSymbol lookup
  currentScope->namespacedVariables[baseName] = {type, true, false};
}

VariableInfo* SemanticAnalyzer::lookupQualifiedVariable(
    const std::string& qualifiedName) {
  return currentScope->lookupQualifiedVariable(qualifiedName);
}

// Helper: Get the full module path including library scope hashes
// e.g., "b" -> "$hash$.b" if b is inside a library scope
std::string SemanticAnalyzer::getFullModulePath(
    const std::string& visiblePath) const {
  if (visiblePath.empty() || !currentScope) return visiblePath;

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
      }

      // Recursively check nested library scopes
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
  for (auto* scopeIt = currentScope; scopeIt != nullptr;
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

const FunctionInfo* SemanticAnalyzer::lookupQualifiedFunction(
    const std::string& qualifiedName) const {
  return currentScope->lookupQualifiedFunction(qualifiedName);
}

sun::QualifiedName SemanticAnalyzer::resolveNameWithUsings(
    const std::string& name) const {
  return currentScope->resolveNameWithUsings(name);
}

void SemanticAnalyzer::addUsingImport(const UsingImport& import) {
  // Skip if already present (idempotent for Pass 1 + Pass 2 double-processing)
  for (const auto& existing : currentScope->usingImports) {
    if (existing.namespacePath == import.namespacePath &&
        existing.target == import.target) {
      return;
    }
  }
  currentScope->usingImports.push_back(import);
}

void SemanticAnalyzer::addImportBinding(const ImportBinding& binding) {
  // Skip if already present (idempotent for Pass 1 + Pass 2 double-processing)
  for (const auto& existing : currentScope->importBindings) {
    if (existing.sourceScope == binding.sourceScope &&
        existing.isWildcard == binding.isWildcard &&
        existing.localName == binding.localName &&
        existing.sourceName == binding.sourceName) {
      return;
    }
  }
  currentScope->importBindings.push_back(binding);
}
