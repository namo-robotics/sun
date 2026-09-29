/** Discovers and validates function and lambda captures. */
#include <set>

#include "ast/free_variables.h"
#include "semantic_analysis/expression_analysis/expression_properties.h"
#include "semantic_analysis/semantic_analyzer.h"

using sun::ast::ASTNodeType;
using sun::ast::CaptureKind;
using sun::ast::ExprAST;
using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

std::vector<sun::ast::Capture> DeclarationAnalyzer::buildCaptures(
    const sun::ast::LambdaAST& lambda) {
  const sun::ast::PrototypeAST& proto = lambda.getProto();

  // A lambda body compiles as its own function and cannot reach the
  // enclosing method's receiver, and `this` cannot be named in a capture
  // list: the receiver is a borrow of the whole object. To run a method on
  // a thread or a callback, bind it as a value (`this.poll`) instead.
  for (const auto& stmt : lambda.getBody().getBody()) {
    const ExprAST* thisUse = stmt ? findThisUse(*stmt) : nullptr;
    if (thisUse) {
      logAndThrowError(
          "A lambda cannot use 'this'. Read the fields it needs into local "
          "variables and capture those, pass them as arguments, or use a "
          "bound method ('this.method') as the callable instead",
          thisUse->getLocation());
    }
  }

  // Collect bound variables (lambda parameters)
  std::set<std::string> boundVars;
  for (const auto& arg : proto.getArgNames()) {
    boundVars.insert(arg);
  }

  std::set<std::string> freeVars =
      sun::ast::collectFreeVariablesInBlock(lambda.getBody(), boundVars);

  const auto& refNames = proto.getRefCaptureNames();
  auto isDeclaredRef = [&](const std::string& name) {
    return std::find(refNames.begin(), refNames.end(), name) != refNames.end();
  };
  const auto& ownedNames = proto.getOwnedCaptureNames();

  // Every name in the capture list must be a variable the body actually uses
  auto checkListedName = [&](const std::string& name,
                             bool byRef) -> VariableInfo* {
    if (!freeVars.count(name)) {
      logAndThrowError("Capture list names '" + name +
                           "' but the lambda body does not use it",
                       lambda.getLocation());
    }
    VariableInfo* varInfo = ctx_.currentScope().lookupVariable(name);
    if (!varInfo || !varInfo->type) {
      logAndThrowError("Unknown variable '" + name + "' in lambda capture list",
                       lambda.getLocation());
    }
    if (varInfo->isGlobal) {
      logAndThrowError(
          "Cannot capture global variable '" + name +
              (byRef ? "' by reference; globals are accessed directly"
                     : "'; globals are accessed directly"),
          lambda.getLocation());
    }
    // Capturing a name the enclosing lambda holds by value would reach into
    // that closure's own storage — as a borrow it would alias it, and as an
    // owned capture it would take it away.
    if (varInfo->captureKind && *varInfo->captureKind != CaptureKind::Borrow) {
      logAndThrowError("Cannot capture '" + name +
                           (byRef ? "' by reference" : "'") +
                           ": the enclosing lambda captures it by value",
                       lambda.getLocation());
    }
    return varInfo;
  };

  for (const auto& refName : refNames) {
    checkListedName(refName, /*byRef=*/true);
  }
  for (const auto& ownedName : ownedNames) {
    if (isDeclaredRef(ownedName)) {
      logAndThrowError("Capture list names '" + ownedName +
                           "' twice; a capture is either borrowed or owned",
                       lambda.getLocation());
    }
    checkListedName(ownedName, /*byRef=*/false);
  }

  std::vector<sun::ast::Capture> captures;
  for (const auto& var : freeVars) {
    // Look up the variable's type
    VariableInfo* varInfo = ctx_.currentScope().lookupVariable(var);
    if (varInfo && varInfo->type) {
      if (varInfo->isGlobal) {
        continue;  // Skip global variables - they don't need to be captured
      }
      CaptureKind kind = isDeclaredRef(var)          ? CaptureKind::Borrow
                         : proto.isOwnedCapture(var) ? CaptureKind::Owned
                                                     : CaptureKind::ByValue;
      // A compound value cannot be picked up implicitly — the env copy would
      // silently break aliasing. Naming it in the capture list says which of
      // the three things you meant.
      if (kind == CaptureKind::ByValue &&
          sun::types::unwrapRef(varInfo->type)->isCompound()) {
        logAndThrowError("Cannot capture '" + var + "' of compound type '" +
                             varInfo->type->toDisplayString() +
                             "' by value; capture it by reference with '[ref " +
                             var + "]() => ...', read it with '[const ref " +
                             var +
                             "]() => ...', or move it into the lambda with '[" +
                             var + "]() => ...'",
                         lambda.getLocation());
      }
      // An owned capture of a borrow would launder the borrow into a value
      if (kind == CaptureKind::Owned && varInfo->type->isReference()) {
        logAndThrowError(
            "Cannot move '" + var +
                "' into the lambda: it is a reference, so the value belongs to "
                "someone else. Capture it with '[ref " +
                var + "]() => ...' or '[const ref " + var + "]() => ...'",
            lambda.getLocation());
      }
      // A constant stays constant inside the lambda, however it is captured;
      // `[const ref x]` makes an otherwise mutable variable read-only there.
      // An owned capture is the closure's own value, so it is mutable there
      // even when the variable it came from was constant.
      bool isConst =
          kind != CaptureKind::Owned &&
          ((kind == CaptureKind::Borrow && proto.isConstRefCapture(var)) ||
           varInfo->isConst || sun::types::isConstRef(varInfo->type));
      captures.push_back(
          {var, varInfo->type, kind, isConst, varInfo->declarationId});
    }
  }

  return captures;
}

}  // namespace sun::semantic_analysis
