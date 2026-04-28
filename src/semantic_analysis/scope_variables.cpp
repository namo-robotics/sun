// semantic_analysis/scope_variables.cpp — Scope and variable management

#include <algorithm>
#include <cstdint>
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

  // Functions use signature keys, check by prefix "name("
  // Also check for qualified names: if scope has modulePath, symbols may be
  // registered with that path prefix (e.g., "$hash$_sun_name")
  std::string prefix = name + "(";
  std::string qualifiedPrefix;
  if (!modulePath.empty()) {
    // Convert dot path to underscore: "$hash$.sun" -> "$hash$_sun_"
    std::string mangledPath = modulePath;
    for (char& c : mangledPath) {
      if (c == '.') c = '_';
    }
    qualifiedPrefix = mangledPath + "_" + name + "(";
  }

  for (const auto& [sig, info] : functions) {
    if (sig.compare(0, prefix.size(), prefix) == 0) return true;
    if (!qualifiedPrefix.empty() &&
        sig.compare(0, qualifiedPrefix.size(), qualifiedPrefix) == 0)
      return true;
  }

  // Also check classes/interfaces with qualified names
  if (!qualifiedPrefix.empty()) {
    std::string mangledPath = modulePath;
    for (char& c : mangledPath) {
      if (c == '.') c = '_';
    }
    std::string qualifiedName = mangledPath + "_" + name;
    if (classes.contains(qualifiedName)) return true;
    if (genericClasses.contains(qualifiedName)) return true;
    if (interfaces.contains(qualifiedName)) return true;
    if (genericInterfaces.contains(qualifiedName)) return true;
  }

  // Recurse into child module scopes
  for (const auto& [modName, child] : childModules) {
    if (child && child->hasSymbol(name)) return true;
  }
  return false;
}

std::shared_ptr<sun::ClassType> SemanticScope::findClass(
    const std::string& name) const {
  auto it = classes.find(name);
  if (it != classes.end()) return it->second;
  for (const auto& [modName, child] : childModules) {
    if (child) {
      auto result = child->findClass(name);
      if (result) return result;
    }
  }
  return nullptr;
}

const GenericClassInfo* SemanticScope::findGenericClass(
    const std::string& name) const {
  auto it = genericClasses.find(name);
  if (it != genericClasses.end()) return &it->second;
  for (const auto& [modName, child] : childModules) {
    if (child) {
      auto result = child->findGenericClass(name);
      if (result) return result;
    }
  }
  return nullptr;
}

std::shared_ptr<sun::InterfaceType> SemanticScope::findInterface(
    const std::string& name) const {
  auto it = interfaces.find(name);
  if (it != interfaces.end()) return it->second;
  for (const auto& [modName, child] : childModules) {
    if (child) {
      auto result = child->findInterface(name);
      if (result) return result;
    }
  }
  return nullptr;
}

const GenericInterfaceInfo* SemanticScope::findGenericInterface(
    const std::string& name) const {
  auto it = genericInterfaces.find(name);
  if (it != genericInterfaces.end()) return &it->second;
  for (const auto& [modName, child] : childModules) {
    if (child) {
      auto result = child->findGenericInterface(name);
      if (result) return result;
    }
  }
  return nullptr;
}

std::shared_ptr<sun::EnumType> SemanticScope::findEnum(
    const std::string& name) const {
  auto it = enums.find(name);
  if (it != enums.end()) return it->second;
  for (const auto& [modName, child] : childModules) {
    if (child) {
      auto result = child->findEnum(name);
      if (result) return result;
    }
  }
  return nullptr;
}

const FunctionInfo* SemanticScope::findFunction(const std::string& sig) const {
  auto it = functions.find(sig);
  if (it != functions.end()) return &it->second;
  for (const auto& [modName, child] : childModules) {
    if (child) {
      auto result = child->findFunction(sig);
      if (result) return result;
    }
  }
  return nullptr;
}

void SemanticScope::collectFunctions(const std::string& prefix,
                                     std::vector<FunctionInfo>& results) const {
  for (const auto& [sig, info] : functions) {
    if (sig.compare(0, prefix.size(), prefix) == 0) {
      results.push_back(info);
    }
  }
  for (const auto& [modName, child] : childModules) {
    if (child) child->collectFunctions(prefix, results);
  }
}

// -------------------------------------------------------------------
// Scope management - typed scopes
// -------------------------------------------------------------------

void SemanticAnalyzer::enterScope(ScopeType type) {
  scopeStack.emplace_back(type);
}

void SemanticAnalyzer::enterModuleScope(const std::string& moduleName) {
  // Create or reuse a child module scope in the current scope's tree
  auto& parentScope = scopeStack.back();
  auto& child = parentScope.childModules[moduleName];
  if (!child) {
    child = std::make_shared<SemanticScope>(ScopeType::Module, moduleName);
    child->parent = &parentScope;
    // Compute full dot-separated module path
    std::string parentPath = parentScope.modulePath;
    child->modulePath =
        parentPath.empty() ? moduleName : parentPath + "." + moduleName;
  }
  scopeStack.push_back(*child);  // Push copy onto stack
  scopeStack.back().parent = &parentScope;
}

