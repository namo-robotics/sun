// semantic_context.cpp — The shared state and scope machinery of a semantic
// analysis run (see semantic_context.h)

#include "semantic_analysis/semantic_context.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_registry.h"
#include "semantic_analysis/visibility.h"
#include "support/error.h"

using sun::semantic_analysis::DeclarationId;
using sun::semantic_analysis::DeclarationKind;
using sun::semantic_analysis::QualifiedName;
using sun::types::ClassType;
using sun::types::InterfaceType;
using sun::types::TypePtr;

using sun::support::logAndThrowError;
using sun::support::Position;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::types::Type;

using sun::semantic_analysis::fieldRef;
using sun::semantic_analysis::methodRef;
using sun::semantic_analysis::moduleRef;
using sun::types::unwrapRef;

// isLibraryScope() is provided by semantic_scope.h

SemanticContext::SemanticContext(
    std::shared_ptr<sun::semantic_analysis::AnalysisResults> results)
    : results_(std::move(results)) {
  rootScope_->accessContext = this;  // lookups filter by visibility
  rootScope_->interfaces["IError"] = results_->types->errorInterface;
  rootScope_->classes["ArithmeticError"] = results_->types->arithmeticError;
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
    const std::vector<std::string>& params, const std::vector<TypePtr>& args) {
  enterScope(ScopeType::TypeParams);
  currentScope().declareTypeParameters(params, args);
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
  enterScope(currentScope().declareModule(moduleName));
  if (isLibraryScope(moduleName))
    static_cast<ModuleScope*>(currentScope_)->declarationId =
        results_->declarations.module(moduleName);
}

void SemanticContext::enterClassScope(const QualifiedName& className) {
  auto classScope = std::make_shared<ClassScope>();
  classScope->classBaseName = className.baseName;
  // Use the class's scope path directly
  classScope->scopePath = className.scopePath;
  classScope->scopePath.push_back(className.baseName);
  classScope->parent = currentScope_;
  currentScope_->children.push_back(classScope);
  currentScope_ = classScope.get();
}

void SemanticContext::enterInterfaceScope(const QualifiedName& interfaceName) {
  auto ifaceScope = std::make_shared<InterfaceScope>();
  ifaceScope->interfaceBaseName = interfaceName.baseName;
  // Use the interface's scope path directly
  ifaceScope->scopePath = interfaceName.scopePath;
  ifaceScope->scopePath.push_back(interfaceName.baseName);
  ifaceScope->parent = currentScope_;
  currentScope_->children.push_back(ifaceScope);
  currentScope_ = ifaceScope.get();
}

void SemanticContext::enterFunctionScope(const std::string& funcSig,
                                         const QualifiedName& funcName,
                                         bool canThrow, TypePtr returnType) {
  auto funcScope = std::make_shared<FunctionScope>();
  funcScope->functionSignature = funcSig;
  funcScope->functionName = funcName;
  funcScope->functionCanThrow = canThrow;
  funcScope->functionReturnType = std::move(returnType);
  funcScope->parent = currentScope_;

  // Nested declarations inherit the source spelling; IDs distinguish overloads.
  funcScope->scopePath = funcName.scopePath;
  if (!funcName.baseName.empty())
    funcScope->scopePath.push_back(funcName.baseName);

  currentScope_->children.push_back(funcScope);
  currentScope_ = funcScope.get();
}

