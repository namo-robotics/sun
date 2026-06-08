// expr_ast.cpp — Implementation of ExprAST non-inline methods

#include "ast/expr_ast.h"

#include "ast_deserializer.h"
#include "ast_serializer.h"

std::unique_ptr<ExprAST> ExprAST::clone() const {
  // Serialize this node to protobuf
  sun::serialization::ASTSerializer serializer;
  std::string data = serializer.serializeToString(*this);

  // Deserialize back to a new AST node
  sun::serialization::ASTDeserializer deserializer;
  return deserializer.deserializeFromString(data);
}
