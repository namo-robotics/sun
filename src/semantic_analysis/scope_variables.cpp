// semantic_analysis/scope_variables.cpp — Scope and variable management

#include <algorithm>
#include <cstdint>
#include <sstream>

#include "error.h"
#include "semantic_analyzer.h"

using sun::unwrapRef;

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
  std::string prefix = name + "(";
  for (const auto& [sig, info] : functions) {
    if (sig.compare(0, prefix.size(), prefix) == 0) return true;
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
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    if (it->declaredModules.count(name) > 0) return true;
    for (const auto& [modName, child] : it->childModules) {
      if (child && child->declaredModules.count(name) > 0) return true;
    }
  }
  return false;
}

SemanticScope* SemanticAnalyzer::lookupModuleScope(
    const std::string& dotPath) const {
  if (dotPath.empty() || scopeStack.empty()) return nullptr;
  // Start from global scope (first in stack)
  const SemanticScope* current = &scopeStack.front();
  // Split dot-separated path and traverse childModules
  std::string segment;
  std::istringstream stream(dotPath);
  while (std::getline(stream, segment, '.')) {
    auto it = current->childModules.find(segment);
    if (it == current->childModules.end()) return nullptr;
    current = it->second.get();
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
  // During Pass 2, skip re-registration of functions already declared in Pass 1
  if (!scopeStack.empty() && scopeStack.front().functions.contains(sig)) {
    if (!collectingDeclarations) return;  // Pass 2: skip
    logAndThrowError("Cannot redeclare function '" + name +
                     "' with the same parameter types");
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

const FunctionInfo* SemanticAnalyzer::lookupQualifiedFunction(
    const std::string& qualifiedName) const {
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
  // Walk scope chain
  for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
    auto* result = searchScope(*it);
    if (result) return result;
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
    if (import.isWildcard) {
      candidate = sun::QualifiedName(import.namespacePath, name);
    } else if (import.isPrefixWildcard) {
      if (name.size() >= import.prefix.size() &&
          name.substr(0, import.prefix.size()) == import.prefix) {
        candidate = sun::QualifiedName(import.namespacePath, name);
      }
    } else if (import.target == name) {
      candidate = sun::QualifiedName(import.namespacePath, name);
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
        // Check if name exists in the source scope
        if (binding.sourceScope->hasSymbol(name)) {
          // Reconstruct the QualifiedName from the module path
          sun::QualifiedName candidate(binding.sourceScope->moduleName.empty()
                                           ? ""
                                           : binding.sourceScope->moduleName,
                                       name);
          // Build full module path by walking parent chain
          std::string modPath;
          const SemanticScope* s = binding.sourceScope;
          while (s && s->type == ScopeType::Module) {
            if (modPath.empty())
              modPath = s->moduleName;
            else
              modPath = s->moduleName + "." + modPath;
            s = s->parent;
          }
          if (!modPath.empty()) {
            candidate = sun::QualifiedName(modPath, name);
          }
          if (std::find(matches.begin(), matches.end(), candidate) ==
              matches.end()) {
            matches.push_back(candidate);
          }
        }
      } else if (binding.localName == name) {
        if (binding.sourceScope->hasSymbol(binding.sourceName)) {
          // Reconstruct QualifiedName
          std::string modPath;
          const SemanticScope* s = binding.sourceScope;
          while (s && s->type == ScopeType::Module) {
            if (modPath.empty())
              modPath = s->moduleName;
            else
              modPath = s->moduleName + "." + modPath;
            s = s->parent;
          }
          sun::QualifiedName candidate(modPath, binding.sourceName);
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