void SemanticAnalyzer::enterFunctionScope(const std::string& funcSig) {
  scopeStack.emplace_back(ScopeType::Function);
  scopeStack.back().functionSignature = funcSig;
}

void SemanticAnalyzer::exitScope() {
  if (scopeStack.size() > 1) {
    auto& top = scopeStack.back();
    // Sync module scope symbols back to the canonical childModules entry
    if (top.type == ScopeType::Module && scopeStack.size() >= 2) {
      auto& parent = scopeStack[scopeStack.size() - 2];
      auto it = parent.childModules.find(top.moduleName);
      if (it != parent.childModules.end()) {
        // Copy persistent symbol tables back (but not transient state)
        auto& canonical = *it->second;
        canonical.functions = top.functions;
        canonical.classes = top.classes;
        canonical.genericClasses = top.genericClasses;
        canonical.interfaces = top.interfaces;
        canonical.genericInterfaces = top.genericInterfaces;
        canonical.enums = top.enums;
        canonical.genericFunctions = top.genericFunctions;
        canonical.childModules = top.childModules;
        canonical.namespacedVariables = top.namespacedVariables;
        canonical.declaredModules = top.declaredModules;
      }
    }
    scopeStack.pop_back();
  }
}

std::string SemanticAnalyzer::getCurrentModulePrefix() const {
  // Walk scope stack from innermost to outermost, collecting module names
  // All segments are joined with underscores: $hash$_sun_
  std::string prefix;
  for (const auto& scope : scopeStack) {
    if (scope.type == ScopeType::Module && !scope.moduleName.empty()) {
      if (!prefix.empty()) prefix += "_";
      prefix += scope.moduleName;
    }
  }
  if (!prefix.empty()) prefix += "_";
  return prefix;
}

std::string SemanticAnalyzer::getCurrentModulePath() const {
  // Walk scope stack collecting module names with dot separators
  std::string path;
  for (const auto& scope : scopeStack) {
    if (scope.type == ScopeType::Module && !scope.moduleName.empty()) {
      if (!path.empty()) path += ".";
      path += scope.moduleName;
    }
  }
  return path;
}

sun::QualifiedName SemanticAnalyzer::makeQualifiedName(
    const std::string& baseName) const {
  // Module path includes library hash for moon imports (library scope uses
  // hash as its module name, e.g., "$hash$.sun.submodule")
  return sun::QualifiedName(getCurrentModulePath(), baseName);
}

std::string SemanticAnalyzer::qualifyNameInCurrentModule(
    const std::string& name) const {
  std::string prefix = getCurrentModulePrefix();
  return prefix + name;
}

bool SemanticAnalyzer::isInModuleScope() const {
  for (const auto& scope : scopeStack) {
    if (scope.type == ScopeType::Module) {
      return true;
    }
  }
  return false;
}

bool SemanticAnalyzer::isInFunctionScope() const {
  for (const auto& scope : scopeStack) {
    if (scope.type == ScopeType::Function) {
      return true;
    }
  }
  return false;
}

std::string SemanticAnalyzer::getCurrentFunctionContext() const {
  // Build context from all enclosing function scopes' signatures
  // e.g., "outer(i32)::middle(f64)" for doubly-nested functions
  std::string context;
  for (const auto& scope : scopeStack) {
    if (scope.type == ScopeType::Function && !scope.functionSignature.empty()) {
      if (!context.empty()) context += "::";
      context += scope.functionSignature;
    }
  }
  return context;
}

void SemanticAnalyzer::registerModule(const std::string& modulePath) {
  scopeStack.front().declaredModules.insert(modulePath);
  if (scopeStack.size() > 1) {
    scopeStack.back().declaredModules.insert(modulePath);
  }
}

bool SemanticAnalyzer::isModuleName(const std::string& name) const {
  // Helper to recursively check scopes, traversing through library scopes
  // Library scopes are transparent - we look through them to find nested
  // modules
  std::function<bool(const SemanticScope&)> checkScope =
      [&](const SemanticScope& scope) -> bool {
    // Check if this scope has a direct child module with this name
    if (scope.childModules.count(name) > 0) return true;

    // Search through library scopes transparently
    for (const auto& [modName, child] : scope.childModules) {
      if (!child) continue;
      if (isLibraryScope(modName)) {
        if (checkScope(*child)) return true;
      }
    }
    return false;
  };

  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    if (checkScope(*it)) return true;
  }
  return false;
}

