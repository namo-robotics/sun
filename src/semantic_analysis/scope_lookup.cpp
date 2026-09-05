// scope_lookup.cpp — Scope-chain lookup methods on SemanticScope
//
// These methods traverse the parent chain, import scopes and import bindings
// to find symbols. They encapsulate the lookup
// logic that was previously spread across SemanticAnalyzer.

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "semantic_analysis/semantic_scope.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_rules.h"
#include "support/error.h"

using sun::names::isIntrinsic;
using sun::rules::isAssignableTo;

namespace {

// A module-qualified name split at its last dot: "std.io.File" names the
// symbol "File" in module "std.io". Falsy when the name carries no module.
// Purely syntactic — finding the module is the caller's step, since callers
// differ on what an unknown module should mean.
struct DottedName {
  std::string modulePath;
  std::string symbol;
  explicit operator bool() const { return !modulePath.empty(); }
};

DottedName splitDotted(const std::string& name) {
  size_t lastDot = name.rfind('.');
  if (lastDot == std::string::npos) return {};
  return {name.substr(0, lastDot), name.substr(lastDot + 1)};
}

}  // namespace

bool SemanticScope::hasSymbol(const std::string& name) const {
  if (classes.contains(name)) return true;
  if (genericClasses.contains(name)) return true;
  if (interfaces.contains(name)) return true;
  if (genericInterfaces.contains(name)) return true;
  if (enums.contains(name)) return true;
  if (genericEnums.contains(name)) return true;
  if (namespacedVariables.contains(name)) return true;

  // Check functions by name (O(1) via indexed FunctionTable)
  if (functions.hasName(name)) return true;

  return false;
}

