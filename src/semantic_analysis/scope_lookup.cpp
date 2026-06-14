// scope_lookup.cpp — Scope-chain lookup methods on SemanticScope
//
// These methods traverse the parent chain, import scopes, __definition__
// scopes, and import bindings to find symbols. They encapsulate the lookup
// logic that was previously spread across SemanticAnalyzer.

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "error.h"
#include "semantic_scope.h"

// -------------------------------------------------------------------
// lookupClass — find a class in the scope chain
// -------------------------------------------------------------------
std::shared_ptr<sun::ClassType> SemanticScopeBase::lookupClass(
    const std::string& name) const {
  return lookupInChain<std::shared_ptr<sun::ClassType>>(
      [&](const SemanticScopeBase* scope) { return scope->findClass(name); });
}

// -------------------------------------------------------------------
// lookupGenericClass — find a generic class in the scope chain
// Uses lookupGenericSymbol-style traversal for module-qualified names
// -------------------------------------------------------------------
const GenericClassInfo* SemanticScopeBase::lookupGenericClass(
    const std::string& name) const {
  // Handle module-qualified names like "Test.Inner"
  size_t dotPos = name.find('.');
  if (dotPos != std::string::npos) {
    std::string moduleName = name.substr(0, dotPos);
    std::string symbolName = name.substr(dotPos + 1);

    for (auto* s = this; s != nullptr; s = s->parent) {
      auto modIt = s->childModules.find(moduleName);
      if (modIt != s->childModules.end() && modIt->second) {
        auto* result = modIt->second->findGenericClass(symbolName);
        if (result) return result;
      }
      for (const auto& [childName, child] : s->childModules) {
        if (!child) continue;
        if (child->getType() == ScopeType::Import ||
            childName == "__definition__") {
          auto innerModIt = child->childModules.find(moduleName);
          if (innerModIt != child->childModules.end() && innerModIt->second) {
            auto* result = innerModIt->second->findGenericClass(symbolName);
            if (result) return result;
          }
        }
      }
    }
    // Fall through to regular lookup
  }

  return lookupInChain<const GenericClassInfo*>(
      [&](const SemanticScopeBase* scope) {
        return scope->findGenericClass(name);
      });
}

// -------------------------------------------------------------------
// lookupInterface — find an interface in the scope chain
// -------------------------------------------------------------------
std::shared_ptr<sun::InterfaceType> SemanticScopeBase::lookupInterface(
    const std::string& name) const {
  return lookupInChain<std::shared_ptr<sun::InterfaceType>>(
      [&](const SemanticScopeBase* scope) {
        return scope->findInterface(name);
      });
}

// -------------------------------------------------------------------
// lookupGenericInterface — find a generic interface in the scope chain
// -------------------------------------------------------------------
const GenericInterfaceInfo* SemanticScopeBase::lookupGenericInterface(
    const std::string& name) const {
  // Handle module-qualified names like "Test.Inner"
  size_t dotPos = name.find('.');
  if (dotPos != std::string::npos) {
    std::string moduleName = name.substr(0, dotPos);
    std::string symbolName = name.substr(dotPos + 1);

    for (auto* s = this; s != nullptr; s = s->parent) {
      auto modIt = s->childModules.find(moduleName);
      if (modIt != s->childModules.end() && modIt->second) {
        auto* result = modIt->second->findGenericInterface(symbolName);
        if (result) return result;
      }
      for (const auto& [childName, child] : s->childModules) {
        if (!child) continue;
        if (child->getType() == ScopeType::Import ||
            childName == "__definition__") {
          auto innerModIt = child->childModules.find(moduleName);
          if (innerModIt != child->childModules.end() && innerModIt->second) {
            auto* result = innerModIt->second->findGenericInterface(symbolName);
            if (result) return result;
          }
        }
      }
    }
  }

  return lookupInChain<const GenericInterfaceInfo*>(
      [&](const SemanticScopeBase* scope) {
        return scope->findGenericInterface(name);
      });
}

