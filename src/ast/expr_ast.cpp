// expr_ast.cpp — Implementation of ExprAST non-inline methods

#include "ast/expr_ast.h"

#include "serialization/ast_deserializer.h"
#include "serialization/ast_serializer.h"

std::unique_ptr<ExprAST> ExprAST::clone() const {
  // Serialize this node to protobuf
  sun::serialization::ASTSerializer serializer;
  std::string data = serializer.serializeToString(*this);

  // Deserialize back to a new AST node
  sun::serialization::ASTDeserializer deserializer;
  return deserializer.deserializeFromString(data);
}

void ExprAST::inheritSourceFile(sun::SourceFileId id) {
  if (!sourceFileId_) sourceFileId_ = id;
  // Typed children are forwarded by forEachChildSlot rather than visited.
  // Visit their roots here as well so generated methods retain their file.
  switch (getType()) {
    case ASTNodeType::FUNCTION: {
      auto& function = static_cast<FunctionAST&>(*this);
      if (function.hasBody())
        const_cast<BlockExprAST&>(function.getBody())
            .inheritSourceFile(sourceFileId_);
      return;
    }
    case ASTNodeType::LAMBDA:
      const_cast<BlockExprAST&>(static_cast<LambdaAST&>(*this).getBody())
          .inheritSourceFile(sourceFileId_);
      return;
    case ASTNodeType::MODULE:
      const_cast<BlockExprAST&>(static_cast<ModuleAST&>(*this).getBody())
          .inheritSourceFile(sourceFileId_);
      return;
    case ASTNodeType::MOON_SCOPE:
      static_cast<MoonScopeAST&>(*this).getBody().inheritSourceFile(
          sourceFileId_);
      return;
    case ASTNodeType::CLASS_DEFINITION:
      for (const auto& method :
           static_cast<ClassDefinitionAST&>(*this).getMethods())
        if (method.function) method.function->inheritSourceFile(sourceFileId_);
      return;
    case ASTNodeType::INTERFACE_DEFINITION:
      for (const auto& method :
           static_cast<InterfaceDefinitionAST&>(*this).getMethods())
        if (method.function) method.function->inheritSourceFile(sourceFileId_);
      return;
    default:
      forEachChildSlot([&](std::unique_ptr<ExprAST>& child) {
        if (child) child->inheritSourceFile(sourceFileId_);
      });
  }
}
