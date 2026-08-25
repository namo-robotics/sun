// semantic_analysis/captures.cpp — Free variable collection and closure
// captures

#include <set>

#include "semantic_analysis/semantic_analyzer.h"

// -------------------------------------------------------------------
// Free variable collection
// -------------------------------------------------------------------

std::set<std::string> SemanticAnalyzer::collectFreeVariables(
    const ExprAST& expr, const std::set<std::string>& bound) {
  std::set<std::string> free;

  switch (expr.getType()) {
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
    case ASTNodeType::CHAR_LITERAL:
    case ASTNodeType::NULL_LITERAL:
    case ASTNodeType::BOOL_LITERAL:
      break;

    case ASTNodeType::VARIABLE_REFERENCE: {
      const auto& varRef = static_cast<const VariableReferenceAST&>(expr);
      if (bound.find(varRef.getName()) == bound.end()) {
        free.insert(varRef.getName());
      }
      break;
    }

    case ASTNodeType::VARIABLE_CREATION: {
      const auto& varCreate = static_cast<const VariableCreationAST&>(expr);
      auto valueFree = collectFreeVariables(*varCreate.getValue(), bound);
      free.insert(valueFree.begin(), valueFree.end());
      break;
    }

    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      const auto& varAssign = static_cast<const VariableAssignmentAST&>(expr);
      // The variable name might be free if not bound
      if (bound.find(varAssign.getName()) == bound.end()) {
        free.insert(varAssign.getName());
      }
      auto valueFree = collectFreeVariables(*varAssign.getValue(), bound);
      free.insert(valueFree.begin(), valueFree.end());
      break;
    }

    case ASTNodeType::BINARY: {
      const auto& binExpr = static_cast<const BinaryExprAST&>(expr);
      auto lhsFree = collectFreeVariables(*binExpr.getLHS(), bound);
      auto rhsFree = collectFreeVariables(*binExpr.getRHS(), bound);
      free.insert(lhsFree.begin(), lhsFree.end());
      free.insert(rhsFree.begin(), rhsFree.end());
      break;
    }

    case ASTNodeType::UNARY: {
      const auto& unaryExpr = static_cast<const UnaryExprAST&>(expr);
      auto operandFree = collectFreeVariables(*unaryExpr.getOperand(), bound);
      free.insert(operandFree.begin(), operandFree.end());
      break;
    }

    case ASTNodeType::CALL: {
      const auto& callExpr = static_cast<const CallExprAST&>(expr);
      // Collect free variables from the callee expression
      auto calleeFree = collectFreeVariables(*callExpr.getCallee(), bound);
      free.insert(calleeFree.begin(), calleeFree.end());
      // Collect free variables from arguments
      for (const auto& arg : callExpr.getArgs()) {
        auto argFree = collectFreeVariables(*arg, bound);
        free.insert(argFree.begin(), argFree.end());
      }
      break;
    }

    case ASTNodeType::IF: {
      const auto& ifExpr = static_cast<const IfExprAST&>(expr);
      auto condFree = collectFreeVariables(*ifExpr.getCond(), bound);
      auto thenFree = collectFreeVariables(*ifExpr.getThen(), bound);
      free.insert(condFree.begin(), condFree.end());
      free.insert(thenFree.begin(), thenFree.end());
      if (ifExpr.getElse()) {
        auto elseFree = collectFreeVariables(*ifExpr.getElse(), bound);
        free.insert(elseFree.begin(), elseFree.end());
      }
      break;
    }

    case ASTNodeType::TERNARY: {
      const auto& ternary = static_cast<const TernaryExprAST&>(expr);
      auto condFree = collectFreeVariables(*ternary.getCond(), bound);
      auto thenFree = collectFreeVariables(*ternary.getThen(), bound);
      auto elseFree = collectFreeVariables(*ternary.getElse(), bound);
      free.insert(condFree.begin(), condFree.end());
      free.insert(thenFree.begin(), thenFree.end());
      free.insert(elseFree.begin(), elseFree.end());
      break;
    }

    case ASTNodeType::MATCH: {
      const auto& matchExpr = static_cast<const MatchExprAST&>(expr);
      auto discFree = collectFreeVariables(*matchExpr.getDiscriminant(), bound);
      free.insert(discFree.begin(), discFree.end());
      for (const auto& arm : matchExpr.getArms()) {
        if (arm.pattern) {
          auto patternFree = collectFreeVariables(*arm.pattern, bound);
          free.insert(patternFree.begin(), patternFree.end());
        }
        auto bodyFree = collectFreeVariables(*arm.body, bound);
        free.insert(bodyFree.begin(), bodyFree.end());
      }
      break;
    }

    case ASTNodeType::FOR_LOOP: {
      const auto& forExpr = static_cast<const ForExprAST&>(expr);
      if (forExpr.getInit()) {
        auto initFree = collectFreeVariables(*forExpr.getInit(), bound);
        free.insert(initFree.begin(), initFree.end());
      }
      if (forExpr.getCondition()) {
        auto condFree = collectFreeVariables(*forExpr.getCondition(), bound);
        free.insert(condFree.begin(), condFree.end());
      }
      if (forExpr.getIncrement()) {
        auto incrFree = collectFreeVariables(*forExpr.getIncrement(), bound);
        free.insert(incrFree.begin(), incrFree.end());
      }
      auto bodyFree = collectFreeVariables(*forExpr.getBody(), bound);
      free.insert(bodyFree.begin(), bodyFree.end());
      break;
    }

    case ASTNodeType::FOR_IN_LOOP: {
      const auto& forInExpr = static_cast<const ForInExprAST&>(expr);
      // Iterable expression can have free variables
      auto iterableFree = collectFreeVariables(*forInExpr.getIterable(), bound);
      free.insert(iterableFree.begin(), iterableFree.end());
      // Loop variable is bound in the body
      std::set<std::string> bodyBound = bound;
      bodyBound.insert(forInExpr.getLoopVar());
      auto bodyFree = collectFreeVariables(*forInExpr.getBody(), bodyBound);
      free.insert(bodyFree.begin(), bodyFree.end());
      break;
    }

    case ASTNodeType::WHILE_LOOP: {
      const auto& whileExpr = static_cast<const WhileExprAST&>(expr);
      auto condFree = collectFreeVariables(*whileExpr.getCondition(), bound);
      auto bodyFree = collectFreeVariables(*whileExpr.getBody(), bound);
      free.insert(condFree.begin(), condFree.end());
      free.insert(bodyFree.begin(), bodyFree.end());
      break;
    }

    case ASTNodeType::BLOCK: {
      const auto& block = static_cast<const BlockExprAST&>(expr);
      auto blockFree = collectFreeVariablesInBlock(block, bound);
      free.insert(blockFree.begin(), blockFree.end());
      break;
    }

    case ASTNodeType::FUNCTION:
      // Functions define their own scope - handled separately
      break;

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

    case ASTNodeType::SPAWN: {
      // spawn(lambda) - analyze the lambda for captures
      const auto& spawnExpr = static_cast<const SpawnExprAST&>(expr);
      auto lambdaFree = collectFreeVariables(spawnExpr.getLambda(), bound);
      free.insert(lambdaFree.begin(), lambdaFree.end());
      break;
    }

    case ASTNodeType::MEMBER_ACCESS: {
      const auto& access = static_cast<const MemberAccessAST&>(expr);
      auto objectFree = collectFreeVariables(*access.getObject(), bound);
      free.insert(objectFree.begin(), objectFree.end());
      break;
    }

    case ASTNodeType::MEMBER_ASSIGNMENT: {
      const auto& assignment = static_cast<const MemberAssignmentAST&>(expr);
      auto objectFree = collectFreeVariables(*assignment.getObject(), bound);
      free.insert(objectFree.begin(), objectFree.end());
      auto valueFree = collectFreeVariables(*assignment.getValue(), bound);
      free.insert(valueFree.begin(), valueFree.end());
      break;
    }

    case ASTNodeType::INDEX: {
      const auto& indexExpr = static_cast<const IndexAST&>(expr);
      auto targetFree = collectFreeVariables(*indexExpr.getTarget(), bound);
      free.insert(targetFree.begin(), targetFree.end());
      for (const auto& slice : indexExpr.getIndices()) {
        if (slice->getStart()) {
          auto f = collectFreeVariables(*slice->getStart(), bound);
          free.insert(f.begin(), f.end());
        }
        if (slice->getEnd()) {
          auto f = collectFreeVariables(*slice->getEnd(), bound);
          free.insert(f.begin(), f.end());
        }
      }
      break;
    }

    case ASTNodeType::INDEXED_ASSIGNMENT: {
      const auto& assignment = static_cast<const IndexedAssignmentAST&>(expr);
      auto targetFree = collectFreeVariables(*assignment.getTarget(), bound);
      free.insert(targetFree.begin(), targetFree.end());
      auto valueFree = collectFreeVariables(*assignment.getValue(), bound);
      free.insert(valueFree.begin(), valueFree.end());
      break;
    }

    case ASTNodeType::COMPOUND_ASSIGNMENT: {
      const auto& assignment = static_cast<const CompoundAssignmentAST&>(expr);
      auto targetFree = collectFreeVariables(*assignment.getTarget(), bound);
      free.insert(targetFree.begin(), targetFree.end());
      auto valueFree = collectFreeVariables(*assignment.getValue(), bound);
      free.insert(valueFree.begin(), valueFree.end());
      break;
    }

    case ASTNodeType::RETURN: {
      const auto& returnExpr = static_cast<const ReturnExprAST&>(expr);
      if (returnExpr.hasValue()) {
        auto valueFree = collectFreeVariables(*returnExpr.getValue(), bound);
        free.insert(valueFree.begin(), valueFree.end());
      }
      break;
    }

    default:
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
    VariableInfo* varInfo = lookupVariable(var);
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

std::vector<Capture> SemanticAnalyzer::buildCaptures(const LambdaAST& lambda) {
  const PrototypeAST& proto = lambda.getProto();

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
    VariableInfo* varInfo = lookupVariable(name);
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
    if (varInfo->isCapture && !varInfo->isByRefCapture) {
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
    VariableInfo* varInfo = lookupVariable(var);
    if (varInfo && varInfo->type) {
      if (varInfo->isGlobal) {
        continue;  // Skip global variables - they don't need to be captured
      }
      bool byRef = isDeclaredRef(var);
      bool owned = !byRef && proto.isOwnedCapture(var);
      bool declaredConstRef = byRef && proto.isConstRefCapture(var);
      // A compound value cannot be picked up implicitly — the env copy would
      // silently break aliasing. Naming it in the capture list says which of
      // the three things you meant.
      if (!byRef && !owned && sun::unwrapRef(varInfo->type)->isCompound()) {
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
      if (owned && varInfo->type->isReference()) {
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
      bool isConst = !owned && (declaredConstRef || varInfo->isConst ||
                                sun::isConstRef(varInfo->type));
      captures.push_back({var, varInfo->type, byRef, isConst, owned});
    }
  }

  return captures;
}