SemanticScope* SemanticAnalyzer::lookupModuleScope(
    const std::string& dotPath) const {
  if (dotPath.empty() || scopeStack.empty()) return nullptr;

  // Helper to find a segment in a scope, traversing through library scopes
  // Returns nullptr if not found, or the found scope
  // Throws if ambiguous (same name found in multiple library scopes)
  std::function<SemanticScope*(const SemanticScope&, const std::string&)>
      findInScope = [&](const SemanticScope& scope,
                        const std::string& segment) -> SemanticScope* {
    // Direct child lookup — always succeeds for explicit library scope names
    // and for non-library-scope names
    auto it = scope.childModules.find(segment);
    if (it != scope.childModules.end()) {
      // If this IS a library scope name ($hash$), navigate into it directly
      if (isLibraryScope(segment)) {
        return it->second.get();
      }
      // Non-library direct child
      return it->second.get();
    }

    // Search inside library scopes (transparent lookup for non-library names)
    // Track all matches to detect ambiguity
    SemanticScope* found = nullptr;
    std::string foundInLib;

    for (const auto& [modName, child] : scope.childModules) {
      if (!child || !isLibraryScope(modName)) continue;

      // Check if this library scope has the segment as direct child
      auto childIt = child->childModules.find(segment);
      if (childIt != child->childModules.end()) {
        if (found) {
          // Ambiguity: same module name in multiple library scopes
          logAndThrowError("Ambiguous module reference '" + segment +
                           "': found in both library '" + foundInLib +
                           "' and '" + modName + "'");
        }
        found = childIt->second.get();
        foundInLib = modName;
      }

      // Also recursively check nested library scopes
      auto* nested = findInScope(*child, segment);
      if (nested) {
        if (found && found != nested) {
          logAndThrowError("Ambiguous module reference '" + segment +
                           "': found in multiple library scopes");
        }
        found = nested;
      }
    }
    return found;
  };

  // Start from global scope
  const SemanticScope* current = &scopeStack.front();

  // Split dot-separated path and traverse
  std::string segment;
  std::istringstream stream(dotPath);
  while (std::getline(stream, segment, '.')) {
    auto* found = findInScope(*current, segment);
    if (!found) return nullptr;
    current = found;
  }
  return const_cast<SemanticScope*>(current);
}

std::vector<UsingImport> SemanticAnalyzer::getActiveUsingImports() const {
  std::vector<UsingImport> result;
  for (const auto& scope : scopeStack) {
    result.insert(result.end(), scope.usingImports.begin(),
                  scope.usingImports.end());
  }
  return result;
}

// -------------------------------------------------------------------
// Unified Symbol Lookup
// Library scopes ($hash$) are transparent - we look through them
// Throws on ambiguity (same name in multiple library scopes)
// -------------------------------------------------------------------

SymbolMatch SemanticAnalyzer::findSymbol(const std::string& name,
                                         SymbolKind filterKind) const {
  std::vector<SymbolMatch> matches;

  // Helper to extract library hash from a module path
  auto extractLibraryHash = [](const std::string& path) -> std::string {
    if (path.empty()) return "";
    size_t dollarPos = path.find('$');
    if (dollarPos == std::string::npos) return "";
    size_t endDollar = path.find('$', dollarPos + 1);
    if (endDollar == std::string::npos) return "";
    return path.substr(dollarPos, endDollar - dollarPos + 1);
  };

  // Helper to check if a kind matches the filter
  auto matchesFilter = [filterKind](SymbolKind kind) {
    return filterKind == SymbolKind::None || filterKind == kind;
  };

  // Recursive helper to search a scope and its children
  // Library scopes are traversed transparently
  std::function<void(const SemanticScope&, const std::string&)> searchScope =
      [&](const SemanticScope& scope, const std::string& currentPath) {
        std::string libHash = extractLibraryHash(currentPath);

        // Check classes
        if (matchesFilter(SymbolKind::Class)) {
          auto classIt = scope.classes.find(name);
          if (classIt != scope.classes.end()) {
            SymbolMatch match;
            match.kind = SymbolKind::Class;
            match.name = name;
            match.modulePath = currentPath;
            match.libraryHash = libHash;
            match.classType = classIt->second;
            matches.push_back(match);
          }
        }

        // Check generic classes
        if (matchesFilter(SymbolKind::GenericClass)) {
          auto genClassIt = scope.genericClasses.find(name);
          if (genClassIt != scope.genericClasses.end()) {
            SymbolMatch match;
            match.kind = SymbolKind::GenericClass;
            match.name = name;
            match.modulePath = currentPath;
            match.libraryHash = libHash;
            match.genericClassInfo = &genClassIt->second;
            matches.push_back(match);
          }
        }

        // Check interfaces
        if (matchesFilter(SymbolKind::Interface)) {
          auto ifaceIt = scope.interfaces.find(name);
          if (ifaceIt != scope.interfaces.end()) {
            SymbolMatch match;
            match.kind = SymbolKind::Interface;
            match.name = name;
            match.modulePath = currentPath;
            match.libraryHash = libHash;
            match.interfaceType = ifaceIt->second;
            matches.push_back(match);
          }
        }

        // Check generic interfaces
        if (matchesFilter(SymbolKind::GenericInterface)) {
          auto genIfaceIt = scope.genericInterfaces.find(name);
          if (genIfaceIt != scope.genericInterfaces.end()) {
            SymbolMatch match;
            match.kind = SymbolKind::GenericInterface;
            match.name = name;
            match.modulePath = currentPath;
            match.libraryHash = libHash;
            match.genericInterfaceInfo = &genIfaceIt->second;
            matches.push_back(match);
          }
        }

        // Check enums
        if (matchesFilter(SymbolKind::Enum)) {
          auto enumIt = scope.enums.find(name);
          if (enumIt != scope.enums.end()) {
            SymbolMatch match;
            match.kind = SymbolKind::Enum;
            match.name = name;
            match.modulePath = currentPath;
            match.libraryHash = libHash;
            match.enumType = enumIt->second;
            matches.push_back(match);
          }
        }

        // Check functions (by prefix)
        if (matchesFilter(SymbolKind::Function)) {
          std::string funcPrefix = name + "(";
          for (const auto& [sig, info] : scope.functions) {
            if (sig.compare(0, funcPrefix.size(), funcPrefix) == 0) {
              SymbolMatch match;
              match.kind = SymbolKind::Function;
              match.name = name;
              match.modulePath = currentPath;
              match.libraryHash = libHash;
              match.functionInfo = &info;
              matches.push_back(match);
              break;  // Only need one function match per scope
            }
          }
        }

        // Recurse into child modules
        for (const auto& [modName, child] : scope.childModules) {
          if (!child) continue;
          std::string childPath =
              currentPath.empty() ? modName : currentPath + "." + modName;
          // Library scopes are transparent - continue search inside them
          if (isLibraryScope(modName)) {
            searchScope(*child, childPath);
          }
        }
      };

  // Search from each scope in the stack
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    searchScope(*it, it->modulePath);
  }

  // Check for ambiguity - same symbol from different library scopes
  if (matches.size() > 1) {
    // Remove duplicates (same libraryHash)
    std::vector<SymbolMatch> uniqueMatches;
    std::set<std::string> seenHashes;
    for (const auto& m : matches) {
      std::string key = m.libraryHash.empty() ? "__local__" : m.libraryHash;
      if (seenHashes.insert(key).second) {
        uniqueMatches.push_back(m);
      }
    }

    if (uniqueMatches.size() > 1) {
      std::string msg = "Ambiguous symbol '" + name + "': found in ";
      for (size_t i = 0; i < uniqueMatches.size(); ++i) {
        if (i > 0) msg += " and ";
        msg += uniqueMatches[i].display();
      }
      logAndThrowError(msg);
    }
    matches = uniqueMatches;
  }

  return matches.empty() ? SymbolMatch{} : matches[0];
}