bool SemanticScope::hasAccessibleSymbol(const std::string& name,
                                        AccessFilter& filter) const {
  if (!filter.enabled()) return hasSymbol(name);
  if (auto it = classes.find(name);
      it != classes.end() && filter.admit(it->second))
    return true;
  if (auto it = genericClasses.find(name);
      it != genericClasses.end() && filter.admit(&it->second))
    return true;
  if (auto it = interfaces.find(name);
      it != interfaces.end() && filter.admit(it->second))
    return true;
  if (auto it = genericInterfaces.find(name);
      it != genericInterfaces.end() && filter.admit(&it->second))
    return true;
  if (auto it = enums.find(name); it != enums.end() && filter.admit(it->second))
    return true;
  if (auto it = genericEnums.find(name);
      it != genericEnums.end() && filter.admit(&it->second))
    return true;
  if (auto it = namespacedVariables.find(name);
      it != namespacedVariables.end() && filter.admit(&it->second))
    return true;
  if (auto* overloads = functions.getOverloads(name)) {
    for (const auto* info : *overloads) {
      if (filter.admit(info)) return true;
    }
  }
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

const GenericEnumInfo* SemanticScope::findGenericEnum(
    const std::string& name) const {
  auto it = genericEnums.find(name);
  if (it != genericEnums.end()) return &it->second;
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
  if (auto* mod = dynamic_cast<ModuleScope*>(clone.get())) {
    const auto& src = static_cast<const ModuleScope&>(*this);
    mod->qualifiedName = src.qualifiedName;
    mod->visibility = src.visibility;
    mod->visibilityDeclared = src.visibilityDeclared;
  }
  // Copy all symbol tables (shallow — shares type objects)
  clone->functions = functions;
  clone->classes = classes;
  clone->classDefinitions = classDefinitions;
  clone->genericClasses = genericClasses;
  clone->interfaces = interfaces;
  clone->genericInterfaces = genericInterfaces;
  clone->enums = enums;
  clone->genericEnums = genericEnums;
  clone->genericFunctions = genericFunctions;
  clone->childModules = childModules;
  clone->namespacedVariables = namespacedVariables;
  clone->variables = variables;
  clone->usingImports = usingImports;
  clone->importBindings = importBindings;
  return clone;
}

// -------------------------------------------------------------------
// lookupClass — find a class in the scope chain
// -------------------------------------------------------------------
std::shared_ptr<sun::ClassType> SemanticScopeBase::lookupClass(
    const std::string& name) const {
  if (auto dotted = splitDotted(name)) {
    if (auto* modScope = lookupModuleScope(dotted.modulePath)) {
      if (auto found = modScope->findClass(dotted.symbol)) return found;
    }
  }
  return lookupInChain<std::shared_ptr<sun::ClassType>>(
      name,
      [&](const SemanticScopeBase* scope) { return scope->findClass(name); });
}

// -------------------------------------------------------------------
// lookupGenericClass — find a generic class in the scope chain
// Uses lookupGenericSymbol-style traversal for module-qualified names
// -------------------------------------------------------------------
const GenericClassInfo* SemanticScopeBase::lookupGenericClass(
    const std::string& name) const {
  // Handle module-qualified names like "Test.Inner" or "std.Vec"
  if (auto dotted = splitDotted(name)) {
    if (auto* modScope = lookupModuleScope(dotted.modulePath)) {
      if (auto* found = modScope->findGenericClass(dotted.symbol)) return found;
    }
  }

  return lookupInChain<const GenericClassInfo*>(
      name, [&](const SemanticScopeBase* scope) {
        return scope->findGenericClass(name);
      });
}

const GenericClassInfo* SemanticScopeBase::lookupGenericClass(
    const sun::QualifiedName& qualifiedName) const {
  if (qualifiedName.scopePath.empty()) {
    return lookupGenericClass(qualifiedName.baseName);
  }
  auto* modScope = lookupModuleScope(qualifiedName.scopePathString());
  return modScope ? modScope->findGenericClass(qualifiedName.baseName)
                  : nullptr;
}

// -------------------------------------------------------------------
// lookupInterface — find an interface in the scope chain
// -------------------------------------------------------------------
std::shared_ptr<sun::InterfaceType> SemanticScopeBase::lookupInterface(
    const std::string& name) const {
  if (auto dotted = splitDotted(name)) {
    if (auto* modScope = lookupModuleScope(dotted.modulePath)) {
      if (auto found = modScope->findInterface(dotted.symbol)) return found;
    }
  }
  return lookupInChain<std::shared_ptr<sun::InterfaceType>>(
      name, [&](const SemanticScopeBase* scope) {
        return scope->findInterface(name);
      });
}

// -------------------------------------------------------------------
// lookupGenericInterface — find a generic interface in the scope chain
// -------------------------------------------------------------------
const GenericInterfaceInfo* SemanticScopeBase::lookupGenericInterface(
    const std::string& name) const {
  // Handle module-qualified names like "Test.Inner" or "std.IIterator"
  if (auto dotted = splitDotted(name)) {
    if (auto* modScope = lookupModuleScope(dotted.modulePath)) {
      if (auto* found = modScope->findGenericInterface(dotted.symbol)) {
        return found;
      }
    }
  }

  return lookupInChain<const GenericInterfaceInfo*>(
      name, [&](const SemanticScopeBase* scope) {
        return scope->findGenericInterface(name);
      });
}

// -------------------------------------------------------------------
// lookupEnum — find an enum in the scope chain
// -------------------------------------------------------------------
std::shared_ptr<sun::EnumType> SemanticScopeBase::lookupEnum(
    const std::string& name) const {
  if (auto dotted = splitDotted(name)) {
    if (auto* modScope = lookupModuleScope(dotted.modulePath)) {
      if (auto found = modScope->findEnum(dotted.symbol)) return found;
    }
  }
  return lookupInChain<std::shared_ptr<sun::EnumType>>(
      name,
      [&](const SemanticScopeBase* scope) { return scope->findEnum(name); });
}

// -------------------------------------------------------------------
// lookupGenericEnum — find a generic enum template in the scope chain
// -------------------------------------------------------------------
const GenericEnumInfo* SemanticScopeBase::lookupGenericEnum(
    const std::string& name) const {
  if (auto dotted = splitDotted(name)) {
    if (auto* modScope = lookupModuleScope(dotted.modulePath)) {
      if (auto* found = modScope->findGenericEnum(dotted.symbol)) return found;
    }
  }
  return lookupInChain<const GenericEnumInfo*>(
      name, [&](const SemanticScopeBase* scope) {
        return scope->findGenericEnum(name);
      });
}

// -------------------------------------------------------------------
// lookupVariable — find a variable in the scope chain
// -------------------------------------------------------------------
VariableInfo* SemanticScopeBase::lookupVariable(const std::string& name) {
  AccessFilter filter(this);
  auto probe = [&](SemanticScopeBase* s) -> VariableInfo* {
    auto found = s->variables.find(name);
    if (found == s->variables.end()) return nullptr;
    return filter.admit(&found->second) ? &found->second : nullptr;
  };
  for (auto* s = this; s != nullptr; s = s->parent) {
    if (auto* v = probe(s)) return v;
    // Search direct import-scope children
    for (const auto& [childName, child] : s->childModules) {
      if (child && child->getType() == ScopeType::Import) {
        if (auto* v = probe(child.get())) return v;
      }
    }
    // Search import bindings from using statements
    for (const auto& binding : s->importBindings) {
      if (!s->admitsImport(binding.sourceFileId)) continue;
      if (!binding.sourceScope ||
          (!binding.isWildcard && binding.localName != name))
        continue;
      if (auto* v = probe(binding.sourceScope)) return v;
    }
  }
  filter.finish();
  return nullptr;
}

// -------------------------------------------------------------------
// lookupGenericFunction — find a generic function in the scope chain
// -------------------------------------------------------------------
const GenericFunctionInfo* SemanticScopeBase::lookupGenericFunction(
    const std::string& name) const {
  auto scopePath = getCurrentScopePath();
  sun::QualifiedName qname(scopePath, name);
  AccessFilter filter(this);
  auto probe = [&](const SemanticScopeBase* s,
                   const sun::QualifiedName& qn) -> const GenericFunctionInfo* {
    auto found = s->genericFunctions.find(qn);
    if (found == s->genericFunctions.end()) return nullptr;
    return filter.admit(&found->second) ? &found->second : nullptr;
  };

  for (auto* s = this; s != nullptr; s = s->parent) {
    if (auto* g = probe(s, qname)) return g;
    // Try global scope (empty scope path)
    if (!scopePath.empty()) {
      if (auto* g = probe(s, sun::QualifiedName({}, name))) return g;
    }
    // Search direct import-scope children
    for (const auto& [modName, child] : s->childModules) {
      if (!child || child->getType() != ScopeType::Import) continue;
      if (auto* g = probe(child.get(), {child->scopePath, name})) return g;
      for (const auto& [subName, subChild] : child->childModules) {
        if (!subChild || subChild->getType() == ScopeType::Import) continue;
        if (auto* g = probe(subChild.get(), {subChild->scopePath, name}))
          return g;
      }
    }
    // Search import bindings
    for (const auto& binding : s->importBindings) {
      if (!s->admitsImport(binding.sourceFileId)) continue;
      if (!binding.sourceScope ||
          (!binding.isWildcard && binding.localName != name))
        continue;
      if (auto* g = probe(binding.sourceScope,
                          {binding.sourceScope->scopePath, name}))
        return g;
    }
  }
  filter.finish();
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

  auto collectFrom = [&](const SemanticScopeBase* s) {
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
      if (!s->admitsImport(binding.sourceFileId)) continue;
      if (!binding.sourceScope ||
          (!binding.isWildcard && binding.localName != name))
        continue;
      binding.sourceScope->collectFunctions(prefix, allResults);
    }
  };

  for (auto* s = this; s != nullptr; s = s->parent) {
    collectFrom(s);
  }

  AccessFilter filter(this);
  for (const auto& info : allResults) {
    if (filter.admit(info)) addIfUnique(info);
  }
  if (results.empty()) filter.finish();
  return results;
}

