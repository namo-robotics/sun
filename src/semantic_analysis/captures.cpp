// semantic_analysis/captures.cpp — Free variable collection and closure
// captures

#include <set>

#include "ast/ast_children.h"
#include "semantic_analysis/semantic_analyzer.h"

// -------------------------------------------------------------------
// Free variable collection
// -------------------------------------------------------------------

// Names the expression uses that nothing inside it declares.
//
// Most nodes are just a shape to walk through, so `forEachChild` — the
// enumerator that already knows every node's children — supplies the default.
// Only the handful of nodes that mention a name, or that declare one their
// children can see, need their own case here. Anything left out of both is
// silently treated as capturing nothing, which is how a lambda ends up
// looking for a local among the module's globals.
std::set<std::string> SemanticAnalyzer::collectFreeVariables(
    const ExprAST& expr, const std::set<std::string>& bound) {
  std::set<std::string> free;

  auto collectFrom = [&](const ExprAST& child,
                         const std::set<std::string>& childBound) {
    auto childFree = collectFreeVariables(child, childBound);
    free.insert(childFree.begin(), childFree.end());
  };

  switch (expr.getType()) {
    case ASTNodeType::VARIABLE_REFERENCE: {
      const auto& varRef = static_cast<const VariableReferenceAST&>(expr);
      if (!bound.count(varRef.getName())) {
        free.insert(varRef.getName());
      }
      break;
    }

    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      const auto& varAssign = static_cast<const VariableAssignmentAST&>(expr);
      // Writing to a name uses it just as reading does
      if (!bound.count(varAssign.getName())) {
        free.insert(varAssign.getName());
      }
      collectFrom(*varAssign.getValue(), bound);
      break;
    }

    case ASTNodeType::BLOCK: {
      const auto& block = static_cast<const BlockExprAST&>(expr);
      auto blockFree = collectFreeVariablesInBlock(block, bound);
      free.insert(blockFree.begin(), blockFree.end());
      break;
    }

    case ASTNodeType::FOR_LOOP: {
      const auto& forExpr = static_cast<const ForExprAST&>(expr);
      std::set<std::string> innerBound = bound;
      if (forExpr.getInit()) {
        collectFrom(*forExpr.getInit(), bound);
        // A loop counter declared in the header is visible to the rest of it
        if (forExpr.getInit()->getType() == ASTNodeType::VARIABLE_CREATION) {
          innerBound.insert(
              static_cast<const VariableCreationAST&>(*forExpr.getInit())
                  .getName());
        }
      }
      if (forExpr.getCondition())
        collectFrom(*forExpr.getCondition(), innerBound);
      if (forExpr.getIncrement())
        collectFrom(*forExpr.getIncrement(), innerBound);
      collectFrom(*forExpr.getBody(), innerBound);
      break;
    }

    case ASTNodeType::FOR_IN_LOOP: {
      const auto& forInExpr = static_cast<const ForInExprAST&>(expr);
      collectFrom(*forInExpr.getIterable(), bound);
      // The loop variable is declared by the loop, not captured from outside
      std::set<std::string> bodyBound = bound;
      bodyBound.insert(forInExpr.getLoopVar());
      collectFrom(*forInExpr.getBody(), bodyBound);
      break;
    }

    case ASTNodeType::TRY_CATCH: {
      const auto& tryCatch = static_cast<const TryCatchExprAST&>(expr);
      collectFrom(tryCatch.getTryBlock(), bound);
      for (const auto& clause : tryCatch.getCatchClauses()) {
        // The caught error is declared by the clause
        std::set<std::string> clauseBound = bound;
        clauseBound.insert(clause.bindingName);
        collectFrom(*clause.body, clauseBound);
      }
      break;
    }

    case ASTNodeType::LAMBDA: {
      // A nested lambda's free variables (minus its own params) are free in
      // the enclosing scope too: the enclosing closure must capture them so
      // the inner closure can initialize its env from the enclosing one
      const auto& lambda = static_cast<const LambdaAST&>(expr);
      std::set<std::string> innerBound = bound;
      for (const auto& arg : lambda.getProto().getArgNames()) {
        innerBound.insert(arg);
      }
      auto innerFree =
          collectFreeVariablesInBlock(lambda.getBody(), innerBound);
      free.insert(innerFree.begin(), innerFree.end());
      break;
    }

    // Definitions carry their own scope and cannot reach an enclosing local
    case ASTNodeType::FUNCTION:
    case ASTNodeType::CLASS_DEFINITION:
    case ASTNodeType::INTERFACE_DEFINITION:
    case ASTNodeType::ENUM_DEFINITION:
    case ASTNodeType::MODULE:
    case ASTNodeType::MOON_SCOPE:
    case ASTNodeType::IMPORT_SCOPE:
      break;

    default:
      forEachChild(expr,
                   [&](const ExprAST& child) { collectFrom(child, bound); });
      break;
  }

  return free;
}

