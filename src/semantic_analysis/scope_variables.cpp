// semantic_analysis/scope_variables.cpp — Scope and variable management

#include <algorithm>
#include <cstdint>
#include <functional>
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

  // Also check with qualified name prefix
  if (!modulePath.empty()) {
    std::string mangledPath = modulePath;
    for (char& c : mangledPath) {
      if (c == '.') c = '_';
    }
    std::string qualifiedName = mangledPath + "_" + name;
    if (functions.hasName(qualifiedName)) return true;
    if (classes.contains(qualifiedName)) return true;
    if (genericClasses.contains(qualifiedName)) return true;
    if (interfaces.contains(qualifiedName)) return true;
    if (genericInterfaces.contains(qualifiedName)) return true;
  }

  return false;
}

std::shared_ptr<sun::ClassType> SemanticScope::findClass(
    const std::string& name) const {
  auto it = classes.find(name);
  if (it != classes.end()) return it->second;
  // Also try qualified name if this scope has a module path (library hash)
  if (!modulePath.empty()) {
    std::string mangledPath = modulePath;
    for (char& c : mangledPath) {
      if (c == '.') c = '_';
    }
    std::string qualifiedName = mangledPath + "_" + name;
    it = classes.find(qualifiedName);
    if (it != classes.end()) return it->second;
  }
  return nullptr;
}

const GenericClassInfo* SemanticScope::findGenericClass(
    const std::string& name) const {
  auto it = genericClasses.find(name);
  if (it != genericClasses.end()) return &it->second;
  // Also try qualified name if this scope has a module path (library hash)
  if (!modulePath.empty()) {
    std::string mangledPath = modulePath;
    for (char& c : mangledPath) {
      if (c == '.') c = '_';
    }
    std::string qualifiedName = mangledPath + "_" + name;
    auto it2 = genericClasses.find(qualifiedName);
    if (it2 != genericClasses.end()) return &it2->second;
  }
  return nullptr;
}

std::shared_ptr<sun::InterfaceType> SemanticScope::findInterface(
    const std::string& name) const {
  auto it = interfaces.find(name);
  if (it != interfaces.end()) return it->second;
  // Also try qualified name if this scope has a module path (library hash)
  if (!modulePath.empty()) {
    std::string mangledPath = modulePath;
    for (char& c : mangledPath) {
      if (c == '.') c = '_';
    }
    std::string qualifiedName = mangledPath + "_" + name;
    it = interfaces.find(qualifiedName);
    if (it != interfaces.end()) return it->second;
  }
  return nullptr;
}

const GenericInterfaceInfo* SemanticScope::findGenericInterface(
    const std::string& name) const {
  auto it = genericInterfaces.find(name);
  if (it != genericInterfaces.end()) return &it->second;
  // Also try qualified name if this scope has a module path (library hash)
  if (!modulePath.empty()) {
    std::string mangledPath = modulePath;
    for (char& c : mangledPath) {
      if (c == '.') c = '_';
    }
    std::string qualifiedName = mangledPath + "_" + name;
    auto it2 = genericInterfaces.find(qualifiedName);
    if (it2 != genericInterfaces.end()) return &it2->second;
  }
  return nullptr;
}

