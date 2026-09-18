#include "semantic_analysis/access_checker.h"

#include "support/error.h"

namespace sun::access {

namespace {

/** Read the declaring module from semantic ownership, including imported items.
 */
DeclarationId ownerModule(const ItemRef& item, const DeclarationTable& table) {
  return item.declaration ? table.get(item.declaration).module
                          : DeclarationId{};
}

/** Build a diagnostic path from the module ownership chain. */
ModulePath ownerPath(const ItemRef& item, const DeclarationTable& table) {
  ModulePath path;
  for (auto module = ownerModule(item, table); module;
       module = table.get(module).owner) {
    const auto& record = table.get(module);
    if (record.kind != DeclarationKind::Module)
      logAndThrowError("Visibility ownership must refer to a module");
    path.insert(path.begin(), record.name);
  }
  return path;
}

}  // namespace

std::string describeOwner(const ItemRef& item, const DeclarationTable& table) {
  const auto owner = ownerPath(item, table);
  std::string modulePart;
  std::string moduleName = displayModulePath(owner);
  if (!moduleName.empty()) {
    modulePart = "module '" + moduleName + "'";
  } else if (!owner.empty() && isLibraryHashSegment(owner.front())) {
    modulePart = "the top-level scope of its bundle";
  } else {
    modulePart = "the top-level scope";
  }
  if (item.ownerTypeName.empty()) return modulePart;
  return item.ownerTypeName + " in " + modulePart;
}

std::string denialMessage(const ItemRef& item, const DeclarationTable& table) {
  std::string kind = item.kind ? item.kind : "item";
  std::string subject = kind == "field" || kind == "method"
                            ? "'" + item.name + "'"
                            : kind + " '" + item.name + "'";
  return subject + " is private to " + describeOwner(item, table) +
         " and cannot be accessed here";
}

bool isAccessible(DeclarationId from, const ItemRef& item,
                  const DeclarationTable& table) {
  if (item.visibility == Visibility::Public) return true;
  auto owner = ownerModule(item, table);
  if (!owner) return true;
  for (auto module = from; module; module = table.get(module).owner) {
    if (module == owner) return true;
  }
  return false;
}

void denyAccess(const ItemRef& item, const Position& loc,
                const DeclarationTable& table) {
  logSemanticError(denialMessage(item, table), loc);
}

void requireAccessible(DeclarationId from, const ItemRef& item,
                       const Position& loc, const DeclarationTable& table) {
  if (!isAccessible(from, item, table)) denyAccess(item, loc, table);
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
          cls.getDeclarationId()};
}

ItemRef methodRef(const sun::ClassType& cls, const sun::ClassMethod& m) {
  return {"method", m.name,
          "class '" + cleanTypeName(cls.getDisplayName()) + "'", m.visibility,
          cls.getDeclarationId()};
}

ItemRef fieldRef(const sun::InterfaceType& iface,
                 const sun::InterfaceField& f) {
  return {"field", f.name, "interface '" + iface.getBaseName() + "'",
          f.visibility, iface.getDeclarationId()};
}

ItemRef methodRef(const sun::InterfaceType& iface,
                  const sun::InterfaceMethod& m) {
  return {"method", m.name, "interface '" + iface.getBaseName() + "'",
          m.visibility, iface.getDeclarationId()};
}

}  // namespace sun::access
