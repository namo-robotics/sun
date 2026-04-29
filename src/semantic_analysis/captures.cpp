// semantic_analysis/captures.cpp — Free variable collection and closure
// captures

#include <set>

#include "semantic_analyzer.h"

// -------------------------------------------------------------------
// Free variable collection
// -------------------------------------------------------------------

std::set<std::string> SemanticAnalyzer::collectFreeVariables(
    const ExprAST& expr, const std::set<std::string>& bound) {
  std::set<std::string> free;

  switch (expr.getType()) {
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
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

    case ASTNodeType::LAMBDA:
      // Lambdas define their own scope - handled separately
      break;

    case ASTNodeType::INDEXED_ASSIGNMENT: {
      const auto& assignment = static_cast<const IndexedAssignmentAST&>(expr);
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
      if (varInfo->scopeDepth == 0) {
        continue;  // Skip global variables - they don't need to be captured
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

  std::vector<Capture> captures;
  for (const auto& var : freeVars) {
    // Look up the variable's type
    VariableInfo* varInfo = lookupVariable(var);
    if (varInfo && varInfo->type) {
      if (varInfo->scopeDepth == 0) {
        continue;  // Skip global variables - they don't need to be captured
      }
      captures.push_back({var, varInfo->type});
    }
  }

  return captures;
}