std::shared_ptr<sun::EnumType> SemanticScope::findEnum(
    const std::string& name) const {
  auto it = enums.find(name);
  if (it != enums.end()) return it->second;
  // Also try qualified name if this scope has a module path (library hash)
  if (!modulePath.empty()) {
    std::string mangledPath = modulePath;
    for (char& c : mangledPath) {
      if (c == '.') c = '_';
    }
    std::string qualifiedName = mangledPath + "_" + name;
    auto it2 = enums.find(qualifiedName);
    if (it2 != enums.end()) return it2->second;
  }
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

void SemanticAnalyzer::enterScope(ScopeType type) {
  auto child = std::make_shared<SemanticScope>(type);
  child->parent = currentScope;
  currentScope->children.push_back(child);
  currentScope = child.get();
}

void SemanticAnalyzer::enterModuleScope(const std::string& moduleName) {
  // Create or reuse a child module scope in the current scope's tree
  auto& child = currentScope->childModules[moduleName];
  if (!child) {
    child = std::make_shared<SemanticScope>(ScopeType::Module, moduleName);
    child->parent = currentScope;
    // Compute full dot-separated module path
    std::string parentPath = currentScope->modulePath;
    child->modulePath =
        parentPath.empty() ? moduleName : parentPath + "." + moduleName;
  }
  currentScope = child.get();
}

void SemanticAnalyzer::enterFunctionScope(const std::string& funcSig) {
  auto child = std::make_shared<SemanticScope>(ScopeType::Function);
  child->functionSignature = funcSig;
  child->parent = currentScope;
  currentScope->children.push_back(child);
  currentScope = child.get();
}

void SemanticAnalyzer::enterImportScope(const std::string& sourceFile,
                                        const std::string& contentHash) {
  // For .moon imports with a content hash, use the hash as scope name.
  // This unifies the import scope with the library hash scope — the content
  // hash provides both symbol isolation (via modulePath) and diamond-dep dedup.
  // For .sun imports (no content hash), use a hash of the source file path.
  std::string scopeName;
  if (!contentHash.empty()) {
    scopeName = contentHash;
  } else {
    auto hash = std::hash<std::string>{}(sourceFile);
    scopeName = "$import_" + std::to_string(hash & 0xFFFFFFFF) + "$";
  }

  // Reuse existing import scope if same file already imported (diamond deps)
  auto it = currentScope->childModules.find(scopeName);
  if (it != currentScope->childModules.end()) {
    currentScope = it->second.get();
    return;
  }

  auto child = std::make_shared<SemanticScope>(ScopeType::Import, scopeName);
  child->parent = currentScope;

  if (!contentHash.empty()) {
    // .moon import: content hash contributes to modulePath for symbol mangling
    child->modulePath = contentHash;
  } else {
    // .sun import: transparent for module path (inherit parent's)
    child->modulePath = currentScope->modulePath;
  }

  currentScope->childModules[scopeName] = child;
  currentScope = child.get();
}

void SemanticAnalyzer::exitScope() {
  if (currentScope->parent) {
    auto* parent = currentScope->parent;
    auto type = currentScope->type;

    // For transient scopes (Function, Block, Class), remove from parent's
    // children list to free memory. Module and Import scopes are persistent.
    if (type == ScopeType::Function || type == ScopeType::Block ||
        type == ScopeType::Class) {
      auto& children = parent->children;
      for (auto it = children.begin(); it != children.end(); ++it) {
        if (it->get() == currentScope) {
          children.erase(it);
          break;
        }
      }
    }

    currentScope = parent;
  }
}

std::string SemanticAnalyzer::getCurrentModulePrefix() const {
  // Find the nearest scope with a module path and mangle it to underscore form
  // modulePath is pre-computed (e.g., "$hash$.sun") so we just convert dots
  std::string path = getCurrentModulePath();
  if (path.empty()) return "";
  for (char& c : path) {
    if (c == '.') c = '_';
  }
  return path + "_";
}

std::string SemanticAnalyzer::getCurrentModulePath() const {
  // Walk up to find the nearest Module or Import scope with a modulePath
  // The modulePath field already accumulates the full dot-separated path
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    if ((s->type == ScopeType::Module || s->type == ScopeType::Import) &&
        !s->modulePath.empty()) {
      return s->modulePath;
    }
  }
  return "";
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

std::string SemanticAnalyzer::getCurrentFunctionContext() const {
  // Build context from all enclosing function scopes' signatures
  // Need root-to-current order, so collect ancestors then reverse
  std::string context;
  std::vector<const SemanticScope*> ancestors;
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    ancestors.push_back(s);
  }
  for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
    if ((*it)->type == ScopeType::Function &&
        !(*it)->functionSignature.empty()) {
      if (!context.empty()) context += "::";
      context += (*it)->functionSignature;
    }
  }
  return context;
}