SymbolMatch SemanticAnalyzer::findSymbolInModule(const std::string& modulePath,
                                                 const std::string& name,
                                                 SymbolKind filterKind) const {
  // Get full module path including library hashes
  std::string fullPath = getFullModulePath(modulePath);

  // Find the module scope
  SemanticScope* modScope = lookupModuleScope(fullPath);
  if (!modScope) {
    return SymbolMatch{};
  }

  std::string libHash;
  if (fullPath.size() >= 2 && fullPath.front() == '$') {
    size_t endDollar = fullPath.find('$', 1);
    if (endDollar != std::string::npos) {
      libHash = fullPath.substr(0, endDollar + 1);
    }
  }

  // Helper to check if a kind matches the filter
  auto matchesFilter = [filterKind](SymbolKind kind) {
    return filterKind == SymbolKind::None || filterKind == kind;
  };

  // Check classes
  if (matchesFilter(SymbolKind::Class)) {
    auto classIt = modScope->classes.find(name);
    if (classIt != modScope->classes.end()) {
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
    auto genClassIt = modScope->genericClasses.find(name);
    if (genClassIt != modScope->genericClasses.end()) {
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
    auto ifaceIt = modScope->interfaces.find(name);
    if (ifaceIt != modScope->interfaces.end()) {
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
    auto genIfaceIt = modScope->genericInterfaces.find(name);
    if (genIfaceIt != modScope->genericInterfaces.end()) {
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
    auto enumIt = modScope->enums.find(name);
    if (enumIt != modScope->enums.end()) {
      SymbolMatch match;
      match.kind = SymbolKind::Enum;
      match.name = name;
      match.modulePath = fullPath;
      match.libraryHash = libHash;
      match.enumType = enumIt->second;
      return match;
    }
  }

  // Check functions - need to look for mangled name in the scope
  // The function is registered with the full mangled path
  if (matchesFilter(SymbolKind::Function)) {
    std::string mangledPath = mangleModulePath(fullPath);
    std::string funcPrefix = mangledPath + "_" + name + "(";

    // Search the entire scope chain for the function
    for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
      for (const auto& [sig, info] : it->functions) {
        if (sig.compare(0, funcPrefix.size(), funcPrefix) == 0) {
          SymbolMatch match;
          match.kind = SymbolKind::Function;
          match.name = name;
          match.modulePath = fullPath;
          match.libraryHash = libHash;
          match.functionInfo = &info;
          return match;
        }
      }
    }
  }

  // Check namespaced variables
  if (matchesFilter(SymbolKind::Variable)) {
    std::string mangledPath = mangleModulePath(fullPath);
    std::string qualifiedVarName = mangledPath + "_" + name;
    for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
      auto varIt = it->namespacedVariables.find(qualifiedVarName);
      if (varIt != it->namespacedVariables.end()) {
        SymbolMatch match;
        match.kind = SymbolKind::Variable;
        match.name = name;
        match.modulePath = fullPath;
        match.libraryHash = libHash;
        match.variableInfo = &varIt->second;
        return match;
      }
    }
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
  for (const auto& scope : scopeStack) {
    if (scope.type == ScopeType::Global || scope.type == ScopeType::Module) {
      if (scope.variables.contains(name)) {
        logAndThrowError(
            "Cannot shadow " +
            std::string(scope.type == ScopeType::Global ? "global" : "module") +
            " variable '" + name + "'");
      }
    }
  }
  scopeStack.back().variables[name] = {
      type, static_cast<int>(scopeStack.size() - 1), isParam, false};
}

VariableInfo* SemanticAnalyzer::lookupVariable(const std::string& name) {
  // Search from innermost to outermost scope (handles shadowing)
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    auto found = it->variables.find(name);
    if (found != it->variables.end()) {
      return &found->second;
    }
  }
  return nullptr;
}

bool SemanticAnalyzer::isVariableLocal(const std::string& name) const {
  if (scopeStack.size() <= 1) return false;
  return scopeStack.back().variables.contains(name);
}

// -------------------------------------------------------------------
// Type narrowing (from _is<T> type guards)
// -------------------------------------------------------------------

void SemanticAnalyzer::narrowVariable(const std::string& varName,
                                      sun::TypePtr narrowedType) {
  if (scopeStack.empty()) return;
  scopeStack.back().narrowedTypes[varName] = std::move(narrowedType);
}

sun::TypePtr SemanticAnalyzer::getNarrowedType(
    const std::string& varName, sun::TypePtr originalType) const {
  // Search from innermost to outermost scope
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    auto found = it->narrowedTypes.find(varName);
    if (found != it->narrowedTypes.end()) {
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

void SemanticAnalyzer::registerFunction(const std::string& name,
                                        const FunctionInfo& info) {
  // For nested functions, prepend enclosing function context to create
  // unique qualified names. This ensures each generic instantiation of an
  // outer function gets its own copy of nested functions.
  // e.g., outer<i32>'s inner -> "outer(i32)::inner(i32)"
  std::string funcContext = getCurrentFunctionContext();
  std::string qualifiedName =
      funcContext.empty() ? name : funcContext + "::" + name;
  std::string sig = getFunctionSignature(qualifiedName, info.paramTypes);
  // Pass 1: detect redeclarations of the same function signature
  if (collectingDeclarations) {
    if (!scopeStack.empty() && scopeStack.front().functions.contains(sig)) {
      logAndThrowError("Cannot redeclare function '" + name +
                       "' with the same parameter types");
    }
  }
  // Register in current scope AND global scope (for reachability)
  if (!scopeStack.empty()) {
    scopeStack.back().functions[sig] = info;
    if (scopeStack.size() > 1) {
      scopeStack.front().functions[sig] = info;
    }
  }
}

const GenericFunctionInfo* SemanticAnalyzer::lookupGenericFunction(
    const std::string& name) const {
  std::string modPath = getCurrentModulePath();

  // Build QualifiedName with current module path
  sun::QualifiedName qname(modPath, "", name);

  // Walk scope chain from innermost to outermost, including child modules
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    auto found = it->genericFunctions.find(qname);
    if (found != it->genericFunctions.end()) {
      return &found->second;
    }
    // Try with current function context (for nested generic functions)
    std::string funcContext = getCurrentFunctionContext();
    if (!funcContext.empty()) {
      sun::QualifiedName nestedQname(modPath, funcContext, name);
      found = it->genericFunctions.find(nestedQname);
      if (found != it->genericFunctions.end()) {
        return &found->second;
      }
    }
    // If we're in a module, also try global scope (empty module path)
    if (!modPath.empty()) {
      sun::QualifiedName globalQname("", "", name);
      found = it->genericFunctions.find(globalQname);
      if (found != it->genericFunctions.end()) {
        return &found->second;
      }
    }
    // Search child module scopes
    for (const auto& [modName, child] : it->childModules) {
      if (!child) continue;
      auto childFound = child->genericFunctions.find(qname);
      if (childFound != child->genericFunctions.end()) {
        return &childFound->second;
      }
    }
  }

  return nullptr;
}

std::vector<FunctionInfo> SemanticAnalyzer::getAllFunctions(
    const std::string& name) const {
  std::vector<FunctionInfo> results;
  std::string prefix = name + "(";

  // Also try with current function context for nested functions
  std::string funcContext = getCurrentFunctionContext();
  std::string nestedPrefix;
  if (!funcContext.empty()) {
    nestedPrefix = funcContext + "::" + name + "(";
  }

  // Walk scope chain from innermost to outermost, including child modules
  for (auto sit = scopeStack.rbegin(); sit != scopeStack.rend(); ++sit) {
    sit->collectFunctions(prefix, results);
    if (!nestedPrefix.empty()) {
      sit->collectFunctions(nestedPrefix, results);
    }
  }
  return results;
}

std::optional<FunctionInfo> SemanticAnalyzer::lookupFunction(
    const std::string& name, const std::vector<sun::TypePtr>& argTypes) const {
  std::string sig = getFunctionSignature(name, argTypes);
  std::string funcContext = getCurrentFunctionContext();
  std::string nestedSig;
  if (!funcContext.empty()) {
    nestedSig = getFunctionSignature(funcContext + "::" + name, argTypes);
  }
  std::string prefix = name + "(";
  std::string nestedPrefix;
  if (!funcContext.empty()) {
    nestedPrefix = funcContext + "::" + name + "(";
  }

  // Helper lambda to check compatible overloads in a function map
  auto findCompatible = [&](const std::map<std::string, FunctionInfo>& funcs)
      -> std::optional<FunctionInfo> {
    // Try exact match first
    auto it = funcs.find(sig);
    if (it != funcs.end()) return it->second;
    if (!nestedSig.empty()) {
      it = funcs.find(nestedSig);
      if (it != funcs.end()) return it->second;
    }

    // Try compatible overload
    for (const auto& [funcSig, info] : funcs) {
      bool matchesPrefix = funcSig.compare(0, prefix.size(), prefix) == 0;
      bool matchesNestedPrefix =
          !nestedPrefix.empty() &&
          funcSig.compare(0, nestedPrefix.size(), nestedPrefix) == 0;
      if (!matchesPrefix && !matchesNestedPrefix) continue;
      if (info.paramTypes.size() != argTypes.size()) continue;

      bool compatible = true;
      for (size_t i = 0; i < argTypes.size(); ++i) {
        if (!argTypes[i] || !info.paramTypes[i]) {
          compatible = false;
          break;
        }
        if (info.paramTypes[i]->equals(*argTypes[i])) continue;

        if (info.paramTypes[i]->isReference()) {
          auto* refType =
              static_cast<const sun::ReferenceType*>(info.paramTypes[i].get());
          if (refType->getReferencedType()->equals(*argTypes[i])) continue;
          if (refType->getReferencedType()->isArray() &&
              argTypes[i]->isArray()) {
            auto* paramArray = static_cast<const sun::ArrayType*>(
                refType->getReferencedType().get());
            auto* argArray =
                static_cast<const sun::ArrayType*>(argTypes[i].get());
            if (paramArray->isUnsized() && paramArray->getElementType()->equals(
                                               *argArray->getElementType()))
              continue;
          }
        }

        if (argTypes[i]->isReference()) {
          auto* refType =
              static_cast<const sun::ReferenceType*>(argTypes[i].get());
          if (info.paramTypes[i]->equals(*refType->getReferencedType()))
            continue;
        }

        if (argTypes[i]->isNullPointer() &&
            info.paramTypes[i]->isAnyPointer()) {
          continue;
        }

        if (argTypes[i]->isStaticPointer() &&
            info.paramTypes[i]->isRawPointer()) {
          auto* staticPtr =
              static_cast<const sun::StaticPointerType*>(argTypes[i].get());
          auto* rawPtr =
              static_cast<const sun::RawPointerType*>(info.paramTypes[i].get());
          if (staticPtr->getPointeeType()->equals(*rawPtr->getPointeeType())) {
            continue;
          }
        }

        if (isAssignableTo(argTypes[i], info.paramTypes[i])) {
          continue;
        }

        compatible = false;
        break;
      }

      if (compatible) {
        return info;
      }
    }
    return std::nullopt;
  };

  // Helper: recursively search a scope and its childModules
  std::function<std::optional<FunctionInfo>(const SemanticScope&)> findInScope =
      [&](const SemanticScope& scope) -> std::optional<FunctionInfo> {
    auto result = findCompatible(scope.functions);
    if (result) return result;
    for (const auto& [modName, child] : scope.childModules) {
      if (child) {
        result = findInScope(*child);
        if (result) return result;
      }
    }
    return std::nullopt;
  };

  // Walk scope chain from innermost to outermost
  for (auto sit = scopeStack.rbegin(); sit != scopeStack.rend(); ++sit) {
    auto result = findInScope(*sit);
    if (result) return result;
  }

  return std::nullopt;
}

void SemanticAnalyzer::registerGlobal(const std::string& name,
                                      sun::TypePtr type) {
  scopeStack.front().variables[name] = {type, 0, false, false};
}

// -------------------------------------------------------------------
// Builtin function registration
// -------------------------------------------------------------------

void SemanticAnalyzer::registerBuiltinFunctions() {
  using sun::Types;

  // Low-level print intrinsics (used by stdlib print functions)
  registerFunction("_print_i32", {Types::Void(), {Types::Int32()}, {}});
  registerFunction("_print_i64", {Types::Void(), {Types::Int64()}, {}});
  registerFunction("_print_f64", {Types::Void(), {Types::Float64()}, {}});
  registerFunction("_print_newline", {Types::Void(), {}, {}});
  // _print_bytes intrinsic: write raw bytes to stdout
  registerFunction(
      "_print_bytes",
      {Types::Void(), {Types::RawPointer(Types::Int8()), Types::Int64()}, {}});
  // _println_str: print string literal with newline
  registerFunction("_println_str", {Types::Void(), {Types::String()}, {}});
  registerFunction("_println_str",
                   {Types::Void(), {Types::RawPointer(Types::UInt8())}, {}});

  // File I/O builtins
  registerFunction("file_open",
                   {Types::Int32(), {Types::String(), Types::Int32()}, {}});
  registerFunction("file_close", {Types::Int32(), {Types::Int32()}, {}});
  registerFunction("file_write",
                   {Types::Int32(), {Types::Int32(), Types::String()}, {}});
  registerFunction(
      "file_read",
      {Types::RawPointer(Types::Int8()), {Types::Int32(), Types::Int32()}, {}});

  // Low-level memory access intrinsics
  // _load_i64(ptr, index) - load i64 from ptr at byte offset index*8
  registerFunction(
      "_load_i64",
      {Types::Int64(), {Types::RawPointer(Types::Int8()), Types::Int64()}, {}});
  // _store_i64(ptr, index, value) - store i64 to ptr at byte offset index*8
  registerFunction("_store_i64", {Types::Void(),
                                  {Types::RawPointer(Types::Int8()),
                                   Types::Int64(), Types::Int64()},
                                  {}});

  // Memory allocation intrinsics
  // _malloc(size) - allocate size bytes, returns raw_ptr<i8>
  registerFunction("_malloc",
                   {Types::RawPointer(Types::Int8()), {Types::Int64()}, {}});
  // _free(ptr) - free previously allocated memory
  registerFunction("_free",
                   {Types::Void(), {Types::RawPointer(Types::Int8())}, {}});
}

// -------------------------------------------------------------------
// Namespace-qualified symbols (separate from scope-based lookup)
// -------------------------------------------------------------------

void SemanticAnalyzer::registerNamespacedVariable(
    const std::string& qualifiedName, sun::TypePtr type) {
  scopeStack.front().namespacedVariables[qualifiedName] = {type, 0, false};
  if (scopeStack.size() > 1) {
    scopeStack.back().namespacedVariables[qualifiedName] = {type, 0, false};
  }
}

VariableInfo* SemanticAnalyzer::lookupQualifiedVariable(
    const std::string& qualifiedName) {
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    auto found = it->namespacedVariables.find(qualifiedName);
    if (found != it->namespacedVariables.end()) {
      return &found->second;
    }
    // Also search child module scopes
    for (const auto& [modName, child] : it->childModules) {
      if (child) {
        auto cFound = child->namespacedVariables.find(qualifiedName);
        if (cFound != child->namespacedVariables.end()) {
          return &cFound->second;
        }
      }
    }
  }
  return nullptr;
}

// Helper: Get the full module path including library scope hashes
// e.g., "b" -> "$hash$.b" if b is inside a library scope
std::string SemanticAnalyzer::getFullModulePath(
    const std::string& visiblePath) const {
  if (visiblePath.empty() || scopeStack.empty()) return visiblePath;

  // Helper to find a segment and return its full path including library scopes
  // Throws on ambiguity (same name found in multiple library scopes)
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

      // Check if this library scope has the segment as direct child
      auto childIt = child->childModules.find(segment);
      if (childIt != child->childModules.end()) {
        std::string libPath =
            currentPath.empty() ? modName : currentPath + "." + modName;
        std::string candidate = libPath + "." + segment;
        if (!found.empty()) {
          logAndThrowError("Ambiguous module reference '" + segment +
                           "': found in both library '" + foundInLib +
                           "' and '" + modName + "'");
        }
        found = candidate;
        foundInLib = modName;
      }

      // Recursively check nested library scopes
      std::string libPath =
          currentPath.empty() ? modName : currentPath + "." + modName;
      auto result = findFullPath(*child, segment, libPath);
      if (!result.empty()) {
        if (!found.empty() && found != result) {
          logAndThrowError("Ambiguous module reference '" + segment +
                           "': found in multiple library scopes");
        }
        found = result;
      }
    }
    return found;
  };

  // Start from global scope and traverse the visible path
  std::string fullPath;
  const SemanticScope* current = &scopeStack.front();

  std::string segment;
  std::istringstream stream(visiblePath);
  while (std::getline(stream, segment, '.')) {
    // Find full path for this segment
    std::string segmentFullPath = findFullPath(*current, segment, fullPath);
    if (segmentFullPath.empty()) {
      // Segment not found, return original
      return visiblePath;
    }
    fullPath = segmentFullPath;

    // Navigate to that scope for next segment
    SemanticScope* nextScope = lookupModuleScope(fullPath);
    if (!nextScope) return visiblePath;
    current = nextScope;
  }

  return fullPath;
}