// -------------------------------------------------------------------
// lookupEnum — find an enum in the scope chain
// -------------------------------------------------------------------
std::shared_ptr<sun::EnumType> SemanticScopeBase::lookupEnum(
    const std::string& name) const {
  return lookupInChain<std::shared_ptr<sun::EnumType>>(
      [&](const SemanticScopeBase* scope) { return scope->findEnum(name); });
}

// -------------------------------------------------------------------
// lookupVariable — find a variable in the scope chain
// -------------------------------------------------------------------
VariableInfo* SemanticScopeBase::lookupVariable(const std::string& name) {
  for (auto* s = this; s != nullptr; s = s->parent) {
    auto found = s->variables.find(name);
    if (found != s->variables.end()) {
      return &found->second;
    }
    // Search direct import-scope children
    for (const auto& [childName, child] : s->childModules) {
      if (child && child->getType() == ScopeType::Import) {
        auto childFound = child->variables.find(name);
        if (childFound != child->variables.end()) {
          return &childFound->second;
        }
      }
    }
    // Search import bindings from using statements
    for (const auto& binding : s->importBindings) {
      if (!binding.sourceScope) continue;
      if (binding.isWildcard) {
        auto bindingFound = binding.sourceScope->variables.find(name);
        if (bindingFound != binding.sourceScope->variables.end()) {
          return &bindingFound->second;
        }
      }
    }
  }
  return nullptr;
}

// -------------------------------------------------------------------
// lookupGenericFunction — find a generic function in the scope chain
// -------------------------------------------------------------------
const GenericFunctionInfo* SemanticScopeBase::lookupGenericFunction(
    const std::string& name) const {
  auto scopePath = getCurrentScopePath();
  sun::QualifiedName qname(scopePath, name);

  for (auto* s = this; s != nullptr; s = s->parent) {
    auto found = s->genericFunctions.find(qname);
    if (found != s->genericFunctions.end()) {
      return &found->second;
    }
    // Try global scope (empty scope path)
    if (!scopePath.empty()) {
      sun::QualifiedName globalQname({}, name);
      found = s->genericFunctions.find(globalQname);
      if (found != s->genericFunctions.end()) {
        return &found->second;
      }
    }
    // Search direct import-scope children
    for (const auto& [modName, child] : s->childModules) {
      if (!child || child->getType() != ScopeType::Import) continue;
      sun::QualifiedName childQname(child->scopePath, name);
      auto childFound = child->genericFunctions.find(childQname);
      if (childFound != child->genericFunctions.end()) {
        return &childFound->second;
      }
      for (const auto& [subName, subChild] : child->childModules) {
        if (!subChild || subChild->getType() == ScopeType::Import) continue;
        sun::QualifiedName subQname(subChild->scopePath, name);
        auto subFound = subChild->genericFunctions.find(subQname);
        if (subFound != subChild->genericFunctions.end()) {
          return &subFound->second;
        }
      }
    }
    // Search import bindings
    for (const auto& binding : s->importBindings) {
      if (!binding.sourceScope || !binding.isWildcard) continue;
      sun::QualifiedName bindingQname(binding.sourceScope->scopePath, name);
      auto bindingFound =
          binding.sourceScope->genericFunctions.find(bindingQname);
      if (bindingFound != binding.sourceScope->genericFunctions.end()) {
        return &bindingFound->second;
      }
    }
  }
  return nullptr;
}