void SemanticAnalyzer::registerModule(const std::string& modulePath) {
  rootScope->declaredModules.insert(modulePath);
  if (currentScope != rootScope.get()) {
    currentScope->declaredModules.insert(modulePath);
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

  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    if (checkScope(*s)) return true;
  }
  return false;
}

SemanticScope* SemanticAnalyzer::lookupModuleScope(
    const std::string& dotPath) const {
  if (dotPath.empty() || !currentScope) return nullptr;

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

    // Search inside library/import scopes (transparent lookup)
    // Track all matches to detect ambiguity (import scopes don't count for
    // ambiguity — only real library scopes with different hashes do)
    SemanticScope* found = nullptr;
    std::string foundInLib;

    for (const auto& [modName, child] : scope.childModules) {
      if (!child || !isLibraryScope(modName)) continue;

      // Check if this library scope has the segment as direct child
      auto childIt = child->childModules.find(segment);
      if (childIt != child->childModules.end()) {
        if (found && !isImportScope(modName) && !isImportScope(foundInLib)) {
          // Ambiguity: same module name in multiple real library scopes
          logAndThrowError("Ambiguous module reference '" + segment +
                           "': found in both library '" + foundInLib +
                           "' and '" + modName + "'");
        }
        if (!found) {
          found = childIt->second.get();
          foundInLib = modName;
        }
      }

      // Also recursively check nested library scopes
      auto* nested = findInScope(*child, segment);
      if (nested) {
        if (found && found != nested && !isImportScope(modName) &&
            !isImportScope(foundInLib)) {
          logAndThrowError("Ambiguous module reference '" + segment +
                           "': found in multiple library scopes");
        }
        if (!found) {
          found = nested;
          foundInLib = modName;
        }
      }
    }
    return found;
  };

  // Search from currentScope upward so that modules
  // registered inside function bodies (via scoped imports) are found.
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    const SemanticScope* current = s;

    // Split dot-separated path and traverse
    std::string segment;
    std::istringstream stream(dotPath);
    bool resolved = true;
    while (std::getline(stream, segment, '.')) {
      auto* found = findInScope(*current, segment);
      if (!found) {
        resolved = false;
        break;
      }
      current = found;
    }
    if (resolved) return const_cast<SemanticScope*>(current);
  }
  return nullptr;
}

std::vector<UsingImport> SemanticAnalyzer::getActiveUsingImports() const {
  std::vector<UsingImport> result;
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    result.insert(result.end(), s->usingImports.begin(), s->usingImports.end());
  }
  return result;
}

// -------------------------------------------------------------------
// Unified Symbol Lookup
// Library scopes ($hash$) are transparent - we look through them
// Throws on ambiguity (same name in multiple library scopes)
// -------------------------------------------------------------------