const FunctionInfo* SemanticAnalyzer::lookupQualifiedFunction(
    const std::string& qualifiedName) const {
  // First, try direct lookup with the given name
  std::string prefix = qualifiedName + "(";

  // Recursive helper to search a scope and its child modules
  std::function<const FunctionInfo*(const SemanticScope&)> searchScope =
      [&](const SemanticScope& scope) -> const FunctionInfo* {
    for (const auto& [sig, info] : scope.functions) {
      if (sig.compare(0, prefix.size(), prefix) == 0) {
        return &info;
      }
    }
    for (const auto& [modName, child] : scope.childModules) {
      if (child) {
        auto* result = searchScope(*child);
        if (result) return result;
      }
    }
    return nullptr;
  };

  // Walk scope chain with direct name
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    auto* result = searchScope(*it);
    if (result) return result;
  }

  // If not found, try resolving through library scopes
  // Split qualified name into module path and function name
  size_t lastDot = qualifiedName.rfind('.');
  if (lastDot != std::string::npos) {
    std::string modulePath = qualifiedName.substr(0, lastDot);
    std::string funcName = qualifiedName.substr(lastDot + 1);

    // Get full module path including library scope hashes
    std::string fullModulePath = getFullModulePath(modulePath);
    if (fullModulePath != modulePath) {
      // Mangle the full path: "$hash$.b" -> "$hash$_b"
      std::string mangledPath = mangleModulePath(fullModulePath);
      std::string fullQualifiedName = mangledPath + "_" + funcName;
      prefix = fullQualifiedName + "(";

      // Search again with resolved name
      for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
        auto* result = searchScope(*it);
        if (result) return result;
      }
    }
  }

  return nullptr;
}

