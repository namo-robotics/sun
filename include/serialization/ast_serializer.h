// ast_serializer.h — Serialize AST nodes to protobuf format

#pragma once

#include <memory>
#include <string>

#include "ast.h"
#include "ast.pb.h"

namespace sun {
namespace serialization {

inline ast::Visibility toProto(sun::Visibility v) {
  return v == sun::Visibility::Public ? ast::PUBLIC : ast::PRIVATE;
}

// Configuration for AST serialization
struct SerializerConfig {
  bool include_location = true;  // Include source locations
};

// Serialize AST nodes to protobuf format
class ASTSerializer {
 public:
  explicit ASTSerializer(SerializerConfig config = {}) : config_(config) {}

  // Serialize a complete program (root block)
  ast::Program serializeProgram(const BlockExprAST& root) const;

  // Serialize any expression node
  ast::ASTNode serialize(const ExprAST& expr) const;

  // Serialize a prototype (non-ExprAST node)
  ast::Prototype serializePrototype(const PrototypeAST& proto) const;

  // Serialize to bytes (convenience)
  std::string serializeToString(const ExprAST& expr) const;
  std::string serializeProgramToString(const BlockExprAST& root) const;

 private:
  SerializerConfig config_;

  // Type annotation serialization
  ast::TypeAnnotation serializeTypeAnnotation(const TypeAnnotation& type) const;

  // Position serialization
  ast::Position serializePosition(const Position& pos) const;

  // Token serialization (for operators)
  ast::Token serializeToken(const Token& token) const;

  // Capture serialization
  ast::Capture serializeCapture(const Capture& cap) const;

  // Set common ExprAST fields on the proto message
  void serializeExprBase(const ExprAST& expr, ast::ASTNode* node) const;

  // Statements and span of a block that is stored without a node wrapper
  // (function bodies, module bodies, try and catch blocks)
  void serializeBlockInto(const BlockExprAST& block,
                          ast::BlockExpr* proto) const;

  // One index of a subscript: `a[i]` and `a[i..j]` share a slice message
  void serializeSliceInto(const SliceExprAST& slice,
                          ast::SliceExpr* proto) const;

  // The function behind a class or interface method
  void serializeMethodFunction(const FunctionAST& function,
                               ast::FunctionDef* proto) const;

  // Class and interface fields are declared alike, so they store alike
  template <typename FieldDecl, typename FieldProto>
  void serializeFieldInto(const FieldDecl& field, FieldProto* proto) const {
    proto->set_name(field.name);
    *proto->mutable_type() = serializeTypeAnnotation(field.type);
    if (config_.include_location) {
      *proto->mutable_location() = serializePosition(field.location);
    }
    proto->set_visibility(toProto(field.visibility));
    proto->set_doc(field.doc);
  }

  // Individual node type serializers (dispatch by ASTNodeType)
  void serializeNumber(const NumberExprAST& expr, ast::ASTNode* node) const;
  void serializeCharLiteral(const CharLiteralAST& expr,
                            ast::ASTNode* node) const;
  void serializeString(const StringLiteralAST& expr, ast::ASTNode* node) const;
  void serializeBool(const BoolLiteralAST& expr, ast::ASTNode* node) const;
  void serializeArray(const ArrayLiteralAST& expr, ast::ASTNode* node) const;
  void serializeStructLiteral(const StructLiteralAST& expr,
                              ast::ASTNode* node) const;

  void serializeSlice(const SliceExprAST& expr, ast::ASTNode* node) const;
  void serializeIndex(const IndexAST& expr, ast::ASTNode* node) const;
  void serializeArrayIndex(const ArrayIndexAST& expr, ast::ASTNode* node) const;

  void serializeVariableRef(const VariableReferenceAST& expr,
                            ast::ASTNode* node) const;
  void serializeVariableCreation(const VariableCreationAST& expr,
                                 ast::ASTNode* node) const;
  void serializeVariableAssignment(const VariableAssignmentAST& expr,
                                   ast::ASTNode* node) const;
  void serializeReferenceCreation(const ReferenceCreationAST& expr,
                                  ast::ASTNode* node) const;
  void serializeIndexedAssignment(const IndexedAssignmentAST& expr,
                                  ast::ASTNode* node) const;
  void serializeMemberAssignment(const MemberAssignmentAST& expr,
                                 ast::ASTNode* node) const;
  void serializeCompoundAssignment(const CompoundAssignmentAST& expr,
                                   ast::ASTNode* node) const;

  void serializeBinary(const BinaryExprAST& expr, ast::ASTNode* node) const;
  void serializeUnary(const UnaryExprAST& expr, ast::ASTNode* node) const;
  void serializeTernary(const TernaryExprAST& expr, ast::ASTNode* node) const;
  void serializeParen(const ParenExprAST& expr, ast::ASTNode* node) const;
  void serializeInterpolatedString(const InterpolatedStringAST& expr,
                                   ast::ASTNode* node) const;
  void serializePackExpansion(const PackExpansionAST& expr,
                              ast::ASTNode* node) const;

  void serializeBlock(const BlockExprAST& expr, ast::ASTNode* node) const;
  void serializeIf(const IfExprAST& expr, ast::ASTNode* node) const;
  void serializeMatch(const MatchExprAST& expr, ast::ASTNode* node) const;
  void serializeFor(const ForExprAST& expr, ast::ASTNode* node) const;
  void serializeForIn(const ForInExprAST& expr, ast::ASTNode* node) const;
  void serializeWhile(const WhileExprAST& expr, ast::ASTNode* node) const;
  void serializeReturn(const ReturnExprAST& expr, ast::ASTNode* node) const;
  void serializeUnsafeBlock(const UnsafeBlockAST& expr,
                            ast::ASTNode* node) const;

  void serializeFunction(const FunctionAST& expr, ast::ASTNode* node) const;
  void serializeLambda(const LambdaAST& expr, ast::ASTNode* node) const;
  void serializeCall(const CallExprAST& expr, ast::ASTNode* node) const;
  void serializeGenericCall(const GenericCallAST& expr,
                            ast::ASTNode* node) const;

  void serializeManifest(const ManifestAST& expr, ast::ASTNode* node) const;
  void serializeModule(const ModuleAST& expr, ast::ASTNode* node) const;
  void serializeUsing(const UsingAST& expr, ast::ASTNode* node) const;
  void serializeQualifiedName(const QualifiedNameAST& expr,
                              ast::ASTNode* node) const;

  void serializeClassDef(const ClassDefinitionAST& expr,
                         ast::ASTNode* node) const;
  void serializeInterfaceDef(const InterfaceDefinitionAST& expr,
                             ast::ASTNode* node) const;
  void serializeEnumDef(const EnumDefinitionAST& expr,
                        ast::ASTNode* node) const;
  void serializeMemberAccess(const MemberAccessAST& expr,
                             ast::ASTNode* node) const;

  void serializeTryCatch(const TryCatchExprAST& expr, ast::ASTNode* node) const;
  void serializeThrow(const ThrowExprAST& expr, ast::ASTNode* node) const;
  void serializeDeclareType(const DeclareTypeAST& expr,
                            ast::ASTNode* node) const;
};

}  // namespace serialization
}  // namespace sun