// -------------------------------------------------------------------
// getAllFunctions — collect all overloads of a function name
// -------------------------------------------------------------------
std::vector<FunctionInfo> SemanticScopeBase::getAllFunctions(
    const std::string& name) const {
  std::vector<FunctionInfo> results;
  std::string prefix = name + "(";

  // Track seen signatures to avoid duplicates
  std::set<std::string> seenSignatures;
  auto addIfUnique = [&](const FunctionInfo& info) {
    // Build signature for dedup
    std::string sig = name + "(";
    for (size_t i = 0; i < info.paramTypes.size(); ++i) {
      if (i > 0) sig += ",";
      sig += info.paramTypes[i] ? info.paramTypes[i]->toString() : "?";
    }
    sig += ")";
    if (seenSignatures.insert(sig).second) {
      results.push_back(info);
    }
  };

  std::vector<FunctionInfo> allResults;

  for (auto* s = this; s != nullptr; s = s->parent) {
    s->collectFunctions(prefix, allResults);
    for (const auto& [childName, child] : s->childModules) {
      if (child && child->getType() == ScopeType::Import) {
        child->collectFunctions(prefix, allResults);
        for (const auto& [modName, modChild] : child->childModules) {
          if (modChild && modChild->getType() == ScopeType::Module) {
            modChild->collectFunctions(prefix, allResults);
          }
        }
      }
    }
    for (const auto& binding : s->importBindings) {
      if (!binding.sourceScope) continue;
      binding.sourceScope->collectFunctions(prefix, allResults);
    }
  }

  for (const auto& info : allResults) {
    addIfUnique(info);
  }
  return results;
}

// -------------------------------------------------------------------
// lookupFunction — overload resolution
// -------------------------------------------------------------------

// Helper: check if a function name is an intrinsic (starts with '_')
static bool isIntrinsic(const std::string& name) {
  return !name.empty() && name[0] == '_';
}

// Helper: check if argType is assignable to paramType
static bool isAssignableTo(const sun::TypePtr& from, const sun::TypePtr& to) {
  if (!from || !to) return false;
  if (to->equals(*from)) return true;

  // Numeric widening
  if (from->isPrimitive() && to->isPrimitive()) {
    auto fromKind = from->getKind();
    auto toKind = to->getKind();

    auto isInteger = [](sun::Type::Kind k) {
      return k == sun::Type::Kind::Int8 || k == sun::Type::Kind::Int16 ||
             k == sun::Type::Kind::Int32 || k == sun::Type::Kind::Int64 ||
             k == sun::Type::Kind::UInt8 || k == sun::Type::Kind::UInt16 ||
             k == sun::Type::Kind::UInt32 || k == sun::Type::Kind::UInt64;
    };

    auto intBitWidth = [](sun::Type::Kind k) -> int {
      switch (k) {
        case sun::Type::Kind::Int8:
        case sun::Type::Kind::UInt8:
          return 8;
        case sun::Type::Kind::Int16:
        case sun::Type::Kind::UInt16:
          return 16;
        case sun::Type::Kind::Int32:
        case sun::Type::Kind::UInt32:
          return 32;
        case sun::Type::Kind::Int64:
        case sun::Type::Kind::UInt64:
          return 64;
        default:
          return 0;
      }
    };

    if (isInteger(fromKind) && isInteger(toKind)) {
      return intBitWidth(fromKind) <= intBitWidth(toKind);
    }

    if ((fromKind == sun::Type::Kind::Float32 ||
         fromKind == sun::Type::Kind::Float64) &&
        (toKind == sun::Type::Kind::Float32 ||
         toKind == sun::Type::Kind::Float64)) {
      return true;
    }
  }

  // Unwrap reference types
  if (to->isReference() && from->isReference()) {
    auto* toRef = static_cast<const sun::ReferenceType*>(to.get());
    auto* fromRef = static_cast<const sun::ReferenceType*>(from.get());
    return isAssignableTo(fromRef->getReferencedType(),
                          toRef->getReferencedType());
  }

  // Interface assignability: class implements interface
  if (to->isInterface() && from->isClass()) {
    auto* classType = static_cast<const sun::ClassType*>(from.get());
    auto* ifaceType = static_cast<const sun::InterfaceType*>(to.get());
    return classType->implementsInterface(ifaceType->getName());
  }

  // Class -> ref Interface (class can be passed as ref to interface it implements)
  if (to->isReference() && from->isClass()) {
    auto* toRef = static_cast<const sun::ReferenceType*>(to.get());
    sun::TypePtr innerTo = toRef->getReferencedType();
    if (innerTo && innerTo->isInterface()) {
      auto* ifaceType = static_cast<const sun::InterfaceType*>(innerTo.get());
      auto* classType = static_cast<const sun::ClassType*>(from.get());
      return classType->implementsInterface(ifaceType->getName());
    }
  }

  // ref Class -> Interface (unwrap ref, check class implements interface)
  if (to->isInterface() && from->isReference()) {
    auto* fromRef = static_cast<const sun::ReferenceType*>(from.get());
    sun::TypePtr innerFrom = fromRef->getReferencedType();
    if (innerFrom && innerFrom->isClass()) {
      auto* ifaceType = static_cast<const sun::InterfaceType*>(to.get());
      auto* classType = static_cast<const sun::ClassType*>(innerFrom.get());
      return classType->implementsInterface(ifaceType->getName());
    }
  }

  // ref(T) -> T: value can be extracted from reference (copy semantics)
  if (!to->isReference() && from->isReference()) {
    auto* fromRef = static_cast<const sun::ReferenceType*>(from.get());
    return isAssignableTo(fromRef->getReferencedType(), to);
  }

  // Class-to-class: compare by mangled name
  if (to->isClass() && from->isClass()) {
    auto* toClass = static_cast<const sun::ClassType*>(to.get());
    auto* fromClass = static_cast<const sun::ClassType*>(from.get());
    return toClass->getMangledName() == fromClass->getMangledName();
  }

  return false;
}

