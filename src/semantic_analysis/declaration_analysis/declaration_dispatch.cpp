/** Routes declaration syntax to its specialized analyzer. */
#include "ast.h"
#include "semantic_analysis/declaration_analysis/declaration_analyzer.h"
#include "semantic_analysis/semantic_analyzer.h"

using sun::ast::ASTNodeType;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::types::TypePtr;
using sun::types::Types;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
bool DeclarationAnalyzer::tryAnalyzeDeclaration(ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::VARIABLE_CREATION:
      analyzeVariableCreation(
          static_cast<sun::ast::VariableCreationAST&>(expr));
      break;

    case ASTNodeType::REFERENCE_CREATION:
      analyzeReferenceCreation(
          static_cast<sun::ast::ReferenceCreationAST&>(expr));
      break;

    case ASTNodeType::FUNCTION:
      analyzeFunctionDefinition(static_cast<FunctionAST&>(expr));
      break;

    case ASTNodeType::MODULE:
      analyzeModuleDefinition(static_cast<sun::ast::ModuleAST&>(expr));
      break;

    case ASTNodeType::MOON_SCOPE:
      analyzeMoonScope(expr);
      break;

    case ASTNodeType::USING: {
      sema_.pipeline().declarations().registerUsing(
          static_cast<sun::ast::UsingAST&>(expr));
      expr.setResolvedType(Types::Void());
      break;
    }

    case ASTNodeType::CLASS_DEFINITION:
      sema_.classes().analyzeClassDefinition(
          static_cast<sun::ast::ClassDefinitionAST&>(expr));
      break;

    case ASTNodeType::INTERFACE_DEFINITION:
      sema_.interfaces().analyzeInterfaceDefinition(
          static_cast<sun::ast::InterfaceDefinitionAST&>(expr));
      break;

    case ASTNodeType::ENUM_DEFINITION: {
      sema_.enums().analyzeEnumDefinition(
          static_cast<sun::ast::EnumDefinitionAST&>(expr));
      break;
    }

    case ASTNodeType::DECLARE_TYPE:
      analyzeDeclareType(static_cast<sun::ast::DeclareTypeAST&>(expr));
      break;

    default:
      return false;
  }
  return true;
}
}  // namespace sun::semantic_analysis
