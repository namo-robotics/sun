/** Resolves lambda signatures and delegates body checking. */
#include <set>

#include "semantic_analysis/declaration_analysis/declaration_rules.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "support/error.h"

using sun::semantic_analysis::QualifiedName;
using sun::types::TypePtr;
using sun::types::Types;

using sun::ast::PrototypeAST;
using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::types::LambdaType;

void ExpressionAnalyzer::analyzeLambdaExpr(sun::ast::LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  rejectRefEnvReturnType(proto.getReturnType(), lambda.getLocation(),
                         /*allowNamed=*/true);

  // A lambda's own binders and any enclosing binders are both in scope for
  // its signature.
  sema_.declarations().checkSignatureLifetimes(proto, lambda.getLocation());

  // Get lambda signature info (pure computation)
  FunctionInfo lambdaInfo = sema_.declarations().getLambdaInfo(lambda);

  // Apply computed info to prototype
  sema_.declarations().applyFunctionInfoToProto(proto, lambdaInfo);

  // Analyze the lambda body
  sema_.bodies().analyzeLambda(lambda);

  // Set the lambda type on the lambda node
  auto lambdaType =
      Types::Lambda(lambdaInfo.returnType, lambdaInfo.paramTypes,
                    proto.hasReturnType() && proto.getReturnType()->canError);
  if (proto.hasRefCaptures() || !proto.getRefCaptureNames().empty() ||
      !proto.getOwnedCaptureNames().empty())
    static_cast<LambdaType&>(*lambdaType).setHasRefCaptures(true);
  lambda.setResolvedType(lambdaType);
}

}  // namespace sun::semantic_analysis
