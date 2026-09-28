/** Checks conditional value expressions and their result types. */
#include "ast/control_flow.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/type_analysis/type_rules.h"
#include "support/error.h"

using sun::types::ClassType;
using sun::types::TypePtr;
using sun::types::Types;

using sun::ast::ExprAST;
using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

using sun::semantic_analysis::type_analysis::tryCoerceIntegerLiteral;
using sun::semantic_analysis::type_analysis::unifyTernaryTypes;
using sun::types::unwrapRef;

void ExpressionAnalyzer::analyzeTernaryExpr(sun::ast::TernaryExprAST& ternary,
                                            TypePtr expectedType) {
  // Condition is not required to be bool (matches if/while laxness);
  // codegen coerces numeric conditions to i1.
  sema_.analyzeExpr(*ternary.getCond());
  sema_.analyzeExpr(*ternary.getThen(), expectedType);
  sema_.analyzeExpr(*ternary.getElse(), expectedType);

  TypePtr thenType =
      sun::types::unwrapRef(ternary.getThen()->getResolvedType());
  TypePtr elseType =
      sun::types::unwrapRef(ternary.getElse()->getResolvedType());

  // Integer literals adopt the other branch's type: c ? x : 0
  if (thenType && elseType && !thenType->equals(*elseType)) {
    if (tryCoerceIntegerLiteral(ternary.getThen(), elseType, false)) {
      thenType = elseType;
    } else if (tryCoerceIntegerLiteral(ternary.getElse(), thenType, false)) {
      elseType = thenType;
    }
  }

  ternary.setResolvedType(
      unifyTernaryTypes(thenType, elseType, ternary.getLocation()));
}

}  // namespace sun::semantic_analysis
