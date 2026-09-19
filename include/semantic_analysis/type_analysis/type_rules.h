/** Applies type rules to syntax and reports source-located semantic errors. */

#pragma once

#include <optional>

#include "ast.h"
#include "semantic_analysis/type_analysis/type_checking.h"
#include "support/position.h"
#include "types/types.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis::type_analysis {
using sun::ast::ExprAST;

/**
 * Retype an integer literal as `targetType` when its value fits. Returns true
 * when it did. With `throwOnFail`, a value that does not fit is an error
 * rather than a silent no.
 */
bool tryCoerceIntegerLiteral(ExprAST* expr, sun::types::TypePtr targetType,
                             bool throwOnFail = false);

/**
 * Give an untyped numeric literal operand of a binary expression its type from
 * context: the surrounding expected type if there is one, otherwise the other
 * operand's type. Without this `u8_var + 32` would promote to the literal's
 * default i32.
 */
void coerceBinaryLiteralOperands(const sun::ast::BinaryExprAST& binExpr,
                                 const sun::types::TypePtr& expectedType);

/**
 * A char only compares with a char, and never takes part in arithmetic.
 * Without this `'a' + 1` and `c == 65` would quietly fall through to the
 * integer paths, since a char is an i32 underneath.
 */
void checkCharOperands(const sun::ast::BinaryExprAST& binExpr);

/**
 * Unify the branch types of a ternary expression: exact match, or the wider
 * type when one side widens to the other (never narrows f64 to f32). Throws a
 * compile error when the types are incompatible.
 */
sun::types::TypePtr unifyTernaryTypes(
    const sun::types::TypePtr& thenType, const sun::types::TypePtr& elseType,
    std::optional<sun::support::Position> loc);

}  // namespace sun::semantic_analysis::type_analysis