std::set<std::string> SemanticAnalyzer::collectFreeVariablesInBlock(
    const BlockExprAST& block, std::set<std::string> bound) {
  std::set<std::string> free;

  for (const auto& expr : block.getBody()) {
    // Skip nested functions for now - they handle their own captures
    if (expr->isFunction()) {
      continue;
    }

    auto exprFree = collectFreeVariables(*expr, bound);
    free.insert(exprFree.begin(), exprFree.end());

    // Variable creation adds to bound set for subsequent expressions
    if (expr->getType() == ASTNodeType::VARIABLE_CREATION) {
      const auto& varCreate = static_cast<const VariableCreationAST&>(*expr);
      bound.insert(varCreate.getName());
    }
  }

  return free;
}

// -------------------------------------------------------------------
// Build captures for a function
// -------------------------------------------------------------------

std::vector<Capture> SemanticAnalyzer::buildCaptures(const FunctionAST& func) {
  // Extern functions have no body, so no captures
  if (func.isExtern()) {
    return {};
  }

  const PrototypeAST& proto = func.getProto();

  // Collect bound variables (function parameters)
  std::set<std::string> boundVars;
  for (const auto& arg : proto.getArgNames()) {
    boundVars.insert(arg);
  }

  std::set<std::string> freeVars =
      collectFreeVariablesInBlock(func.getBody(), boundVars);

  std::vector<Capture> captures;
  for (const auto& var : freeVars) {
    // Look up the variable's type
    VariableInfo* varInfo = ctx_.lookupVariable(var);
    if (varInfo && varInfo->type) {
      if (varInfo->isGlobal) {
        continue;  // Skip global variables - they don't need to be captured
      }
      // Compound types are copied into the env struct - a broken aliasing
      // model. Nested named functions have no capture list, so they cannot
      // capture compound types at all.
      if (sun::unwrapRef(varInfo->type)->isCompound()) {
        logAndThrowError("Cannot capture '" + var + "' of compound type '" +
                             varInfo->type->toDisplayString() +
                             "' by value in a nested function; use a lambda "
                             "with a [ref " +
                             var + "] capture list instead",
                         func.getLocation());
      }
      captures.push_back({var, varInfo->type});
    }
  }

  return captures;
}

// The first `this` inside the expression, if any. `this` is its own node
// type, so free-variable collection never sees it as a name.
static const ExprAST* findThisUse(const ExprAST& expr) {
  if (expr.getType() == ASTNodeType::THIS) return &expr;
  const ExprAST* found = nullptr;
  forEachChild(expr, [&found](const ExprAST& child) {
    if (!found) found = findThisUse(child);
  });
  return found;
}

std::vector<Capture> SemanticAnalyzer::buildCaptures(const LambdaAST& lambda) {
  const PrototypeAST& proto = lambda.getProto();

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
      collectFreeVariablesInBlock(lambda.getBody(), boundVars);

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
    VariableInfo* varInfo = ctx_.lookupVariable(name);
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

  std::vector<Capture> captures;
  for (const auto& var : freeVars) {
    // Look up the variable's type
    VariableInfo* varInfo = ctx_.lookupVariable(var);
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
          sun::unwrapRef(varInfo->type)->isCompound()) {
        logAndThrowError("Cannot capture '" + var + "' of compound type '" +
                             varInfo->type->toDisplayString() +
                             "' by value; capture it by reference with "
                             "'lambda [ref " +
                             var + "]', read it with 'lambda [const ref " +
                             var +
                             "]', or move it into the lambda with "
                             "'lambda [" +
                             var + "]'",
                         lambda.getLocation());
      }
      // An owned capture of a borrow would launder the borrow into a value
      if (kind == CaptureKind::Owned && varInfo->type->isReference()) {
        logAndThrowError(
            "Cannot move '" + var +
                "' into the lambda: it is a reference, so the value belongs to "
                "someone else. Capture it with 'lambda [ref " +
                var + "]' or 'lambda [const ref " + var + "]'",
            lambda.getLocation());
      }
      // A constant stays constant inside the lambda, however it is captured;
      // `[const ref x]` makes an otherwise mutable variable read-only there.
      // An owned capture is the closure's own value, so it is mutable there
      // even when the variable it came from was constant.
      bool isConst =
          kind != CaptureKind::Owned &&
          ((kind == CaptureKind::Borrow && proto.isConstRefCapture(var)) ||
           varInfo->isConst || sun::isConstRef(varInfo->type));
      captures.push_back({var, varInfo->type, kind, isConst});
    }
  }

  return captures;
}
