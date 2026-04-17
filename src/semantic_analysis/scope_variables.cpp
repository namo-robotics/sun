// semantic_analysis/scope_variables.cpp — Scope and variable management

#include <cstdint>

#include "error.h"
#include "semantic_analyzer.h"

using sun::unwrapRef;

// -------------------------------------------------------------------
// Scope management - typed scopes
// -------------------------------------------------------------------

void SemanticAnalyzer::enterScope(ScopeType type) {
  scopeStack.emplace_back(type);
}

void SemanticAnalyzer::enterModuleScope(const std::string& moduleName) {
  scopeStack.emplace_back(ScopeType::Module, moduleName);
}

void SemanticAnalyzer::exitScope() {
  if (scopeStack.size() > 1) {
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

void SemanticAnalyzer::registerModule(const std::string& modulePath) {
  declaredModules.insert(modulePath);
}

bool SemanticAnalyzer::isModuleName(const std::string& name) const {
  return declaredModules.count(name) > 0;
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
  std::string sig = getFunctionSignature(name, info.paramTypes);
  functionTable[sig] = info;
}

std::vector<FunctionInfo> SemanticAnalyzer::getAllFunctions(
    const std::string& name) const {
  std::vector<FunctionInfo> results;
  std::string prefix = name + "(";
  for (const auto& [sig, info] : functionTable) {
    if (sig.compare(0, prefix.size(), prefix) == 0) {
      results.push_back(info);
    }
  }
  return results;
}

std::optional<FunctionInfo> SemanticAnalyzer::lookupFunction(
    const std::string& name, const std::vector<sun::TypePtr>& argTypes) const {
  // First try exact match
  std::string sig = getFunctionSignature(name, argTypes);
  auto it = functionTable.find(sig);
  if (it != functionTable.end()) {
    return it->second;
  }

  // Try to find a compatible overload
  std::string prefix = name + "(";
  for (const auto& [funcSig, info] : functionTable) {
    if (funcSig.compare(0, prefix.size(), prefix) != 0) continue;
    if (info.paramTypes.size() != argTypes.size()) continue;

    bool compatible = true;
    for (size_t i = 0; i < argTypes.size(); ++i) {
      if (!argTypes[i] || !info.paramTypes[i]) {
        compatible = false;
        break;
      }
      if (info.paramTypes[i]->equals(*argTypes[i])) continue;

      // Check implicit conversions
      // Reference parameter accepts the referenced type directly
      if (info.paramTypes[i]->isReference()) {
        auto* refType =
            static_cast<const sun::ReferenceType*>(info.paramTypes[i].get());
        if (refType->getReferencedType()->equals(*argTypes[i])) continue;

        // ref array<T> (unsized) accepts any array<T, dims...>
        if (refType->getReferencedType()->isArray() && argTypes[i]->isArray()) {
          auto* paramArray = static_cast<const sun::ArrayType*>(
              refType->getReferencedType().get());
          auto* argArray =
              static_cast<const sun::ArrayType*>(argTypes[i].get());
          if (paramArray->isUnsized() &&
              paramArray->getElementType()->equals(*argArray->getElementType()))
            continue;
        }
      }

      // Value parameter accepts a reference (implicit dereference)
      if (argTypes[i]->isReference()) {
        auto* refType =
            static_cast<const sun::ReferenceType*>(argTypes[i].get());
        if (info.paramTypes[i]->equals(*refType->getReferencedType())) continue;
      }

      // Null is compatible with any pointer type
      if (argTypes[i]->isNullPointer() && info.paramTypes[i]->isAnyPointer()) {
        continue;
      }

      // static_ptr<T> is compatible with raw_ptr<T>
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

      // Class-to-interface compatibility:
      // A class C is compatible with interface I if C implements I
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
  namespacedVariables[qualifiedName] = {type, 0, false};
}

void SemanticAnalyzer::registerNamespacedFunction(
    const std::string& qualifiedName, const FunctionInfo& info) {
  namespacedFunctions[qualifiedName] = info;
}

VariableInfo* SemanticAnalyzer::lookupQualifiedVariable(
    const std::string& qualifiedName) {
  auto it = namespacedVariables.find(qualifiedName);
  if (it != namespacedVariables.end()) {
    return &it->second;
  }
  return nullptr;
}

const FunctionInfo* SemanticAnalyzer::lookupQualifiedFunction(
    const std::string& qualifiedName) const {
  auto it = namespacedFunctions.find(qualifiedName);
  if (it != namespacedFunctions.end()) {
    return &it->second;
  }
  return nullptr;
}

std::string SemanticAnalyzer::resolveNameWithUsings(
    const std::string& name) const {
  // Helper to check if a function with given name exists in functionTable
  auto functionExists = [this](const std::string& funcName) -> bool {
    std::string prefix = funcName + "(";
    for (const auto& [sig, info] : functionTable) {
      if (sig.compare(0, prefix.size(), prefix) == 0) {
        return true;
      }
    }
    return false;
  };

  // Helper to check if a class or generic class exists
  auto classExists = [this](const std::string& className) -> bool {
    return classTable.contains(className) ||
           genericClassTable.contains(className);
  };

  // First, if we're inside a module, check for the name with module prefix
  // This allows code inside `module sun { }` to reference other symbols
  // in the same module without needing `using sun;`
  std::string modulePrefix = getCurrentModulePrefix();
  if (!modulePrefix.empty()) {
    std::string candidate = modulePrefix + name;
    if (namespacedVariables.contains(candidate) ||
        namespacedFunctions.contains(candidate) || functionExists(candidate) ||
        classExists(candidate)) {
      return candidate;
    }
  }

  // Get all active using imports from scope stack
  auto activeImports = getActiveUsingImports();

  // Then check using imports
  for (const auto& import : activeImports) {
    if (import.isWildcard) {
      // Wildcard import: try namespacePath_name (mangled)
      std::string candidate = import.namespacePath + "_" + name;
      // Check if this qualified name exists (in namespaced tables,
      // functionTable, or classTable)
      if (namespacedVariables.contains(candidate) ||
          namespacedFunctions.contains(candidate) ||
          functionExists(candidate) || classExists(candidate)) {
        return candidate;
      }
    } else if (import.isPrefixWildcard) {
      // Prefix wildcard: check if name starts with prefix
      if (name.size() >= import.prefix.size() &&
          name.substr(0, import.prefix.size()) == import.prefix) {
        std::string candidate = import.namespacePath + "_" + name;
        if (namespacedVariables.contains(candidate) ||
            namespacedFunctions.contains(candidate) ||
            functionExists(candidate) || classExists(candidate)) {
          return candidate;
        }
      }
    } else if (import.target == name) {
      // Specific import: return the fully qualified (mangled) name
      return import.namespacePath + "_" + name;
    }
  }

  // Return original name if no using import applies
  return name;
}

void SemanticAnalyzer::addUsingImport(const UsingImport& import) {
  // Add using import to the current scope
  if (!scopeStack.empty()) {
    scopeStack.back().usingImports.push_back(import);
  }
}
