#include "semantic_analysis/declaration_naming_pass.h"

#include "ast.h"
#include "ast/ast_children.h"

using sun::ast::ASTNodeType;
using sun::ast::ExprAST;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
/** Keeps the implementation helpers in this file private to this translation unit. */
namespace {

/** Fill a declaration's name while retaining imported or generated identities.
 */
template <typename Declaration>
const QualifiedName& nameDeclaration(
    Declaration& declaration, const std::vector<std::string>& scopePath) {
  if (!declaration.hasQualifiedName()) {
    declaration.setQualifiedName(
        QualifiedName(scopePath, declaration.getName()));
  }
  return declaration.getQualifiedName();
}

/** Share name assignment while each pass supplies its own scope boundary. */
void assignNames(ExprAST& root, const std::vector<std::string>& scopePath,
                 bool moduleLevel) {
  const auto visitChildren = [&](const std::vector<std::string>& childScope,
                                 bool childModuleLevel) {
    sun::ast::forEachChild(root, [&](const ExprAST& child) {
      assignNames(const_cast<ExprAST&>(child), childScope, childModuleLevel);
    });
  };
  const QualifiedName* namedScope = nullptr;
  switch (root.getType()) {
    case ASTNodeType::MODULE:
      namedScope =
          &nameDeclaration(static_cast<sun::ast::ModuleAST&>(root), scopePath);
      moduleLevel = true;
      break;
    case ASTNodeType::MOON_SCOPE: {
      const auto& moon = static_cast<sun::ast::MoonScopeAST&>(root);
      auto childScope = scopePath;
      if (!moon.getContentHash().empty())
        childScope.push_back(moon.getContentHash());
      visitChildren(childScope, moduleLevel);
      return;
    }
    case ASTNodeType::CLASS_DEFINITION:
      namedScope = &nameDeclaration(
          static_cast<sun::ast::ClassDefinitionAST&>(root), scopePath);
      moduleLevel = false;
      break;
    case ASTNodeType::INTERFACE_DEFINITION:
      namedScope = &nameDeclaration(
          static_cast<sun::ast::InterfaceDefinitionAST&>(root), scopePath);
      moduleLevel = false;
      break;
    case ASTNodeType::ENUM_DEFINITION:
      nameDeclaration(static_cast<sun::ast::EnumDefinitionAST&>(root),
                      scopePath);
      return;
    case ASTNodeType::FUNCTION:
      nameDeclaration(static_cast<sun::ast::FunctionAST&>(root).getProtoMut(),
                      scopePath);
      return;
    case ASTNodeType::LAMBDA:
      return;
    case ASTNodeType::VARIABLE_CREATION:
      if (moduleLevel)
        nameDeclaration(static_cast<sun::ast::VariableCreationAST&>(root),
                        scopePath);
      break;
    case ASTNodeType::REFERENCE_CREATION:
      if (moduleLevel)
        nameDeclaration(static_cast<sun::ast::ReferenceCreationAST&>(root),
                        scopePath);
      break;
    default:
      break;
  }
  if (namedScope) {
    auto childScope = namedScope->scopePath;
    childScope.push_back(namedScope->baseName);
    visitChildren(childScope, moduleLevel);
  } else {
    visitChildren(scopePath, moduleLevel);
  }
}

}  // namespace

void DeclarationNamingPass::run(ExprAST& root,
                                const std::vector<std::string>& scopePath,
                                bool moduleLevel) const {
  assignNames(root, scopePath, moduleLevel);
}

void assignLocalDeclarationName(ExprAST& declaration,
                                const std::vector<std::string>& scopePath) {
  const auto nameType = [&](auto& type) {
    const auto& name = nameDeclaration(type, scopePath);
    auto methodScope = name.scopePath;
    methodScope.push_back(name.baseName);
    for (const auto& method : type.getMethods())
      nameDeclaration(method.function->getProtoMut(), methodScope);
  };
  switch (declaration.getType()) {
    case ASTNodeType::CLASS_DEFINITION:
      nameType(static_cast<sun::ast::ClassDefinitionAST&>(declaration));
      break;
    case ASTNodeType::INTERFACE_DEFINITION:
      nameType(static_cast<sun::ast::InterfaceDefinitionAST&>(declaration));
      break;
    case ASTNodeType::ENUM_DEFINITION:
      nameDeclaration(static_cast<sun::ast::EnumDefinitionAST&>(declaration),
                      scopePath);
      break;
    case ASTNodeType::FUNCTION:
      nameDeclaration(
          static_cast<sun::ast::FunctionAST&>(declaration).getProtoMut(),
          scopePath);
      break;
    default:
      break;
  }
}

}  // namespace sun::semantic_analysis
