#include "semantic_analysis/access_checker.h"

#include "support/error.h"

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
// Naming class and interface members (see item_refs.h)
// ---------------------------------------------------------------------------

#include "semantic_analysis/item_refs.h"

namespace sun::access {

namespace {

// Display name without library-hash prefixes ("$hash$.std.Vec<i32>" ->
// "std.Vec<i32>")
std::string cleanTypeName(std::string name) {
  while (!name.empty() && name.front() == '$') {
    size_t close = name.find('$', 1);
    if (close == std::string::npos) break;
    size_t cut = close + 1;
    if (cut < name.size() && (name[cut] == '.' || name[cut] == '_')) ++cut;
    name.erase(0, cut);
  }
  return name;
}

}  // namespace

// Constructors and destructors are always public: they are declared without
// a visibility keyword, and scope exit must be able to run deinit anywhere.
Visibility methodVisibility(const FunctionAST& method) {
  const std::string& name = method.getProto().getName();
  if (name == "init" || name == "deinit") return Visibility::Public;
  return method.getVisibility();
}

// Members are owned by their type's module
ItemRef fieldRef(const sun::ClassType& cls, const sun::ClassField& f) {
  return {"field", f.name,
          "class '" + cleanTypeName(cls.getDisplayName()) + "'", f.visibility,
          cls.getQualifiedName().owner()};
}

ItemRef methodRef(const sun::ClassType& cls, const sun::ClassMethod& m) {
  return {"method", m.name,
          "class '" + cleanTypeName(cls.getDisplayName()) + "'", m.visibility,
          cls.getQualifiedName().owner()};
}

ItemRef fieldRef(const sun::InterfaceType& iface,
                 const sun::InterfaceField& f) {
  return {"field", f.name, "interface '" + iface.getBaseName() + "'",
          f.visibility, iface.getQualifiedName().owner()};
}

ItemRef methodRef(const sun::InterfaceType& iface,
                  const sun::InterfaceMethod& m) {
  return {"method", m.name, "interface '" + iface.getBaseName() + "'",
          m.visibility, iface.getQualifiedName().owner()};
}

}  // namespace sun::access
