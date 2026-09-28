/** Coordinates dispatch to declaration, statement, and expression checking. */
#include "semantic_analysis/passes/declaration_naming_pass.h"
#include "semantic_analysis/semantic_analyzer.h"

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
void SemanticAnalyzer::analyzeExpr(sun::ast::ExprAST &expr,
                                   sun::types::TypePtr expectedType) {
  SemanticContext::SourceFileGuard sourceFile(ctx_, expr.getSourceFileId());
  SemanticContext::LocationGuard location(ctx_, expr.getLocation());
  passes::assignLocalDeclarationName(expr, ctx_.getCurrentScopePath());
  if (declarations_.tryAnalyzeDeclaration(expr)) return;
  if (bodies_.tryAnalyzeStatement(expr, expectedType)) return;
  expressions_.analyzeExpression(expr, expectedType);
}
}  // namespace sun::semantic_analysis
