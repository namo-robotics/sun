#include "access_checker.h"

#include "error.h"

namespace sun::access {

namespace {

bool isBundleOwned(const ModulePath& owner) {
  return !owner.empty() && isLibraryHashSegment(owner.front());
}

}  // namespace

std::string describeOwner(const ItemRef& item) {
  std::string modulePart;
  std::string moduleName = displayModulePath(item.owner);
  if (!moduleName.empty()) {
    modulePart = "module '" + moduleName + "'";
  } else if (isBundleOwned(item.owner)) {
    modulePart = "the top-level scope of its bundle";
  } else {
    modulePart = "the top-level scope";
  }
  if (item.ownerTypeName.empty()) return modulePart;
  return item.ownerTypeName + " in " + modulePart;
}

std::string denialMessage(const ItemRef& item) {
  std::string kind = item.kind ? item.kind : "item";
  std::string subject = kind == "field" || kind == "method"
                            ? "'" + item.name + "'"
                            : kind + " '" + item.name + "'";
  return subject + " is private to " + describeOwner(item) +
         " and cannot be accessed here";
}

bool isAccessible(const ModulePath& from, const ItemRef& item) {
  return isAccessibleFrom(from, item.visibility, item.owner);
}

void denyAccess(const ItemRef& item, const Position& loc) {
  logSemanticError(denialMessage(item), loc);
}

void requireAccessible(const ModulePath& from, const ItemRef& item,
                       const Position& loc) {
  if (!isAccessible(from, item)) denyAccess(item, loc);
}

}  // namespace sun::access

// ---------------------------------------------------------------------------
// SemanticAnalyzer integration
// ---------------------------------------------------------------------------

#include "semantic_analyzer.h"

sun::ModulePath SemanticAnalyzer::currentModulePath() const {
  if (!accessContextStack_.empty()) return accessContextStack_.back();
  for (auto* s = currentScope; s != nullptr; s = s->parent) {
    if (s->getType() == ScopeType::Module) return s->scopePath;
  }
  return {};
}

void SemanticAnalyzer::denyAccess(const sun::access::ItemRef& item) const {
  auto loc = currentLocation();
  logSemanticError(sun::access::denialMessage(item), loc);
}

sun::Visibility SemanticAnalyzer::methodVisibility(const FunctionAST& method) {
  if (method.getProto().getName() == "deinit") return sun::Visibility::Public;
  return method.getVisibility();
}

// Display name without library-hash prefixes ("$hash$.sun.Vec<i32>" -> "sun.Vec<i32>")
static std::string cleanTypeName(std::string name) {
  while (!name.empty() && name.front() == '$') {
    size_t close = name.find('$', 1);
    if (close == std::string::npos) break;
    size_t cut = close + 1;
    if (cut < name.size() && (name[cut] == '.' || name[cut] == '_')) ++cut;
    name.erase(0, cut);
  }
  return name;
}

// Members are owned by their type's module
sun::access::ItemRef SemanticAnalyzer::fieldRef(const sun::ClassType& cls,
                                                const sun::ClassField& f) {
  return {"field", f.name, "class '" + cleanTypeName(cls.getDisplayName()) + "'",
          f.visibility, cls.getQualifiedName().owner()};
}

sun::access::ItemRef SemanticAnalyzer::methodRef(const sun::ClassType& cls,
                                                 const sun::ClassMethod& m) {
  return {"method", m.name,
          "class '" + cleanTypeName(cls.getDisplayName()) + "'", m.visibility,
          cls.getQualifiedName().owner()};
}

sun::access::ItemRef SemanticAnalyzer::fieldRef(const sun::InterfaceType& iface,
                                                const sun::InterfaceField& f) {
  return {"field", f.name, "interface '" + iface.getBaseName() + "'",
          f.visibility, iface.getQualifiedName().owner()};
}

sun::access::ItemRef SemanticAnalyzer::methodRef(
    const sun::InterfaceType& iface, const sun::InterfaceMethod& m) {
  return {"method", m.name, "interface '" + iface.getBaseName() + "'",
          m.visibility, iface.getQualifiedName().owner()};
}


const sun::ClassField* SemanticAnalyzer::accessibleField(
    const sun::ClassType& cls, const std::string& name,
    const Position& loc) const {
  const auto* f = cls.getField(name);
  if (f) requireAccessible(fieldRef(cls, *f), loc);
  return f;
}

const sun::ClassMethod* SemanticAnalyzer::accessibleMethod(
    const sun::ClassType& cls, const std::string& name,
    const Position& loc) const {
  const auto* m = cls.getMethod(name);
  if (m) requireAccessible(methodRef(cls, *m), loc);
  return m;
}

const sun::ClassMethod* SemanticAnalyzer::accessibleMethodForArgs(
    const sun::ClassType& cls, const std::string& name,
    const std::vector<sun::TypePtr>& argTypes, const Position& loc) const {
  const auto* m = cls.getMethodForArgs(name, argTypes);
  if (m) requireAccessible(methodRef(cls, *m), loc);
  return m;
}

const sun::InterfaceField* SemanticAnalyzer::accessibleField(
    const sun::InterfaceType& iface, const std::string& name,
    const Position& loc) const {
  const auto* f = iface.getField(name);
  if (f) requireAccessible(fieldRef(iface, *f), loc);
  return f;
}

const sun::InterfaceMethod* SemanticAnalyzer::accessibleMethod(
    const sun::InterfaceType& iface, const std::string& name,
    const Position& loc) const {
  const auto* m = iface.getMethod(name);
  if (m) requireAccessible(methodRef(iface, *m), loc);
  return m;
}

void SemanticAnalyzer::requireModuleAccessible(
    const SemanticScopeBase& moduleScope, const Position& loc) const {
  for (auto* s = &moduleScope; s && s->getType() == ScopeType::Module;
       s = s->parent) {
    if (isLibraryScope(s->scopeName)) continue;  // bundle boundary, not a module
    requireAccessible(moduleRef(static_cast<const ModuleScope&>(*s)), loc);
  }
}
