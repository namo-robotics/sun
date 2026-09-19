// prototype_ast.cpp — PrototypeAST clone implementation

#include "ast/prototype_ast.h"

#include "serialization/ast_deserializer.h"
#include "serialization/ast_serializer.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

std::unique_ptr<PrototypeAST> PrototypeAST::clone() const {
  // Serialize this prototype to protobuf
  sun::serialization::ASTSerializer serializer;
  sun::proto::ast::Prototype proto = serializer.serializePrototype(*this);

  // Deserialize back to a new PrototypeAST
  sun::serialization::ASTDeserializer deserializer;
  return deserializer.deserializePrototype(proto);
}

}  // namespace sun::ast