sun::QualifiedName SemanticAnalyzer::resolveNameWithUsings(
    const std::string& name) const {
  // Helper to check if a symbol exists (function, variable, or class)
  // Searches scope chain including child module scopes
  auto symbolExists = [this](const std::string& candidate) -> bool {
    // Check scope chain, including child module scopes via hasSymbol
    // and namespaced variables
    for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
      if (it->namespacedVariables.contains(candidate)) return true;
      if (it->hasSymbol(candidate)) return true;
    }
    return false;
  };

  // First, if we're inside a module, check for the name with module prefix
  // This allows code inside `module sun { }` to reference other symbols
  // in the same module without needing `using sun;`
  // Also check parent module prefixes: A_B_foo, then A_foo
  std::string modulePath = getCurrentModulePath();
  while (!modulePath.empty()) {
    sun::QualifiedName candidate(modulePath, name);
    if (symbolExists(candidate.mangled())) {
      return candidate;
    }
    // Try parent module: "A.B" -> "A"
    size_t lastDot = modulePath.rfind('.');
    if (lastDot == std::string::npos) {
      break;  // No more parent modules
    }
    modulePath = modulePath.substr(0, lastDot);
  }

  // Get all active using imports from scope stack
  auto activeImports = getActiveUsingImports();

  // Collect all matching candidates from using imports
  std::vector<sun::QualifiedName> matches;
  for (const auto& import : activeImports) {
    sun::QualifiedName candidate;
    // Resolve to full path including library hash prefixes
    std::string fullNsPath = getFullModulePath(import.namespacePath);
    if (import.isWildcard) {
      candidate = sun::QualifiedName(fullNsPath, name);
    } else if (import.isPrefixWildcard) {
      if (name.size() >= import.prefix.size() &&
          name.substr(0, import.prefix.size()) == import.prefix) {
        candidate = sun::QualifiedName(fullNsPath, name);
      }
    } else if (import.target == name) {
      candidate = sun::QualifiedName(fullNsPath, name);
    }

    if (!candidate.empty() && symbolExists(candidate.mangled())) {
      // Only add if not already in matches (same candidate from different
      // imports)
      if (std::find(matches.begin(), matches.end(), candidate) ==
          matches.end()) {
        matches.push_back(candidate);
      }
    }
  }

  // Also check scope-based ImportBindings (new system, for local modules)
  for (const auto& scope : scopeStack) {
    for (const auto& binding : scope.importBindings) {
      if (!binding.sourceScope) continue;
      if (binding.isWildcard) {
        if (binding.sourceScope->hasSymbol(name)) {
          sun::QualifiedName candidate(binding.sourceScope->modulePath, name);
          if (std::find(matches.begin(), matches.end(), candidate) ==
              matches.end()) {
            matches.push_back(candidate);
          }
        }
      } else if (binding.localName == name) {
        if (binding.sourceScope->hasSymbol(binding.sourceName)) {
          sun::QualifiedName candidate(binding.sourceScope->modulePath,
                                       binding.sourceName);
          if (std::find(matches.begin(), matches.end(), candidate) ==
              matches.end()) {
            matches.push_back(candidate);
          }
        }
      }
    }
  }

  if (matches.size() > 1) {
    std::string msg = "Ambiguous reference to '" + name + "'. Could be: ";
    for (size_t i = 0; i < matches.size(); ++i) {
      if (i > 0) msg += " or ";
      msg += matches[i].display();
    }
    logAndThrowError(msg);
  }

  if (matches.size() == 1) {
    // Check if the bare (unqualified) name also exists as a direct symbol
    // (not via childModules — those are exactly what 'using' imports resolve).
    // Only flag ambiguity for true top-level symbols conflicting with an
    // import.
    auto directSymbolExists = [this](const std::string& candidate) -> bool {
      for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
        if (it->namespacedVariables.contains(candidate)) return true;
        if (it->classes.contains(candidate)) return true;
        if (it->genericClasses.contains(candidate)) return true;
        if (it->interfaces.contains(candidate)) return true;
        if (it->genericInterfaces.contains(candidate)) return true;
        if (it->enums.contains(candidate)) return true;
        std::string prefix = candidate + "(";
        for (const auto& [sig, info] : it->functions) {
          if (sig.compare(0, prefix.size(), prefix) == 0) return true;
        }
      }
      return false;
    };
    if (directSymbolExists(name)) {
      std::string msg = "Ambiguous reference to '" + name +
                        "'. Could be: " + name + " or " + matches[0].display();
      logAndThrowError(msg);
    }
    return matches[0];
  }

  // Return unqualified name if no using import applies
  return sun::QualifiedName("", name);
}

void SemanticAnalyzer::addUsingImport(const UsingImport& import) {
  // Add using import to the current scope
  if (!scopeStack.empty()) {
    scopeStack.back().usingImports.push_back(import);
  }
}

void SemanticAnalyzer::addImportBinding(const ImportBinding& binding) {
  if (!scopeStack.empty()) {
    scopeStack.back().importBindings.push_back(binding);
  }
}
