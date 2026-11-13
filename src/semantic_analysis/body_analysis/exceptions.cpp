/** Checks the bounded exception payload and handler contracts. */
#include "ast.h"
#include "semantic_analysis/body_analysis/body_analyzer.h"
#include "semantic_analysis/semantic_analyzer.h"

/** Resolves exception control flow and local catch bindings. */
namespace sun::semantic_analysis {
using sun::support::logAndThrowError;
using sun::types::Types;

void BodyAnalyzer::analyzeThrow(sun::ast::ThrowAST& statement) {
  auto* function = ctx_.currentFunctionScope();
  if (!function)
    logAndThrowError("throw requires a function body", statement.getLocation());
  if (!function->functionCanThrow && !ctx_.currentScope().hasExceptionHandler())
    logAndThrowError("throw requires a try block or a throws declaration", statement.getLocation());
  if (statement.value) {
    sema_.analyzeExpr(*statement.value);
    auto type = statement.value->getResolvedType();
    if (!type || !type->isClass() ||
        !static_cast<sun::types::ClassType*>(type.get())->isException())
      logAndThrowError("Only a value declared with exception can be thrown", statement.getLocation());
  } else if (!ctx_.currentScope().hasExceptionHandler(true)) {
    logAndThrowError("A bare throw requires an active catch handler", statement.getLocation());
  }
  statement.setResolvedType(Types::Void());
}

void BodyAnalyzer::analyzeTryCatch(sun::ast::TryCatchAST& statement) {
  ctx_.enterScope();
  ctx_.currentScope().catchesExceptions = true;
  sema_.analyzeExpr(*statement.body);
  ctx_.exitScope();
  bool catchAll = false;
  std::vector<sun::types::TypePtr> preceding;
  for (auto& handler : statement.handlers) {
    if (catchAll)
      logAndThrowError("A catch-all handler must be last", handler.body->getLocation());
    ctx_.enterScope();
    ctx_.currentScope().activeExceptionHandler = true;
    if (handler.binding) {
      auto& binding = static_cast<sun::ast::VariableCreationAST&>(*handler.binding);
      auto type = sema_.typeResolver().typeAnnotationToType(*binding.getTypeAnnotation());
      if (!type->isClass() || !static_cast<sun::types::ClassType*>(type.get())->isException())
        logAndThrowError("A catch type must name an exception declaration; '" +
                             type->toDisplayString() + "' is not an exception",
                         binding.getLocation());
      for (const auto& previous : preceding) {
        for (auto ancestor = std::static_pointer_cast<sun::types::ClassType>(type);
             ancestor; ancestor = ancestor->getExceptionParent()) {
          if (ancestor->equals(*previous))
            logAndThrowError("An earlier catch already handles this exception family", binding.getLocation());
        }
      }
      preceding.push_back(type);
      binding.setResolvedType(type);
      ctx_.currentScope().declareVariable(binding.getName(), type, false, false,
                                          binding.getDeclarationId());
      ctx_.currentScope().lookupVariable(binding.getName())->isCatchBinding = true;
    } else {
      catchAll = true;
    }
    sema_.analyzeExpr(*handler.body);
    ctx_.exitScope();
  }
  statement.setResolvedType(Types::Void());
}
}  // namespace sun::semantic_analysis
