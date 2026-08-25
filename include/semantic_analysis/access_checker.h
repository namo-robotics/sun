// access_checker.h — The single place that decides whether an item is
// accessible from a given module context and words the diagnostic.
//
// Used by the semantic analyzer at every use site, by the moon builder, and
// by tooling (e.g. LSP completions) that must hide inaccessible items.

#pragma once

#include <string>

#include "semantic_analysis/visibility.h"
#include "support/position.h"

namespace sun::access {

// Describes an item for the access predicate and its diagnostic.
struct ItemRef {
  const char* kind;           // "field", "method", "function", "class", ...
  std::string name;           // Item name as written by the user
  std::string ownerTypeName;  // Class/interface display name for members
  Visibility visibility;
  ModulePath owner;  // QualifiedName::owner() of the item (or type)
};

// "class 'Vec' in module 'sun'" | "module 'sun'" |
// "the top-level scope of its bundle" | "the top-level scope"
std::string describeOwner(const ItemRef& item);

// "'size_' is private to class 'Vec' in module 'sun'"
// "function 'helper' is private to module 'sun'"
std::string denialMessage(const ItemRef& item);

bool isAccessible(const ModulePath& from, const ItemRef& item);

[[noreturn]] void denyAccess(const ItemRef& item, const Position& loc);

// Throws a semantic error naming the item and its owner when inaccessible.
void requireAccessible(const ModulePath& from, const ItemRef& item,
                       const Position& loc);

}  // namespace sun::access
