/** Routes statements and control flow to body checking. */
#include "ast.h"
#include "semantic_analysis/body_analysis/body_analyzer.h"
#include "semantic_analysis/semantic_analyzer.h"

using sun::ast::ASTNodeType;
using sun::ast::BlockExprAST;
using sun::ast::ExprAST;
using sun::ast::MemberAssignmentAST;
using sun::types::TypePtr;
using sun::types::Types;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
bool BodyAnalyzer::tryAnalyzeStatement(ExprAST& expr, TypePtr expectedType) {
  switch (expr.getType()) {
    case ASTNodeType::VARIABLE_ASSIGNMENT:
      analyzeVariableAssignment(
          static_cast<sun::ast::VariableAssignmentAST&>(expr));
      break;

    case ASTNodeType::COMPOUND_ASSIGNMENT:
      analyzeCompoundAssignment(
          static_cast<sun::ast::CompoundAssignmentAST&>(expr));
      break;

    case ASTNodeType::BLOCK: {
      auto& block = static_cast<BlockExprAST&>(expr);
      ctx_.enterScope();
      analyzeBlock(block);
      expr.setResolvedType(sema_.expressions().preparedBlockType(block));
      ctx_.exitScope();
      break;
    }

    case ASTNodeType::IF:
      analyzeIfExpr(static_cast<sun::ast::IfExprAST&>(expr));
      break;

    case ASTNodeType::MATCH:
      analyzeMatchExpr(static_cast<sun::ast::MatchExprAST&>(expr),
                       expectedType);
      break;

    case ASTNodeType::FOR_LOOP:
      analyzeForLoop(static_cast<sun::ast::ForExprAST&>(expr));
      break;

    case ASTNodeType::FOR_IN_LOOP:
      analyzeForInLoop(static_cast<sun::ast::ForInExprAST&>(expr));
      break;

    case ASTNodeType::WHILE_LOOP: {
      auto& whileExpr = static_cast<sun::ast::WhileExprAST&>(expr);
      sema_.analyzeExpr(const_cast<ExprAST&>(*whileExpr.getCondition()));
      sema_.analyzeExpr(const_cast<ExprAST&>(*whileExpr.getBody()));
      expr.setResolvedType(Types::Float64());  // while loops return 0.0
      break;
    }

    case ASTNodeType::INDEXED_ASSIGNMENT:
      analyzeIndexedAssignment(
          static_cast<sun::ast::IndexedAssignmentAST&>(expr));
      break;

    case ASTNodeType::RETURN:
      analyzeReturnExpr(static_cast<sun::ast::ReturnExprAST&>(expr));
      break;

    case ASTNodeType::MEMBER_ASSIGNMENT:
      analyzeMemberAssignment(static_cast<MemberAssignmentAST&>(expr));
      break;

    case ASTNodeType::TRY_CATCH:
      analyzeTryCatch(static_cast<sun::ast::TryCatchExprAST&>(expr));
      break;

    case ASTNodeType::UNSAFE_BLOCK:
      analyzeUnsafeBlock(static_cast<sun::ast::UnsafeBlockAST&>(expr));
      break;

    case ASTNodeType::THROW:
      analyzeThrowExpr(static_cast<sun::ast::ThrowExprAST&>(expr));
      break;

    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
      // Neither yields a value, but a block that ends in one still needs a
      // prepared type when it sits in a match arm or another value position
      expr.setResolvedType(Types::Void());
      break;

    default:
      return false;
  }
  return true;
}
}  // namespace sun::semantic_analysis
