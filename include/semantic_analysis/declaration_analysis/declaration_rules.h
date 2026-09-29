/** Checks a declaration on its own, without looking anything up in scope. */
#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ast/ast_common.h"
#include "ast/ast_fwd.h"
#include "ast/type_annotation.h"
#include "support/error.h"
#include "support/position.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/**
 * Throws if the name is reserved for builtins (it starts with '_'). `kind`
 * names what is being declared in the message, such as "Class name".
 */
void validateNotReserved(const std::string& name, const std::string& kind,
                         std::optional<sun::support::Position> location);

/**
 * Rejects extern signatures that have no C spelling. Primitives,
 * raw_ptr&lt;T&gt;, `ref T` (C's T*) and objects by value all lower
 * correctly; arrays, slices, interfaces and lambdas do not, and must error
 * rather than silently miscompile. Needs the prototype's resolved types.
 */
void validateExternSignature(const sun::ast::FunctionAST& func);

/**
 * Rejects a '<'_>' lambda type in return position: its captured
 * environment lives in a stack frame that dies when the function returns.
 * With `allowNamed`, a lambda type whose frame has a lifetime name passes.
 */
void rejectRefEnvReturnType(
    const std::optional<sun::ast::TypeAnnotation>& returnType,
    const sun::support::Position& location, bool allowNamed = false);

/**
 * Throws if two lifetime parameters share a name, pointing at the first.
 * `owner` is appended to the message, such as " on class 'Bus'", or empty.
 */
void rejectDuplicateLifetimes(
    const std::vector<sun::ast::LifetimeParameter>& params,
    const std::string& owner);

/**
 * Checks each field name is not reserved and appears only once. Works for
 * class and interface fields alike. `fieldKind` names the fields in the
 * reserved-name message ("Field name"); `owner` names the declaration in the
 * duplicate message ("class 'Point'").
 */
template <typename FieldDecl>
void validateFieldNames(const std::vector<FieldDecl>& fields,
                        const std::string& fieldKind,
                        const std::string& owner) {
  std::set<std::string> seen;
  for (const auto& field : fields) {
    validateNotReserved(field.name, fieldKind, field.location);
    if (!seen.insert(field.name).second) {
      sun::support::logAndThrowError(
          "Field '" + field.name + "' already exists in " + owner,
          field.location);
    }
  }
}

}  // namespace sun::semantic_analysis
