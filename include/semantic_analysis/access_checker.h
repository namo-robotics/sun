// access_checker.h — The single place that decides whether an item is
// accessible from a given module context and words the diagnostic.
//
// Used by the semantic analyzer at every use site, by the moon builder, and
// by tooling (e.g. LSP completions) that must hide inaccessible items.

#pragma once

#include <string>

#include "semantic_analysis/declaration_table.h"
#include "semantic_analysis/visibility.h"
#include "support/position.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Describe a declaration for access checks and diagnostics. */
struct ItemRef {
  const char* kind;           // "field", "method", "function", "class", ...
  std::string name;           // Item name as written by the user
  std::string ownerTypeName;  // Class/interface display name for members
  Visibility visibility;
  DeclarationId declaration;  // The item or the type declaring its members.
};

/** Describe the declaring module and enclosing type for diagnostics. */
std::string describeOwner(const ItemRef& item, const DeclarationTable& table);

/** Explain why a declaration is inaccessible. */
std::string denialMessage(const ItemRef& item, const DeclarationTable& table);

/** Check visibility using the declaring module and its ancestors. */
bool isAccessible(DeclarationId from, const ItemRef& item,
                  const DeclarationTable& table);

/** Report an inaccessible declaration at its use site. */
[[noreturn]] void denyAccess(const ItemRef& item,
                             const sun::support::Position& loc,
                             const DeclarationTable& table);

/** Reject a use outside the declaration's allowed module scope. */
void requireAccessible(DeclarationId from, const ItemRef& item,
                       const sun::support::Position& loc,
                       const DeclarationTable& table);

}  // namespace sun::semantic_analysis