// -------------------------------------------------------------------
// lookupFunction — overload resolution
// -------------------------------------------------------------------

// Resolve an overload against this scope's own function table only, without
// walking to parents. Module-qualified calls need this: the callee's scope is
// already known, and walking up from it would let unrelated same-named
// functions in enclosing scopes win.
std::optional<FunctionInfo> SemanticScopeBase::lookupFunctionLocal(
    const std::string& name, const std::vector<sun::TypePtr>& argTypes,
    AccessFilter* filter) const {
  const FunctionTable& funcs = functions;
  auto admit = [&](const FunctionInfo& info) {
    return !filter || filter->admit(info);
  };

  std::string sig = name + "(";
  for (size_t i = 0; i < argTypes.size(); ++i) {
    if (i > 0) sig += ",";
    sig += argTypes[i] ? argTypes[i]->toString() : "?";
  }
  sig += ")";
  std::string prefix = name + "(";

  {
    // Try exact match first
    auto it = funcs.find(sig);
    if (it != funcs.end() && admit(it->second)) return it->second;

    // Try compatible overload — look up by base name
    auto checkOverloads =
        [&](const std::string& baseName) -> std::optional<FunctionInfo> {
      auto* overloads = funcs.getOverloads(baseName);
      if (!overloads) return std::nullopt;
      for (const auto* info : *overloads) {
        // A C-variadic callee only fixes its leading parameters; anything
        // past them is checked by the C side, not here.
        if (info->isCVariadic ? argTypes.size() < info->paramTypes.size()
                              : info->paramTypes.size() != argTypes.size()) {
          continue;
        }
        if (!admit(*info)) continue;

        bool compatible = true;
        for (size_t i = 0; i < info->paramTypes.size(); ++i) {
          if (!argTypes[i] || !info->paramTypes[i]) {
            compatible = false;
            break;
          }
          if (info->paramTypes[i]->equals(*argTypes[i])) continue;

          if (info->paramTypes[i]->isReference()) {
            auto* refType = static_cast<const sun::ReferenceType*>(
                info->paramTypes[i].get());
            if (refType->getReferencedType()->equals(*argTypes[i])) continue;
            // A borrow handed to a parameter of the other mutability: only
            // ref -> const ref is allowed
            if (argTypes[i]->isReference()) {
              auto* argRef =
                  static_cast<const sun::ReferenceType*>(argTypes[i].get());
              if (sun::refMutabilityConvertible(*argRef, *refType) &&
                  refType->getReferencedType()->equals(
                      *argRef->getReferencedType()))
                continue;
            }
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
            // Reading the value out of the borrow, so only for a scalar
            // parameter type (see isAssignableTo above)
            if (info->paramTypes[i]->equals(*refType->getReferencedType()) &&
                sun::typeCopiesByRead(info->paramTypes[i]))
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

          // raw_ptr<T> is compatible with byte pointers (raw_ptr<i8>/u8)
          // for intrinsics
          if (argTypes[i]->isRawPointer() &&
              info->paramTypes[i]->isRawPointer() && isIntrinsic(baseName)) {
            auto* paramRawPtr = static_cast<const sun::RawPointerType*>(
                info->paramTypes[i].get());
            if (paramRawPtr->getPointeeType()->isInt8() ||
                paramRawPtr->getPointeeType()->isUInt8()) {
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
  }
}

std::optional<FunctionInfo> SemanticScopeBase::lookupFunction(
    const std::string& name, const std::vector<sun::TypePtr>& argTypes) const {
  AccessFilter filter(this);
  auto findInScope =
      [&](const SemanticScopeBase* scope) -> std::optional<FunctionInfo> {
    return scope->lookupFunctionLocal(name, argTypes, &filter);
  };

  // One scope plus its import children and import bindings
  auto searchScope =
      [&](const SemanticScopeBase* s) -> std::optional<FunctionInfo> {
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
      if (!s->admitsImport(binding.sourceFileId)) continue;
      if (!binding.sourceScope ||
          (!binding.isWildcard && binding.localName != name))
        continue;
      result = findInScope(binding.sourceScope);
      if (result) return result;
    }
    return std::nullopt;
  };

  // Walk the scope chain (generic bodies are analyzed inside their
  // definition scope, so the chain already contains their module)
  for (auto* s = this; s != nullptr; s = s->parent) {
    if (auto result = searchScope(s)) return result;
  }

  filter.finish();
  return std::nullopt;
}

// -------------------------------------------------------------------
// lookupModuleScope — find a module scope by dot-separated path
// -------------------------------------------------------------------
SemanticScopeBase* SemanticScopeBase::lookupModuleScope(
    const std::string& dotPath) const {
  if (dotPath.empty()) return nullptr;
  const auto* root = this;
  while (root->parent) root = root->parent;
  if (dotPath.starts_with("$")) {
    auto it = root->canonicalModules.find(dotPath);
    return it == root->canonicalModules.end() ? nullptr : it->second;
  }

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
      if (!isLibraryScope(modName)) continue;

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
    for (const auto& import : s->usingImports) {
      if (s->admitsImport(import.sourceFileId)) result.push_back(import);
    }
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
  if (lookupModuleScope(name)) return true;
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
  bool pinned = dotPath.starts_with("$");
  if (pinned) {
    if (auto* scope = startScope->lookupModuleScope(dotPath))
      results.push_back(scope);
    return results;
  }

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
        if (!s->admitsImport(binding.sourceFileId)) continue;
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
  // Handle qualified (dotted) names like "std.String"
  if (auto dotted = splitDotted(name)) {
    if (auto* modScope = lookupModuleScope(dotted.modulePath)) {
      AccessFilter qualifiedFilter(this);
      if (modScope->hasAccessibleSymbol(dotted.symbol, qualifiedFilter)) {
        return sun::QualifiedName(modScope->scopePath, dotted.symbol);
      }
      qualifiedFilter.finish();
    }
  }
  AccessFilter filter(this);

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
    if (s->getType() == ScopeType::Module &&
        s->hasAccessibleSymbol(name, filter)) {
      addCandidate(const_cast<SemanticScopeBase*>(s));
    }
  }

  // 2. If inside a module, check the module scope hierarchy via path lookup
  if (!visiblePath.empty()) {
    std::string visPathStr = sun::QualifiedName::joinPath(visiblePath);
    auto allScopes = collectAllModuleScopes(this, visPathStr);
    for (auto* modScope : allScopes) {
      if (modScope->hasAccessibleSymbol(name, filter)) {
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
      if (modScope->hasAccessibleSymbol(name, filter)) {
        addCandidate(modScope);
      }
    }
  }

  // 4. Check global scope (functions/classes defined outside any module)
  const SemanticScopeBase* rootScope = this;
  while (rootScope->parent != nullptr) {
    rootScope = rootScope->parent;
  }
  if (rootScope->getType() == ScopeType::Global &&
      rootScope->hasAccessibleSymbol(name, filter)) {
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
        if (overloads->size() == 1 && !(*overloads)[0]->qualifiedName.empty()) {
          return (*overloads)[0]->qualifiedName;
        }
      }
    }
    return sun::QualifiedName(info.first, name);
  }

  // No match found: report a private candidate if that is all there was,
  // otherwise return the unqualified name
  filter.finish();
  return sun::QualifiedName(std::vector<std::string>{}, name);
}

// -------------------------------------------------------------------
// lookupQualifiedVariable — find a module-qualified variable
// -------------------------------------------------------------------
VariableInfo* SemanticScopeBase::lookupQualifiedVariable(
    const std::string& qualifiedName) {
  // An unqualified name is an ordinary variable; a qualified one must come
  // from the named module or not at all.
  auto dotted = splitDotted(qualifiedName);
  if (!dotted) return lookupVariable(qualifiedName);

  auto* modScope = lookupModuleScope(dotted.modulePath);
  if (!modScope) return nullptr;

  AccessFilter filter(this);
  // Search in the module scope's namespaced variables
  auto it = modScope->namespacedVariables.find(dotted.symbol);
  if (it != modScope->namespacedVariables.end() && filter.admit(&it->second)) {
    return &it->second;
  }

  // Also check regular variables in the module scope
  auto varIt = modScope->variables.find(dotted.symbol);
  if (varIt != modScope->variables.end() && filter.admit(&varIt->second)) {
    return &varIt->second;
  }

  filter.finish();
  return nullptr;
}

// -------------------------------------------------------------------
// lookupQualifiedFunction — find a module-qualified function
// -------------------------------------------------------------------
const FunctionInfo* SemanticScopeBase::lookupQualifiedFunction(
    const std::string& qualifiedName) const {
  // Qualified only: an unqualified name is not this function's business
  auto dotted = splitDotted(qualifiedName);
  if (!dotted) return nullptr;

  auto* modScope = lookupModuleScope(dotted.modulePath);
  if (!modScope) return nullptr;

  // Search for function by name in the module scope
  AccessFilter filter(this);
  if (auto* overloads = modScope->functions.getOverloads(dotted.symbol)) {
    for (const auto* info : *overloads) {
      if (filter.admit(info)) return info;
    }
  }

  filter.finish();
  return nullptr;
}