TypePtr SemanticContext::currentFunctionReturnType() const {
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

std::vector<std::string> SemanticContext::getCurrentScopePath() const {
  // Walk up to find the nearest scope with a scopePath (Module or Import).
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    if (!s->scopePath.empty()) {
      return s->scopePath;
    }
  }
  return {};
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

/**
 * Helper: collect ALL module scopes matching a path across import scopes
 * and using statements. This handles the case where two .sun imports define
 * the same module name, or where a module is brought in via `using`.
 * Also collects parent module scopes (e.g., for path "A.B", also collects "A").
 */
static std::vector<SemanticScope*> collectAllModuleScopes(
    const SemanticScope* startScope, const std::string& dotPath) {
  std::vector<SemanticScope*> results;
  if (dotPath.empty() || !startScope) return results;
  bool pinned = dotPath.starts_with("$");
  if (pinned) {
    if (auto* scope = startScope->lookupModuleScope(dotPath))
      results.push_back(scope);
    return results;
  }

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
        if (!s->admitsImport(binding.sourceFileId)) continue;
        if (!binding.sourceScope || !binding.isWildcard) continue;
        // scopeName is the simple name like "std", not the full path with
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
    SymbolKind filterKind, const std::vector<TypePtr>* argTypes) const {
  // Get visible module path for searching across all matching scopes
  std::string visiblePath =
      sun::semantic_analysis::displayModulePath(modulePath);

  // Access filtering: private symbols of other modules are skipped; if that
  // leaves nothing, the denial is reported instead of "unknown member".
  AccessFilter accessFilter(currentScope_);

  // Helper to search a single scope for all symbol types
  auto searchInScope = [&](SemanticScope* scope) -> std::optional<SymbolMatch> {
    std::string fullPath = QualifiedName::joinPath(scope->scopePath);
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
            std::vector<FunctionArgumentType> lookupTypes;
            lookupTypes.reserve(argTypes->size());
            for (const auto& type : *argTypes)
              lookupTypes.push_back({type, {}});
            auto resolved =
                scope->lookupFunctionLocal(name, lookupTypes, &accessFilter);
            if (!resolved) return std::nullopt;
            info = nullptr;
            for (const auto* candidate : *overloads) {
              if (candidate->declarationId == resolved->declarationId) {
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

    // Check generic functions by source name.
    if (matchesFilter(SymbolKind::GenericFunction)) {
      auto genFuncIt = scope->genericFunctions.find(name);
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
      auto varIt = scope->namespacedVariables.find(name);
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

  // An exact module identity must never search other versions by visible name.
  if (modulePath.starts_with("$")) {
    if (auto* scope = lookupModuleScope(modulePath)) {
      if (auto match = searchInScope(scope)) return *match;
    }
    accessFilter.finish();
    return {};
  }
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
      paths += sun::semantic_analysis::displayModulePath(m.modulePath);
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

// -------------------------------------------------------------------
// Type narrowing (from _is<T> type guards)
// -------------------------------------------------------------------

void SemanticContext::narrowVariable(const std::string& varName,
                                     TypePtr narrowedType) {
  currentScope_->narrowedTypes[varName] = std::move(narrowedType);
}

TypePtr SemanticContext::getNarrowedType(const std::string& varName,
                                         TypePtr originalType) const {
  // Search from innermost to outermost scope
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    auto found = s->narrowedTypes.find(varName);
    if (found != s->narrowedTypes.end()) {
      TypePtr narrowedType = found->second;

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
        auto* classType = static_cast<ClassType*>(narrowedType.get());
        auto* ifaceType = static_cast<InterfaceType*>(originalType.get());
        if (classType->implementsInterface(*ifaceType)) return narrowedType;
      }

      // Class -> Interface: Class is more specific, return the class
      if (originalType->isClass() && narrowedType->isInterface()) {
        auto* classType = static_cast<ClassType*>(originalType.get());
        auto* ifaceType = static_cast<InterfaceType*>(narrowedType.get());
        if (classType->implementsInterface(*ifaceType)) return originalType;
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

const GenericFunctionInfo* SemanticContext::lookupGenericFunction(
    const std::string& name) const {
  return currentScope_->lookupGenericFunction(name);
}

std::vector<FunctionInfo> SemanticContext::getAllFunctions(
    const std::string& name) const {
  return currentScope_->getAllFunctions(name);
}

std::optional<FunctionInfo> SemanticContext::lookupFunction(
    const std::string& name, const std::vector<FunctionArgumentType>& argTypes,
    std::optional<Position> loc) const {
  return currentScope_->lookupFunction(name, argTypes, loc);
}

// -------------------------------------------------------------------
// Builtin function registration
// -------------------------------------------------------------------

void SemanticContext::registerBuiltinFunctions() {
  using sun::types::Types;

  // Low-level print intrinsics (used by stdlib print functions)
  currentScope().declareFunction("_print_i32",
                                 {Types::Void(), {Types::Int32()}, {}});
  currentScope().declareFunction("_print_i64",
                                 {Types::Void(), {Types::Int64()}, {}});
  currentScope().declareFunction("_print_u64",
                                 {Types::Void(), {Types::UInt64()}, {}});
  currentScope().declareFunction("_print_f64",
                                 {Types::Void(), {Types::Float64()}, {}});
  currentScope().declareFunction("_print_newline", {Types::Void(), {}, {}});
  // _print_char intrinsic: write one char as UTF-8
  currentScope().declareFunction("_print_char",
                                 {Types::Void(), {Types::Char()}, {}});
  // _print_bytes intrinsic: write raw bytes to stdout
  currentScope().declareFunction(
      "_print_bytes",
      {Types::Void(), {Types::RawPointer(Types::Int8()), Types::Int64()}, {}});
  // _println_str: print string literal with newline
  currentScope().declareFunction("_println_str",
                                 {Types::Void(), {Types::String()}, {}});
  currentScope().declareFunction(
      "_println_str", {Types::Void(), {Types::RawPointer(Types::UInt8())}, {}});

  // File I/O intrinsics
  currentScope().declareFunction(
      "__file_open", {Types::Int32(), {Types::String(), Types::Int32()}, {}});
  currentScope().declareFunction("__file_close",
                                 {Types::Int32(), {Types::Int32()}, {}});
  currentScope().declareFunction(
      "__file_write", {Types::Int32(), {Types::Int32(), Types::String()}, {}});
  currentScope().declareFunction(
      "__file_read",
      {Types::RawPointer(Types::Int8()), {Types::Int32(), Types::Int32()}, {}});

  // Extended file I/O intrinsics
  currentScope().declareFunction(
      "__lseek",
      {Types::Int64(), {Types::Int32(), Types::Int64(), Types::Int32()}, {}});
  currentScope().declareFunction(
      "__fstat",
      {Types::Int32(), {Types::Int32(), Types::RawPointer(Types::Int8())}, {}});
  currentScope().declareFunction("__fsync",
                                 {Types::Int32(), {Types::Int32()}, {}});
  currentScope().declareFunction(
      "__ftruncate", {Types::Int32(), {Types::Int32(), Types::Int64()}, {}});
  currentScope().declareFunction("__unlink",
                                 {Types::Int32(), {Types::String()}, {}});
  currentScope().declareFunction(
      "__rename", {Types::Int32(), {Types::String(), Types::String()}, {}});
  currentScope().declareFunction(
      "__mkdir", {Types::Int32(), {Types::String(), Types::Int32()}, {}});
  currentScope().declareFunction("__rmdir",
                                 {Types::Int32(), {Types::String()}, {}});
  currentScope().declareFunction(
      "__write",
      {Types::Int64(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int64()},
       {}});
  currentScope().declareFunction(
      "__read",
      {Types::Int64(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int64()},
       {}});

  // Low-level memory access intrinsics
  // _load_i64(ptr, index) - load i64 from ptr at byte offset index*8
  currentScope().declareFunction(
      "_load_i64",
      {Types::Int64(), {Types::RawPointer(Types::Int8()), Types::Int64()}, {}});
  // _store_i64(ptr, index, value) - store i64 to ptr at byte offset index*8
  currentScope().declareFunction(
      "_store_i64",
      {Types::Void(),
       {Types::RawPointer(Types::Int8()), Types::Int64(), Types::Int64()},
       {}});

  // Memory allocation intrinsics
  // _malloc(size) - allocate size bytes, returns raw_ptr<i8>
  currentScope().declareFunction(
      "_malloc", {Types::RawPointer(Types::Int8()), {Types::Int64()}, {}});
  // _free(ptr) - free previously allocated memory
  currentScope().declareFunction(
      "_free", {Types::Void(), {Types::RawPointer(Types::Int8())}, {}});
  // _memcpy(dst, src, len) - copy len bytes from src to dst
  currentScope().declareFunction(
      "_memcpy", {Types::Void(),
                  {Types::RawPointer(Types::UInt8()),
                   Types::RawPointer(Types::UInt8()), Types::Int64()},
                  {}});
  // _memmove(dst, src, len) - copy len bytes, allowing overlapping ranges
  currentScope().declareFunction(
      "_memmove", {Types::Void(),
                   {Types::RawPointer(Types::UInt8()),
                    Types::RawPointer(Types::UInt8()), Types::Int64()},
                   {}});
  // _memset(dst, value, len) - set len bytes at dst to value
  currentScope().declareFunction("_memset", {Types::Void(),
                                             {Types::RawPointer(Types::UInt8()),
                                              Types::Int32(), Types::Int64()},
                                             {}});
  // _ptr_offset(ptr, byte_offset) - offset a pointer by byte_offset bytes
  currentScope().declareFunction(
      "_ptr_offset", {Types::RawPointer(Types::UInt8()),
                      {Types::RawPointer(Types::UInt8()), Types::Int64()},
                      {}});

  // Atomic intrinsics use acquire/release ordering and operate on matching
  // pointer and value types.
  auto registerAtomicInteger = [this](const std::string& suffix,
                                      const TypePtr& type) {
    const std::string prefix = "_atomic_";
    currentScope().declareFunction(
        prefix + "cmpxchg_" + suffix,
        {type, {Types::RawPointer(type), type, type}, {}});
    currentScope().declareFunction(
        prefix + "store_" + suffix,
        {Types::Void(), {Types::RawPointer(type), type}, {}});
    currentScope().declareFunction(prefix + "load_" + suffix,
                                   {type, {Types::RawPointer(type)}, {}});
    currentScope().declareFunction(prefix + "fetch_add_" + suffix,
                                   {type, {Types::RawPointer(type), type}, {}});
    currentScope().declareFunction(prefix + "fetch_sub_" + suffix,
                                   {type, {Types::RawPointer(type), type}, {}});
  };
  registerAtomicInteger("i32", Types::Int32());
  registerAtomicInteger("i64", Types::Int64());
  registerAtomicInteger("u64", Types::UInt64());
  currentScope().declareFunction("_atomic_fence_acquire",
                                 {Types::Void(), {}, {}});
  currentScope().declareFunction("_atomic_fence_release",
                                 {Types::Void(), {}, {}});

  // Bit intrinsics
  // _mul_hi_u64(a, b) - high 64 bits of the 128-bit product a * b
  currentScope().declareFunction(
      "_mul_hi_u64", {Types::UInt64(), {Types::UInt64(), Types::UInt64()}, {}});
  // _ctlz_u64(x) / _cttz_u64(x) - leading / trailing zero bit count (64 for 0)
  currentScope().declareFunction("_ctlz_u64",
                                 {Types::UInt64(), {Types::UInt64()}, {}});
  currentScope().declareFunction("_cttz_u64",
                                 {Types::UInt64(), {Types::UInt64()}, {}});
  // _bswap_u16/u32/u64(x) - the same value with its bytes in the opposite
  // order, the primitive under the byte-order helpers in std
  currentScope().declareFunction("_bswap_u16",
                                 {Types::UInt16(), {Types::UInt16()}, {}});
  currentScope().declareFunction("_bswap_u32",
                                 {Types::UInt32(), {Types::UInt32()}, {}});
  currentScope().declareFunction("_bswap_u64",
                                 {Types::UInt64(), {Types::UInt64()}, {}});

  // Target intrinsics
  // _target_is("macos") - compile-time check of the compilation target's
  // operating system; codegen folds it to a constant and keeps only the live
  // side of a branch on it
  currentScope().declareFunction("_target_is",
                                 {Types::Bool(), {Types::String()}, {}});

  // Futex intrinsics (Linux-specific thread synchronization)
  // _futex_wait(ptr, expected) - block if *ptr == expected
  currentScope().declareFunction(
      "_futex_wait",
      {Types::Void(), {Types::RawPointer(Types::Int32()), Types::Int32()}, {}});
  // _futex_wake(ptr) - wake one waiter
  currentScope().declareFunction(
      "_futex_wake", {Types::Void(), {Types::RawPointer(Types::Int32())}, {}});

  // Network socket intrinsics (libc sockets)
  // __socket(domain, type, protocol) -> fd
  currentScope().declareFunction(
      "__socket",
      {Types::Int32(), {Types::Int32(), Types::Int32(), Types::Int32()}, {}});
  // __bind(fd, addr, addrlen) -> result
  currentScope().declareFunction(
      "__bind",
      {Types::Int32(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int32()},
       {}});
  // __listen(fd, backlog) -> result
  currentScope().declareFunction(
      "__listen", {Types::Int32(), {Types::Int32(), Types::Int32()}, {}});
  // __accept(fd, addr, addrlen_ptr) -> new_fd
  currentScope().declareFunction(
      "__accept", {Types::Int32(),
                   {Types::Int32(), Types::RawPointer(Types::UInt8()),
                    Types::RawPointer(Types::Int32())},
                   {}});
  // __connect(fd, addr, addrlen) -> result
  currentScope().declareFunction(
      "__connect",
      {Types::Int32(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int32()},
       {}});
  // __send(fd, buf, len, flags) -> bytes_sent
  currentScope().declareFunction(
      "__send", {Types::Int64(),
                 {Types::Int32(), Types::RawPointer(Types::UInt8()),
                  Types::Int64(), Types::Int32()},
                 {}});
  // __recv(fd, buf, len, flags) -> bytes_received
  currentScope().declareFunction(
      "__recv", {Types::Int64(),
                 {Types::Int32(), Types::RawPointer(Types::UInt8()),
                  Types::Int64(), Types::Int32()},
                 {}});
  // __shutdown(fd, how) -> result
  currentScope().declareFunction(
      "__shutdown", {Types::Int32(), {Types::Int32(), Types::Int32()}, {}});
  // __setsockopt(fd, level, optname, optval, optlen) -> result
  currentScope().declareFunction(
      "__setsockopt", {Types::Int32(),
                       {Types::Int32(), Types::Int32(), Types::Int32(),
                        Types::RawPointer(Types::UInt8()), Types::Int32()},
                       {}});
  // __getsockopt(fd, level, optname, optval, optlen_ptr) -> result
  currentScope().declareFunction(
      "__getsockopt",
      {Types::Int32(),
       {Types::Int32(), Types::Int32(), Types::Int32(),
        Types::RawPointer(Types::UInt8()), Types::RawPointer(Types::Int32())},
       {}});

  // High-level IPv4 socket intrinsics (build sockaddr_in internally)
  // __bind_ipv4(fd, ip, port) -> result
  currentScope().declareFunction(
      "__bind_ipv4",
      {Types::Int32(), {Types::Int32(), Types::Int32(), Types::Int32()}, {}});
  // __connect_ipv4(fd, ip, port) -> result
  currentScope().declareFunction(
      "__connect_ipv4",
      {Types::Int32(), {Types::Int32(), Types::Int32(), Types::Int32()}, {}});
  // __accept_fd(fd) -> new_fd
  currentScope().declareFunction("__accept_fd",
                                 {Types::Int32(), {Types::Int32()}, {}});
  // __sendto_ipv4(fd, buf, len, flags, ip, port) -> bytes_sent
  currentScope().declareFunction(
      "__sendto_ipv4",
      {Types::Int64(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int64(),
        Types::Int32(), Types::Int32(), Types::Int32()},
       {}});
  // __recvfrom_ipv4(fd, buf, len, flags, out_ip, out_port) -> bytes_received
  currentScope().declareFunction(
      "__recvfrom_ipv4",
      {Types::Int64(),
       {Types::Int32(), Types::RawPointer(Types::UInt8()), Types::Int64(),
        Types::Int32(), Types::RawPointer(Types::Int32()),
        Types::RawPointer(Types::Int32())},
       {}});
  // __getsockname_ipv4(fd, out_ip, out_port) -> result
  currentScope().declareFunction(
      "__getsockname_ipv4", {Types::Int32(),
                             {Types::Int32(), Types::RawPointer(Types::Int32()),
                              Types::RawPointer(Types::Int32())},
                             {}});
}

// -------------------------------------------------------------------
// Namespace-qualified symbols (separate from scope-based lookup)
// -------------------------------------------------------------------

VariableInfo* SemanticContext::lookupQualifiedVariable(
    const std::string& qualifiedName) {
  return currentScope_->lookupQualifiedVariable(qualifiedName);
}

// Helper: Get the full module path including library scope hashes
// e.g., "b" -> "$hash$.b" if b is inside a library scope
std::string SemanticContext::getFullModulePath(
    const std::string& visiblePath) const {
  if (auto* scope = lookupModuleScope(visiblePath))
    return QualifiedName(scope->scopePath, "").scopePathString();
  return visiblePath;
}

const FunctionInfo* SemanticContext::lookupQualifiedFunction(
    const std::string& qualifiedName) const {
  return currentScope_->lookupQualifiedFunction(qualifiedName);
}

QualifiedName SemanticContext::resolveNameWithUsings(
    const std::string& name) const {
  return currentScope_->resolveNameWithUsings(name);
}

void SemanticContext::addUsingImport(const UsingImport& import) {
  // Skip if already present (idempotent for Pass 1 + Pass 2 double-processing)
  for (const auto& existing : currentScope_->usingImports) {
    if (existing.sourceFileId == currentSourceFileId() &&
        existing.namespacePath == import.namespacePath &&
        existing.target == import.target) {
      return;
    }
  }
  auto scopedImport = import;
  scopedImport.sourceFileId = currentSourceFileId();
  currentScope_->usingImports.push_back(std::move(scopedImport));
}

void SemanticContext::addImportBinding(const ImportBinding& binding) {
  // Skip if already present (idempotent for Pass 1 + Pass 2 double-processing)
  for (const auto& existing : currentScope_->importBindings) {
    if (existing.sourceFileId == currentSourceFileId() &&
        existing.sourceScope == binding.sourceScope &&
        existing.isWildcard == binding.isWildcard &&
        existing.localName == binding.localName &&
        existing.sourceName == binding.sourceName) {
      return;
    }
  }
  auto scopedBinding = binding;
  scopedBinding.sourceFileId = currentSourceFileId();
  currentScope_->importBindings.push_back(std::move(scopedBinding));
}

std::shared_ptr<ClassType> SemanticContext::lookupClass(
    const std::string& name) const {
  return currentScope_->lookupClass(name);
}

void SemanticContext::setCurrentClass(std::shared_ptr<ClassType> classType) {
  currentClass_ = std::move(classType);
}

std::shared_ptr<ClassType> SemanticContext::getCurrentClass() const {
  return currentClass_;
}

const GenericClassInfo* SemanticContext::lookupGenericClass(
    const std::string& name) const {
  return currentScope_->lookupGenericClass(name);
}

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/** Retrieve selected templates, including declarations in closed local scopes.
 */
template <typename Info>
const Info* findTemplate(
    const SemanticScopeBase& scope, DeclarationId id,
    std::map<std::string, Info> SemanticScopeBase::* members) {
  for (const auto& [name, info] : scope.*members)
    if (info.AST && info.AST->getDeclarationId() == id) return &info;
  for (const auto& [name, child] : scope.childModules)
    if (auto* info = findTemplate(*child, id, members)) return info;
  for (const auto& child : scope.children)
    if (auto* info = findTemplate(*child, id, members)) return info;
  return nullptr;
}

}  // namespace

const GenericClassInfo* SemanticContext::lookupGenericClass(
    DeclarationId id) const {
  results_->declarations.get(id);
  return findTemplate(*rootScope_, id, &SemanticScopeBase::genericClasses);
}

const GenericInterfaceInfo* SemanticContext::lookupGenericInterface(
    DeclarationId id) const {
  results_->declarations.get(id);
  return findTemplate(*rootScope_, id, &SemanticScopeBase::genericInterfaces);
}

const GenericEnumInfo* SemanticContext::lookupGenericEnum(
    DeclarationId id) const {
  results_->declarations.get(id);
  return findTemplate(*rootScope_, id, &SemanticScopeBase::genericEnums);
}

TypePtr SemanticContext::findTypeParameter(const std::string& name) const {
  // Search from innermost to outermost scope
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    auto found = s->typeParameters.find(name);
    if (found != s->typeParameters.end()) {
      return found->second;
    }
  }
  return nullptr;
}

TypePtr SemanticContext::findTypeAlias(const std::string& name) const {
  // Search from innermost to outermost scope
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    auto found = s->typeAliases.find(name);
    if (found != s->typeAliases.end()) {
      return found->second;
    }
  }
  return nullptr;
}

std::shared_ptr<InterfaceType> SemanticContext::lookupInterface(
    const std::string& name) const {
  return currentScope_->lookupInterface(name);
}

const GenericInterfaceInfo* SemanticContext::lookupGenericInterface(
    const std::string& name) const {
  return currentScope_->lookupGenericInterface(name);
}

std::shared_ptr<sun::types::EnumType> SemanticContext::lookupEnum(
    const std::string& name) const {
  return currentScope_->lookupEnum(name);
}

const GenericEnumInfo* SemanticContext::lookupGenericEnum(
    const std::string& name) const {
  return currentScope_->lookupGenericEnum(name);
}

DeclarationId SemanticContext::currentModuleId() const {
  for (auto* s = currentScope_; s != nullptr; s = s->parent) {
    if (s->getType() == ScopeType::Module)
      return static_cast<const ModuleScope*>(s)->declarationId;
  }
  return {};
}

void SemanticContext::denyAccess(
    const sun::semantic_analysis::ItemRef& item) const {
  auto loc = currentLocation();
  sun::support::logSemanticError(
      sun::semantic_analysis::denialMessage(item, declarationTable()), loc);
}

const sun::types::ClassField* SemanticContext::accessibleField(
    const ClassType& cls, const std::string& name, const Position& loc) const {
  const auto* f = cls.getField(name);
  if (f) requireAccessible(fieldRef(cls, *f), loc);
  return f;
}

const sun::types::ClassMethod* SemanticContext::accessibleMethod(
    const ClassType& cls, const std::string& name, const Position& loc) const {
  const auto* m = cls.getMethod(name);
  if (m) requireAccessible(methodRef(cls, *m), loc);
  return m;
}

const sun::types::ClassMethod* SemanticContext::accessibleMethodForArgs(
    const ClassType& cls, const std::string& name,
    const std::vector<TypePtr>& argTypes, const Position& loc) const {
  const auto* m = cls.getMethodForArgs(name, argTypes);
  if (m) requireAccessible(methodRef(cls, *m), loc);
  return m;
}

const sun::types::InterfaceField* SemanticContext::accessibleField(
    const InterfaceType& iface, const std::string& name,
    const Position& loc) const {
  const auto* f = iface.getField(name);
  if (f) requireAccessible(fieldRef(iface, *f), loc);
  return f;
}

const sun::types::InterfaceMethod* SemanticContext::accessibleMethod(
    const InterfaceType& iface, const std::string& name,
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

SemanticScopeBase* SemanticContext::lookupModuleScope(DeclarationId id) const {
  if (!id)
    logAndThrowError(
        "Imported module identity is missing; import the required exact "
        "bundle");
  if (results_->declarations.get(id).kind != DeclarationKind::Module)
    logAndThrowError(
        "Imported module reference has the wrong declaration kind");
  auto find = [&](auto&& self, SemanticScopeBase* scope) -> SemanticScopeBase* {
    if (auto* module = dynamic_cast<ModuleScope*>(scope);
        module && module->declarationId == id)
      return module;
    for (auto& [name, child] : scope->childModules)
      if (auto* found = self(self, child.get())) return found;
    for (auto& child : scope->children)
      if (auto* found = self(self, child.get())) return found;
    return nullptr;
  };
  auto* result = find(find, rootScope_.get());
  if (!result)
    logAndThrowError("Imported module declaration has no registered scope");
  return result;
}

DeclarationId SemanticContext::requireDeclaration(
    const sun::semantic_analysis::PortableDeclarationKey& key,
    const std::string& exporter,
    std::optional<sun::types::Type::Kind> expectedKind,
    const std::string& displayName) const {
  auto id = results_->declarations.findPortable(key);
  bool wrongKind = false;
  if (id) {
    auto kind = results_->declarations.get(id).kind;
    auto actual =
        kind == DeclarationKind::Class       ? sun::types::Type::Kind::Class
        : kind == DeclarationKind::Interface ? sun::types::Type::Kind::Interface
        : kind == DeclarationKind::Enum      ? sun::types::Type::Kind::Enum
                                             : sun::types::Type::Kind::Void;
    wrongKind = actual == sun::types::Type::Kind::Void ||
                (expectedKind && actual != *expectedKind);
    if (!wrongKind) return id;
  }
  std::string message = "moon exact dependency: ";
  if (!exporter.empty()) message += "library '" + exporter + "' ";
  message += "requires declaration '" + displayName + "' with identity " +
             key.encoding();
  if (wrongKind) message += " (declaration has the wrong type kind)";
  if (!id) {
    const auto& table = results_->declarations;
    for (uint64_t i = 1; i <= table.size(); ++i) {
      const auto& candidate = table.get(DeclarationId(i));
      if (candidate.portableKey &&
          (candidate.name == displayName ||
           displayName.ends_with("." + candidate.name)) &&
          !(*candidate.portableKey == key)) {
        message += " (conflicting bundle supplied)";
        break;
      }
    }
  }
  logAndThrowError(message + ". Explicitly import the required exact bundle.",
                   currentLocation());
}

}  // namespace sun::semantic_analysis
