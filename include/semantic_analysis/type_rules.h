// type_rules.h — Type rules that depend on nothing but the types (and, for
// literal coercion, the expression node being retyped).
//
// These are the answers to "does this fit there?" questions the analyzer asks
// at assignments, arguments, returns and binary operators. They hold no
// analyzer state, so overload resolution inside the scope tree can reach the
// same rules the analyzer uses instead of keeping its own copy.

#pragma once

#include <optional>

#include "ast.h"
#include "semantic_analysis/types.h"
#include "support/position.h"

namespace sun::rules {

/**
 * True when a value of type `from` may be used where `to` is expected.
 * Covers exact equality, integer widening, f32/f64 conversion, static_ptr to
 * raw_ptr narrowing, class-to-interface conformance (through a `ref` on either
 * side), a non-throwing lambda where a throwing one is expected, and reading a
 * scalar out of a borrow. A compound read out of a borrow is rejected: that
 * would give the copy and the borrowed value the same buffer.
 */
bool isAssignableTo(const sun::TypePtr& from, const sun::TypePtr& to);

/**
 * Retype an integer literal as `targetType` when its value fits. Returns true
 * when it did. With `throwOnFail`, a value that does not fit is an error
 * rather than a silent no.
 */
bool tryCoerceIntegerLiteral(ExprAST* expr, sun::TypePtr targetType,
                             bool throwOnFail = false);

/**
 * Give an untyped numeric literal operand of a binary expression its type from
 * context: the surrounding expected type if there is one, otherwise the other
 * operand's type. Without this `u8_var + 32` would promote to the literal's
 * default i32.
 */
void coerceBinaryLiteralOperands(const BinaryExprAST& binExpr,
                                 const sun::TypePtr& expectedType);

/**
 * A char only compares with a char, and never takes part in arithmetic.
 * Without this `'a' + 1` and `c == 65` would quietly fall through to the
 * integer paths, since a char is an i32 underneath.
 */
void checkCharOperands(const BinaryExprAST& binExpr);

/**
 * The type an arithmetic/bitwise/shift binary expression produces: the wider
 * of the two operand types, mirroring codegen's operand unification.
 */
sun::TypePtr promoteBinaryOperands(const sun::TypePtr& lhsType,
                                   const sun::TypePtr& rhsType);

/**
 * Unify the branch types of a ternary expression: exact match, or the wider
 * type when one side widens to the other (never narrows f64 to f32). Throws a
 * compile error when the types are incompatible.
 */
sun::TypePtr unifyTernaryTypes(const sun::TypePtr& thenType,
                               const sun::TypePtr& elseType,
                               std::optional<Position> loc);

/**
 * A borrow binds the storage of an addressable lvalue: a variable, a field, or
 * an array element. Rejects everything else (temporaries, class `__index__`
 * results, slices), so `ref r = x` and `var r: ref T = x` agree.
 */
bool isBorrowableLvalue(const ExprAST& target);

/**
 * Does evaluating this statement guarantee the function exits — through a
 * return or a throw — rather than falling through to whatever comes next?
 * Conservative: anything unrecognized answers no. Two rules build on it: a
 * non-void body must end on a path where this answers yes (Sun has no
 * implicit returns), and a block whose body always exits produces no value
 * and cannot be bound.
 */
bool alwaysExits(const ExprAST& expr);

}  // namespace sun::rules
