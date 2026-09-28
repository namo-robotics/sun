/** Routes value expressions to checking and resolution helpers. */
#include "ast.h"
#include "semantic_analysis/expression_analysis/expression_analyzer.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/type_analysis/type_rules.h"
#include "support/error.h"

using sun::ast::ASTNodeType;
using sun::ast::ExprAST;
using sun::ast::IndexAST;
using sun::ast::MemberAccessAST;
using sun::ast::TernaryExprAST;
using sun::semantic_analysis::type_analysis::isAssignableTo;
using sun::support::logAndThrowError;
using sun::types::TypePtr;
using sun::types::Types;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
void ExpressionAnalyzer::analyzeExpression(ExprAST& expr,
                                           TypePtr expectedType) {
  switch (expr.getType()) {
    case ASTNodeType::NUMBER:
      analyzeNumberLiteral(expr, expectedType);
      break;

    case ASTNodeType::CHAR_LITERAL: {
      // 'a' is always a char and b'a' is always a u8; neither takes its type
      // from context the way an integer literal does.
      expr.setResolvedType(
          static_cast<const sun::ast::CharLiteralAST&>(expr).isByte()
              ? Types::UInt8()
              : Types::Char());
      break;
    }

    case ASTNodeType::STRING_LITERAL: {
      expr.setResolvedType(Types::String());
      break;
    }

    case ASTNodeType::BOOL_LITERAL: {
      expr.setResolvedType(Types::Bool());
      break;
    }

    case ASTNodeType::NULL_LITERAL: {
      expr.setResolvedType(Types::NullPointer());
      break;
    }

    case ASTNodeType::STRUCT_LITERAL: {
      analyzeStructLiteral(static_cast<sun::ast::StructLiteralAST&>(expr),
                           expectedType);
      break;
    }

    case ASTNodeType::ARRAY_LITERAL:
      analyzeArrayLiteral(static_cast<sun::ast::ArrayLiteralAST&>(expr),
                          expectedType);
      break;

    case ASTNodeType::INDEX:
      analyzeIndexExpr(static_cast<IndexAST&>(expr));
      break;

    case ASTNodeType::SLICE:
      analyzeSliceExpr(expr);
      break;

    case ASTNodeType::VARIABLE_REFERENCE: {
      if (expr.getModuleDeclaration()) {
        expr.setResolvedType(resolveModuleReference(expr));
        break;
      }
      auto& varRef = static_cast<sun::ast::VariableReferenceAST&>(expr);
      sema_.declarations().ensureGlobalAnalyzed(varRef.getName());

      // An expected function-pointer type selects one overload without
      // changing ordinary call-site overload resolution.
      if (expectedType && expectedType->isFunction() &&
          !ctx_.currentScope().lookupVariable(varRef.getName())) {
        sun::semantic_analysis::QualifiedName resolved =
            ctx_.resolveNameWithUsings(varRef.getName());
        std::vector<FunctionInfo> matches;
        for (const auto& candidate : ctx_.getAllFunctions(resolved.baseName)) {
          auto candidateType = Types::Function(
              candidate.returnType, candidate.paramTypes, candidate.canThrow);
          if (isAssignableTo(candidateType, expectedType)) {
            matches.push_back(candidate);
          }
        }
        if (matches.size() == 1) {
          const FunctionInfo& match = matches.front();
          expr.setResolvedType(Types::Function(
              match.returnType, match.paramTypes, match.canThrow));
          varRef.setQualifiedName(match.qualifiedName);
          varRef.setTargetDeclarationId(match.declarationId);
          break;
        }
        if (!ctx_.getAllFunctions(resolved.baseName).empty()) {
          logAndThrowError("No overload of '" + varRef.getName() +
                               "' matches expected type '" +
                               expectedType->toDisplayString() + "'",
                           varRef.getLocation());
        }
      }

      expr.setResolvedType(resolveVariableReferenceType(varRef));
      sun::semantic_analysis::QualifiedName resolved =
          ctx_.resolveNameWithUsings(varRef.getName());
      varRef.setQualifiedName(resolved);
      if (VariableInfo* info =
              ctx_.currentScope().lookupVariable(varRef.getName())) {
        varRef.setTargetDeclarationId(info->declarationId);
        checkExternVariableAccessAllowed(*info, resolved.display(),
                                         varRef.getLocation());
      }
      break;
    }

    case ASTNodeType::LAMBDA:
      analyzeLambdaExpr(static_cast<sun::ast::LambdaAST&>(expr));
      break;

    case ASTNodeType::TERNARY:
      analyzeTernaryExpr(static_cast<TernaryExprAST&>(expr), expectedType);
      break;

    case ASTNodeType::BINARY:
      analyzeBinaryExpr(static_cast<sun::ast::BinaryExprAST&>(expr),
                        expectedType);
      break;

    case ASTNodeType::UNARY:
      analyzeUnaryExpr(static_cast<sun::ast::UnaryExprAST&>(expr));
      break;

    case ASTNodeType::CALL: {
      auto& callExpr = static_cast<sun::ast::CallExprAST&>(expr);
      sema_.calls().analyzeCall(callExpr, expectedType);
      break;
    }

    case ASTNodeType::QUALIFIED_NAME:
      if (expr.getModuleDeclaration())
        expr.setResolvedType(resolveModuleReference(expr));
      else
        analyzeQualifiedName(static_cast<sun::ast::QualifiedNameAST&>(expr));
      break;

    case ASTNodeType::THIS: {
      expr.setResolvedType(ctx_.getCurrentClass() ? ctx_.getCurrentClass()
                                                  : Types::Void());
      break;
    }

    case ASTNodeType::MEMBER_ACCESS:
      if (expr.getModuleDeclaration())
        expr.setResolvedType(resolveModuleReference(expr));
      else
        analyzeMemberAccess(static_cast<MemberAccessAST&>(expr), expectedType);
      break;

    case ASTNodeType::GENERIC_CALL:
      sema_.calls().analyzeGenericCall(
          static_cast<sun::ast::GenericCallAST&>(expr));
      break;

    case ASTNodeType::PACK_EXPANSION: {
      // Pack expansion is handled at codegen time
      // Just set the resolved type for now
      expr.setResolvedType(Types::Void());
      break;
    }

    default:
      break;
  }
}
}  // namespace sun::semantic_analysis