// Helper: collect ALL module scopes matching a path across import scopes.
// This handles the case where two .sun imports define the same module name.
static std::vector<SemanticScope*> collectAllModuleScopes(
    const SemanticScope* startScope, const std::string& dotPath) {
  std::vector<SemanticScope*> results;
  if (dotPath.empty() || !startScope) return results;

  // Helper lambda to find ALL scopes for a segment (not just the first)
  std::function<void(const SemanticScope&, const std::string&,
                     std::vector<SemanticScope*>&)>
      findAllInScope = [&](const SemanticScope& scope,
                           const std::string& segment,
                           std::vector<SemanticScope*>& out) {
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

  // For single-segment paths, collect all matching scopes
  // For multi-segment paths (A.B), we collect all "A" scopes then search
  // each for "B"
  std::vector<SemanticScope*> currentScopes;

  // Start from each ancestor scope
  for (auto* s = startScope; s != nullptr; s = s->parent) {
    std::string segment;
    std::istringstream stream(dotPath);
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
      results.insert(results.end(), currentScopes.begin(), currentScopes.end());
      break;  // Found in this ancestor level, stop searching up
    }
  }
  return results;
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
    std::string qualifiedFuncName = mangledPath + "_" + name;

    // First search directly in the already-resolved module scope.
    // This handles library-hash-prefixed paths (e.g., "$hash$.b") where
    // collectAllModuleScopes fails due to library scope filtering.
    if (auto* overloads = modScope->functions.getOverloads(qualifiedFuncName)) {
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

    // Search in ALL matching module scopes (handles same module defined
    // across multiple .sun imports), then fall back to the scope chain.
    auto allScopes = collectAllModuleScopes(currentScope, modulePath);
    for (auto* scope : allScopes) {
      if (auto* overloads = scope->functions.getOverloads(qualifiedFuncName)) {
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
    // Fall back to scope chain (for globally-registered functions)
    for (auto* s = currentScope; s != nullptr; s = s->parent) {
      if (auto* overloads = s->functions.getOverloads(qualifiedFuncName)) {
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
  }

  // Check namespaced variables
  if (matchesFilter(SymbolKind::Variable)) {
    std::string mangledPath = mangleModulePath(fullPath);
    std::string qualifiedVarName = mangledPath + "_" + name;

    // First search directly in the already-resolved module scope
    auto varIt = modScope->namespacedVariables.find(qualifiedVarName);
    if (varIt != modScope->namespacedVariables.end()) {
      SymbolMatch match;
      match.kind = SymbolKind::Variable;
      match.name = name;
      match.modulePath = fullPath;
      match.libraryHash = libHash;
      match.variableInfo = &varIt->second;
      return match;
    }

    // Search all matching module scopes first
    auto allScopes = collectAllModuleScopes(currentScope, modulePath);
    for (auto* scope : allScopes) {
      varIt = scope->namespacedVariables.find(qualifiedVarName);
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
    // Fall back to scope chain
    for (auto* s = currentScope; s != nullptr; s = s->parent) {
      auto varIt = s->namespacedVariables.find(qualifiedVarName);
      if (varIt != s->namespacedVariables.end()) {
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
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    if (s->type == ScopeType::Global || s->type == ScopeType::Module) {
      if (s->variables.contains(name)) {
        logAndThrowError(
            "Cannot shadow " +
            std::string(s->type == ScopeType::Global ? "global" : "module") +
            " variable '" + name + "'");
      }
    }
  }
  // Compute scope depth by counting ancestors
  int depth = 0;
  for (auto* s = currentScope; s != nullptr; s = s->parent) depth++;
  currentScope->variables[name] = {type, depth - 1, isParam, false};
}

VariableInfo* SemanticAnalyzer::lookupVariable(const std::string& name) {
  // Search from innermost to outermost scope (handles shadowing)
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    auto found = s->variables.find(name);
    if (found != s->variables.end()) {
      return &found->second;
    }
  }
  return nullptr;
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

void SemanticAnalyzer::registerFunction(const std::string& name,
                                        const FunctionInfo& info) {
  // For nested functions, prepend enclosing function context to create
  // unique qualified names. This ensures each generic instantiation of an
  // outer function gets its own copy of nested functions.
  // e.g., outer<i32>'s inner -> "outer(i32)::inner(i32)"
  // Skip function context for imported functions (inside import scopes)
  // since those are module-level functions regardless of where the import
  // appears in the source.
  std::string funcContext =
      importScopeDepth_ > 0 ? "" : getCurrentFunctionContext();
  std::string qualifiedName =
      funcContext.empty() ? name : funcContext + "::" + name;
  std::string sig = getFunctionSignature(qualifiedName, info.paramTypes);
  // Pass 1: detect redeclarations of the same function signature
  if (collectingDeclarations) {
    if (currentScope->functions.contains(sig)) {
      // Allow re-registration from duplicate imports (diamond deps)
      if (importScopeDepth_ > 0) return;
      logAndThrowError("Cannot redeclare function '" + name +
                       "' with the same parameter types");
    }
  }
  // Register in current scope
  currentScope->functions[sig] = info;
}

const GenericFunctionInfo* SemanticAnalyzer::lookupGenericFunction(
    const std::string& name) const {
  std::string modPath = getCurrentModulePath();

  // Build QualifiedName with current module path
  sun::QualifiedName qname(modPath, "", name);

  // Walk scope chain from innermost to outermost, including child modules
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    auto found = s->genericFunctions.find(qname);
    if (found != s->genericFunctions.end()) {
      return &found->second;
    }
    // Try with current function context (for nested generic functions)
    std::string funcContext = getCurrentFunctionContext();
    if (!funcContext.empty()) {
      sun::QualifiedName nestedQname(modPath, funcContext, name);
      found = s->genericFunctions.find(nestedQname);
      if (found != s->genericFunctions.end()) {
        return &found->second;
      }
    }
    // If we're in a module, also try global scope (empty module path)
    if (!modPath.empty()) {
      sun::QualifiedName globalQname("", "", name);
      found = s->genericFunctions.find(globalQname);
      if (found != s->genericFunctions.end()) {
        return &found->second;
      }
    }
    // Search direct import-scope children (one level of transparency)
    for (const auto& [modName, child] : s->childModules) {
      if (!child || child->type != ScopeType::Import) continue;
      auto childFound = child->genericFunctions.find(qname);
      if (childFound != child->genericFunctions.end()) {
        return &childFound->second;
      }
      // Also search import scope's non-import children (modules within)
      for (const auto& [subName, subChild] : child->childModules) {
        if (!subChild || subChild->type == ScopeType::Import) continue;
        auto subFound = subChild->genericFunctions.find(qname);
        if (subFound != subChild->genericFunctions.end()) {
          return &subFound->second;
        }
      }
    }
    // Search import bindings from using statements
    for (const auto& binding : s->importBindings) {
      if (!binding.sourceScope || !binding.isWildcard) continue;
      auto bindingFound = binding.sourceScope->genericFunctions.find(qname);
      if (bindingFound != binding.sourceScope->genericFunctions.end()) {
        return &bindingFound->second;
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

  // Walk scope chain from innermost to outermost
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    s->collectFunctions(prefix, results);
    if (!nestedPrefix.empty()) {
      s->collectFunctions(nestedPrefix, results);
    }
    // Search direct import-scope children (one level of transparency)
    for (const auto& [childName, child] : s->childModules) {
      if (child && child->type == ScopeType::Import) {
        child->collectFunctions(prefix, results);
        if (!nestedPrefix.empty()) {
          child->collectFunctions(nestedPrefix, results);
        }
        // Also search import scope's module children
        for (const auto& [modName, modChild] : child->childModules) {
          if (modChild && modChild->type == ScopeType::Module) {
            modChild->collectFunctions(prefix, results);
            if (!nestedPrefix.empty()) {
              modChild->collectFunctions(nestedPrefix, results);
            }
          }
        }
      }
    }
    // Search import bindings from using statements
    for (const auto& binding : s->importBindings) {
      if (!binding.sourceScope) continue;
      // Search source scope for both wildcard and specific bindings.
      // For specific bindings, the caller already resolved the qualified name
      // (e.g., "math_square"), so the prefix match naturally filters correctly.
      binding.sourceScope->collectFunctions(prefix, results);
      if (!nestedPrefix.empty()) {
        binding.sourceScope->collectFunctions(nestedPrefix, results);
      }
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

  // Helper lambda to check compatible overloads in a FunctionTable
  auto findCompatible =
      [&](const FunctionTable& funcs) -> std::optional<FunctionInfo> {
    // Try exact match first
    auto it = funcs.find(sig);
    if (it != funcs.end()) return it->second;
    if (!nestedSig.empty()) {
      it = funcs.find(nestedSig);
      if (it != funcs.end()) return it->second;
    }

    // Try compatible overload — look up by base name
    auto checkOverloads =
        [&](const std::string& baseName) -> std::optional<FunctionInfo> {
      auto* overloads = funcs.getOverloads(baseName);
      if (!overloads) return std::nullopt;
      for (const auto* info : *overloads) {
        if (info->paramTypes.size() != argTypes.size()) continue;

        bool compatible = true;
        for (size_t i = 0; i < argTypes.size(); ++i) {
          if (!argTypes[i] || !info->paramTypes[i]) {
            compatible = false;
            break;
          }
          if (info->paramTypes[i]->equals(*argTypes[i])) continue;

          if (info->paramTypes[i]->isReference()) {
            auto* refType = static_cast<const sun::ReferenceType*>(
                info->paramTypes[i].get());
            if (refType->getReferencedType()->equals(*argTypes[i])) continue;
            if (refType->getReferencedType()->isArray() &&
                argTypes[i]->isArray()) {
              auto* paramArray = static_cast<const sun::ArrayType*>(
                  refType->getReferencedType().get());
              auto* argArray =
                  static_cast<const sun::ArrayType*>(argTypes[i].get());
              if (paramArray->isUnsized() &&
                  paramArray->getElementType()->equals(
                      *argArray->getElementType()))
                continue;
            }
          }

          if (argTypes[i]->isReference()) {
            auto* refType =
                static_cast<const sun::ReferenceType*>(argTypes[i].get());
            if (info->paramTypes[i]->equals(*refType->getReferencedType()))
              continue;
          }

          if (argTypes[i]->isNullPointer() &&
              info->paramTypes[i]->isAnyPointer()) {
            continue;
          }

          if (argTypes[i]->isStaticPointer() &&
              info->paramTypes[i]->isRawPointer()) {
            auto* staticPtr =
                static_cast<const sun::StaticPointerType*>(argTypes[i].get());
            auto* rawPtr = static_cast<const sun::RawPointerType*>(
                info->paramTypes[i].get());
            if (staticPtr->getPointeeType()->equals(
                    *rawPtr->getPointeeType())) {
              continue;
            }
          }

          if (isAssignableTo(argTypes[i], info->paramTypes[i])) {
            continue;
          }

          compatible = false;
          break;
        }

        if (compatible) {
          return *info;
        }
      }
      return std::nullopt;
    };

    // Check with base name
    std::string baseName = prefix.substr(0, prefix.size() - 1);
    auto result = checkOverloads(baseName);
    if (result) return result;

    // Check with nested name
    if (!nestedPrefix.empty()) {
      std::string nestedBaseName =
          nestedPrefix.substr(0, nestedPrefix.size() - 1);
      result = checkOverloads(nestedBaseName);
      if (result) return result;
    }

    return std::nullopt;
  };

  // Helper: search a scope's function map (no child recursion)
  auto findInScope =
      [&](const SemanticScope& scope) -> std::optional<FunctionInfo> {
    return findCompatible(scope.functions);
  };

  // Walk scope chain from innermost to outermost
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    auto result = findInScope(*s);
    if (result) return result;
    // Search direct import-scope children (one level of transparency)
    for (const auto& [childName, child] : s->childModules) {
      if (child && child->type == ScopeType::Import) {
        result = findInScope(*child);
        if (result) return result;
        // Also search import scope's module children
        for (const auto& [modName, modChild] : child->childModules) {
          if (modChild && modChild->type == ScopeType::Module) {
            result = findInScope(*modChild);
            if (result) return result;
          }
        }
      }
    }
    // Search import bindings from using statements
    for (const auto& binding : s->importBindings) {
      if (!binding.sourceScope) continue;
      if (binding.isWildcard) {
        result = findInScope(*binding.sourceScope);
        if (result) return result;
      }
    }
  }

  return std::nullopt;
}

void SemanticAnalyzer::registerGlobal(const std::string& name,
                                      sun::TypePtr type) {
  rootScope->variables[name] = {type, 0, false, false};
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
  rootScope->namespacedVariables[qualifiedName] = {type, 0, false};
  if (currentScope != rootScope.get()) {
    currentScope->namespacedVariables[qualifiedName] = {type, 0, false};
  }
}

VariableInfo* SemanticAnalyzer::lookupQualifiedVariable(
    const std::string& qualifiedName) {
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    auto found = s->namespacedVariables.find(qualifiedName);
    if (found != s->namespacedVariables.end()) {
      return &found->second;
    }
    // Also search child module scopes
    for (const auto& [modName, child] : s->childModules) {
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

      // Import scopes are transparent — traverse through them without
      // adding their name to the path
      std::string libPath;
      if (isImportScope(modName)) {
        libPath = currentPath;
      } else {
        libPath = currentPath.empty() ? modName : currentPath + "." + modName;
      }

      // Check if this library scope has the segment as direct child
      auto childIt = child->childModules.find(segment);
      if (childIt != child->childModules.end()) {
        std::string candidate =
            libPath.empty() ? segment : libPath + "." + segment;
        if (!found.empty() && found != candidate) {
          logAndThrowError("Ambiguous module reference '" + segment +
                           "': found in both library '" + foundInLib +
                           "' and '" + modName + "'");
        }
        found = candidate;
        foundInLib = modName;
      }

      // Recursively check nested library scopes
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
  // First, try direct lookup with the given name

  // Recursive helper to search a scope and its non-import child modules
  std::function<const FunctionInfo*(const SemanticScope&, const std::string&)>
      searchScope = [&](const SemanticScope& scope,
                        const std::string& lookupName) -> const FunctionInfo* {
    if (auto* overloads = scope.functions.getOverloads(lookupName)) {
      if (!overloads->empty()) return overloads->front();
    }
    for (const auto& [modName, child] : scope.childModules) {
      if (child && child->type != ScopeType::Import) {
        auto* result = searchScope(*child, lookupName);
        if (result) return result;
      }
    }
    return nullptr;
  };

  // Walk scope chain with direct name
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    auto* result = searchScope(*s, qualifiedName);
    if (result) return result;
    // Search direct import-scope children (one level of transparency)
    for (const auto& [childName, child] : s->childModules) {
      if (child && child->type == ScopeType::Import) {
        result = searchScope(*child, qualifiedName);
        if (result) return result;
      }
    }
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

      // Search again with resolved name
      for (auto* s = currentScope; s != nullptr; s = s->parent) {
        auto* result = searchScope(*s, fullQualifiedName);
        if (result) return result;
        for (const auto& [childName, child] : s->childModules) {
          if (child && child->type == ScopeType::Import) {
            result = searchScope(*child, fullQualifiedName);
            if (result) return result;
          }
        }
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
    // Check scope chain, including import scope module children
    for (auto* s = currentScope; s != nullptr; s = s->parent) {
      if (s->namespacedVariables.contains(candidate)) return true;
      if (s->hasSymbol(candidate)) return true;
      // Search direct import-scope children (one level of transparency)
      for (const auto& [childName, child] : s->childModules) {
        if (child && child->type == ScopeType::Import) {
          if (child->hasSymbol(candidate)) return true;
          // Also search import scope's module children
          for (const auto& [modName, modChild] : child->childModules) {
            if (modChild && modChild->type == ScopeType::Module) {
              if (modChild->hasSymbol(candidate)) return true;
            }
          }
        }
      }
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
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    for (const auto& binding : s->importBindings) {
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
      for (auto* s = currentScope; s != nullptr; s = s->parent) {
        if (s->namespacedVariables.contains(candidate)) return true;
        if (s->classes.contains(candidate)) return true;
        if (s->genericClasses.contains(candidate)) return true;
        if (s->interfaces.contains(candidate)) return true;
        if (s->genericInterfaces.contains(candidate)) return true;
        if (s->enums.contains(candidate)) return true;
        if (s->functions.hasName(candidate)) return true;
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
