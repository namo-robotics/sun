// prototype_ast.cpp — PrototypeAST clone implementation

#include "ast/prototype_ast.h"

#include "ast_deserializer.h"
#include "ast_serializer.h"

std::unique_ptr<PrototypeAST> PrototypeAST::clone() const {
  // Serialize this prototype to protobuf
  sun::serialization::ASTSerializer serializer;
  sun::ast::Prototype proto = serializer.serializePrototype(*this);

  // Deserialize back to a new PrototypeAST
  sun::serialization::ASTDeserializer deserializer;
  return deserializer.deserializePrototype(proto);
}