std::optional<FunctionInfo> SemanticScopeBase::lookupFunction(
    const std::string& name,
    const std::vector<sun::TypePtr>& argTypes) const {
  std::string sig = name + "(";
  for (size_t i = 0; i < argTypes.size(); ++i) {
    if (i > 0) sig += ",";
    sig += argTypes[i] ? argTypes[i]->toString() : "?";
  }
  sig += ")";
  std::string prefix = name + "(";

  // Helper lambda to check compatible overloads in a FunctionTable
  auto findCompatible =
      [&](const FunctionTable& funcs) -> std::optional<FunctionInfo> {
    // Try exact match first
    auto it = funcs.find(sig);
    if (it != funcs.end()) return it->second;

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

          // raw_ptr<T> is compatible with raw_ptr<i8> for intrinsics
          if (argTypes[i]->isRawPointer() &&
              info->paramTypes[i]->isRawPointer() && isIntrinsic(baseName)) {
            auto* paramRawPtr = static_cast<const sun::RawPointerType*>(
                info->paramTypes[i].get());
            if (paramRawPtr->getPointeeType()->isInt8()) {
              continue;
            }
          }

          if (::isAssignableTo(argTypes[i], info->paramTypes[i])) {
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

    std::string baseName = prefix.substr(0, prefix.size() - 1);
    return checkOverloads(baseName);
  };

  auto findInScope =
      [&](const SemanticScopeBase* scope) -> std::optional<FunctionInfo> {
    return findCompatible(scope->functions);
  };

  // Walk scope chain
  for (auto* s = this; s != nullptr; s = s->parent) {
    auto result = findInScope(s);
    if (result) return result;
    for (const auto& [childName, child] : s->childModules) {
      if (child && child->getType() == ScopeType::Import) {
        result = findInScope(child.get());
        if (result) return result;
        for (const auto& [modName, modChild] : child->childModules) {
          if (modChild && modChild->getType() == ScopeType::Module) {
            result = findInScope(modChild.get());
            if (result) return result;
          }
        }
      }
    }
    for (const auto& binding : s->importBindings) {
      if (!binding.sourceScope) continue;
      result = findInScope(binding.sourceScope);
      if (result) return result;
    }
  }

  return std::nullopt;
}

// -------------------------------------------------------------------
// lookupModuleScope — find a module scope by dot-separated path
// -------------------------------------------------------------------
SemanticScopeBase* SemanticScopeBase::lookupModuleScope(
    const std::string& dotPath) const {
  if (dotPath.empty()) return nullptr;

  // Helper to find a segment in a scope, traversing through library scopes
  std::function<SemanticScopeBase*(const SemanticScopeBase&,
                                   const std::string&)>
      findInScope = [&](const SemanticScopeBase& scope,
                        const std::string& segment) -> SemanticScopeBase* {
    auto it = scope.childModules.find(segment);
    if (it != scope.childModules.end()) {
      return it->second.get();
    }

    // Search inside library/import scopes (transparent lookup)
    SemanticScopeBase* found = nullptr;
    for (const auto& [modName, child] : scope.childModules) {
      if (!child) continue;
      if (!isLibraryScope(modName) && modName != "__definition__") continue;

      auto childIt = child->childModules.find(segment);
      if (childIt != child->childModules.end()) {
        if (!found) {
          found = childIt->second.get();
        }
      }

      auto* nested = findInScope(*child, segment);
      if (nested && !found) {
        found = nested;
      }
    }
    return found;
  };

  for (auto* s = this; s != nullptr; s = s->parent) {
    const SemanticScopeBase* current = s;
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
    if (resolved) return const_cast<SemanticScopeBase*>(current);
  }
  return nullptr;
}

// -------------------------------------------------------------------
// getActiveUsingImports — collect all using imports from scope chain
// -------------------------------------------------------------------
std::vector<UsingImport> SemanticScopeBase::getActiveUsingImports() const {
  std::vector<UsingImport> result;
  for (auto* s = this; s != nullptr; s = s->parent) {
    result.insert(result.end(), s->usingImports.begin(), s->usingImports.end());
  }
  return result;
}

// -------------------------------------------------------------------
// getCurrentScopePath — get module path segments for current position
// -------------------------------------------------------------------
std::vector<std::string> SemanticScopeBase::getCurrentScopePath() const {
  // Walk up to find the nearest scope with a scopePath (Module or Import)
  for (auto* s = this; s != nullptr; s = s->parent) {
    if (!s->scopePath.empty()) {
      return s->scopePath;
    }
  }
  return {};
}

// -------------------------------------------------------------------
// isModuleName — check if a name refers to a module
// -------------------------------------------------------------------
bool SemanticScopeBase::isModuleName(const std::string& name) const {
  for (auto* s = this; s != nullptr; s = s->parent) {
    // Direct child module
    if (s->childModules.count(name) > 0) return true;
    // Search inside import/library scopes
    for (const auto& [modName, child] : s->childModules) {
      if (!child) continue;
      if (child->getType() == ScopeType::Import || isLibraryScope(modName)) {
        if (child->childModules.count(name) > 0) return true;
      }
    }
  }
  return false;
}

// -------------------------------------------------------------------
// resolveNameWithUsings — resolve a name through module scopes and usings
// -------------------------------------------------------------------

// Helper: collect ALL module scopes matching a path across import scopes
static std::vector<SemanticScopeBase*> collectAllModuleScopes(
    const SemanticScopeBase* startScope, const std::string& dotPath) {
  std::vector<SemanticScopeBase*> results;
  if (dotPath.empty() || !startScope) return results;

  auto addUnique = [&results](SemanticScopeBase* scope) {
    if (std::find(results.begin(), results.end(), scope) == results.end()) {
      results.push_back(scope);
    }
  };

  std::set<const SemanticScopeBase*> visitedScopes;

  std::function<void(const SemanticScopeBase&, const std::string&,
                     std::vector<SemanticScopeBase*>&)>
      findAllInScope = [&](const SemanticScopeBase& scope,
                           const std::string& segment,
                           std::vector<SemanticScopeBase*>& out) {
        if (segment.find('(') != std::string::npos) return;
        if (visitedScopes.count(&scope)) return;
        visitedScopes.insert(&scope);

        auto it = scope.childModules.find(segment);
        if (it != scope.childModules.end() && !isLibraryScope(segment)) {
          out.push_back(it->second.get());
        }

        for (const auto& [modName, child] : scope.childModules) {
          if (!child || !isLibraryScope(modName)) continue;
          auto childIt = child->childModules.find(segment);
          if (childIt != child->childModules.end()) {
            out.push_back(childIt->second.get());
          }
          findAllInScope(*child, segment, out);
        }
      };

  auto collectForPath = [&](const std::string& path) {
    std::vector<SemanticScopeBase*> currentScopes;

    for (auto* s = startScope; s != nullptr; s = s->parent) {
      std::string segment;
      std::istringstream stream(path);
      bool firstSegment = true;
      std::vector<SemanticScopeBase*> segScopes;

      while (std::getline(stream, segment, '.')) {
        segScopes.clear();
        if (firstSegment) {
          findAllInScope(*s, segment, segScopes);
          firstSegment = false;
        } else {
          std::vector<SemanticScopeBase*> nextScopes;
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

      for (const auto& binding : s->importBindings) {
        if (!binding.sourceScope || !binding.isWildcard) continue;
        if (binding.sourceScope->scopeName == path) {
          addUnique(binding.sourceScope);
        }
      }
    }
  };

  collectForPath(dotPath);

  // Also collect parent module scopes
  std::string parentPath = dotPath;
  size_t lastDot = parentPath.rfind('.');
  while (lastDot != std::string::npos) {
    parentPath = parentPath.substr(0, lastDot);
    collectForPath(parentPath);
    lastDot = parentPath.rfind('.');
  }

  return results;
}

sun::QualifiedName SemanticScopeBase::resolveNameWithUsings(
    const std::string& name) const {
  // Handle qualified (dotted) names like "sun.String"
  size_t lastDot = name.rfind('.');
  if (lastDot != std::string::npos) {
    std::string modulePath = name.substr(0, lastDot);
    std::string symbolName = name.substr(lastDot + 1);

    if (auto* modScope = lookupModuleScope(modulePath)) {
      if (modScope->hasSymbol(symbolName)) {
        return sun::QualifiedName(modScope->scopePath, symbolName);
      }
    }
  }

  // Helper to filter out $...$ hash segments from scope path
  auto getVisiblePath =
      [](const std::vector<std::string>& path) -> std::vector<std::string> {
    std::vector<std::string> result;
    for (const auto& segment : path) {
      if (!segment.empty() && segment[0] != '$') {
        result.push_back(segment);
      }
    }
    return result;
  };

  // Collect ALL candidate matches
  std::map<std::vector<std::string>,
           std::pair<std::vector<std::string>, SemanticScopeBase*>>
      candidates;

  auto addCandidate = [&](SemanticScopeBase* scope) {
    auto visPath = getVisiblePath(scope->scopePath);
    if (candidates.find(visPath) == candidates.end()) {
      candidates[visPath] = {scope->scopePath, scope};
    }
  };

  auto visiblePath = getVisiblePath(scopePath);

  // 1. Check enclosing module scopes by walking up the parent chain
  for (auto* s = this; s != nullptr; s = s->parent) {
    if (s->getType() == ScopeType::Module && s->hasSymbol(name)) {
      addCandidate(const_cast<SemanticScopeBase*>(s));
    }
  }

  // 2. If inside a module, check the module scope hierarchy via path lookup
  if (!visiblePath.empty()) {
    std::string visPathStr = sun::QualifiedName::joinPath(visiblePath);
    auto allScopes = collectAllModuleScopes(this, visPathStr);
    for (auto* modScope : allScopes) {
      if (modScope->hasSymbol(name)) {
        addCandidate(modScope);
      }
    }
  }

  // 3. Check using imports
  auto activeImports = getActiveUsingImports();
  for (const auto& import : activeImports) {
    if (!import.isWildcard && import.target != name) continue;

    auto allScopes = collectAllModuleScopes(this, import.namespacePath);
    for (auto* modScope : allScopes) {
      if (modScope->hasSymbol(name)) {
        addCandidate(modScope);
      }
    }
  }

  // 4. Check global scope (functions/classes defined outside any module)
  const SemanticScopeBase* rootScope = this;
  while (rootScope->parent != nullptr) {
    rootScope = rootScope->parent;
  }
  if (rootScope->getType() == ScopeType::Global && rootScope->hasSymbol(name)) {
    std::vector<std::string> emptyPath;
    if (candidates.find(emptyPath) == candidates.end()) {
      candidates[emptyPath] = {emptyPath,
                               const_cast<SemanticScopeBase*>(rootScope)};
    }
  }

  // Check for ambiguity
  if (candidates.size() > 1) {
    std::string paths;
    for (const auto& [visPath, info] : candidates) {
      if (!paths.empty()) paths += " or ";
      paths +=
          visPath.empty() ? "<global>" : sun::QualifiedName::joinPath(visPath);
    }
    logAndThrowError("Ambiguous reference to '" + name +
                     "'. Could be: " + paths);
  }

  // Return the single match, or unqualified name if no match
  if (candidates.size() == 1) {
    const auto& [visPath, info] = *candidates.begin();
    // If the symbol is a unique function in the matched scope, return its
    // actual qualified name (which includes paramSuffix for overload mangling)
    if (info.second) {
      if (auto* overloads = info.second->functions.getOverloads(name)) {
        if (overloads->size() == 1 &&
            !(*overloads)[0]->qualifiedName.empty()) {
          return (*overloads)[0]->qualifiedName;
        }
      }
    }
    return sun::QualifiedName(info.first, name);
  }

  // No match found - return unqualified name
  return sun::QualifiedName(std::vector<std::string>{}, name);
}

// -------------------------------------------------------------------
// lookupQualifiedVariable — find a module-qualified variable
// -------------------------------------------------------------------
VariableInfo* SemanticScopeBase::lookupQualifiedVariable(
    const std::string& qualifiedName) {
  // Split "module.varName" into module path and variable name
  size_t lastDot = qualifiedName.rfind('.');
  if (lastDot == std::string::npos) {
    return lookupVariable(qualifiedName);
  }

  std::string modulePath = qualifiedName.substr(0, lastDot);
  std::string varName = qualifiedName.substr(lastDot + 1);

  // Look up the module scope and search for the variable there
  auto* modScope = lookupModuleScope(modulePath);
  if (!modScope) return nullptr;

  // Search in the module scope's namespaced variables
  auto it = modScope->namespacedVariables.find(varName);
  if (it != modScope->namespacedVariables.end()) {
    return &it->second;
  }

  // Also check regular variables in the module scope
  auto varIt = modScope->variables.find(varName);
  if (varIt != modScope->variables.end()) {
    return &varIt->second;
  }

  return nullptr;
}

// -------------------------------------------------------------------
// lookupQualifiedFunction — find a module-qualified function
// -------------------------------------------------------------------
const FunctionInfo* SemanticScopeBase::lookupQualifiedFunction(
    const std::string& qualifiedName) const {
  // Split "module.funcName" into module path and function name
  size_t lastDot = qualifiedName.rfind('.');
  if (lastDot == std::string::npos) return nullptr;

  std::string modulePath = qualifiedName.substr(0, lastDot);
  std::string funcName = qualifiedName.substr(lastDot + 1);

  // Look up the module scope
  auto* modScope = lookupModuleScope(modulePath);
  if (!modScope) return nullptr;

  // Search for function by name in the module scope
  if (auto* overloads = modScope->functions.getOverloads(funcName)) {
    if (!overloads->empty()) {
      return (*overloads)[0];
    }
  }

  return nullptr;
}
