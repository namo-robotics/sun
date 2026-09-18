// ast_serializer.h — Serialize AST nodes to protobuf format

#pragma once

#include <memory>
#include <string>

#include "ast.h"
#include "ast.pb.h"

namespace sun::serialization {
namespace pbc = sun::proto::ast;

using sun::ast::BlockExprAST;
using sun::ast::ExprAST;

inline pbc::Visibility toProto(sun::semantic_analysis::Visibility v) {
  return v == sun::semantic_analysis::Visibility::Public ? pbc::PUBLIC
                                                         : pbc::PRIVATE;
}

// Configuration for AST serialization
struct SerializerConfig {
  const sun::semantic_analysis::DeclarationTable* declarations = nullptr;
  bool include_location = true;  // Include source locations
};

// Serialize AST nodes to protobuf format
class ASTSerializer {
 public:
  explicit ASTSerializer(SerializerConfig config = {}) : config_(config) {}

  // Serialize a complete program (root block)
  pbc::Program serializeProgram(const BlockExprAST& root) const;

  // Serialize any expression node
  pbc::ASTNode serialize(const ExprAST& expr) const;

  // Serialize a prototype (non-ExprAST node)
  pbc::Prototype serializePrototype(const sun::ast::PrototypeAST& proto) const;

  // Serialize to bytes (convenience)
  std::string serializeToString(const ExprAST& expr) const;
  std::string serializeProgramToString(const BlockExprAST& root) const;

 private:
  SerializerConfig config_;

  // Store a parameter and its optional constraint for any declaration kind.
  void serializeTypeParameterInto(const sun::ast::TypeParameter& parameter,
                                  pbc::TypeParameter* proto) const;

  // Export declaration and binder keys only at the artifact boundary.
  pbc::DeclarationIdentity serializeIdentity(
      const sun::semantic_analysis::DeclarationIdentity& identity) const;

  // Type annotation serialization
  pbc::TypeAnnotation serializeTypeAnnotation(
      const sun::ast::TypeAnnotation& type) const;

  // Position serialization
  pbc::Position serializePosition(const sun::support::Position& pos) const;

  // Token serialization (for operators)
  pbc::Token serializeToken(const sun::parsing::Token& token) const;

  // Set common ExprAST fields on the proto message
  void serializeExprBase(const ExprAST& expr, pbc::ASTNode* node) const;

  // Statements and span of a block that is stored without a node wrapper
  // (function bodies, module bodies, try and catch blocks)
  void serializeBlockInto(const BlockExprAST& block,
                          pbc::BlockExpr* proto) const;

  // One index of a subscript: `a[i]` and `a[i..j]` share a slice message
  void serializeSliceInto(const sun::ast::SliceExprAST& slice,
                          pbc::SliceExpr* proto) const;

  // The function behind a class or interface method
  void serializeMethodFunction(const sun::ast::FunctionAST& function,
                               pbc::FunctionDef* proto) const;

  // Class and interface fields are declared alike, so they store alike
  template <typename FieldDecl, typename FieldProto>
  void serializeFieldInto(const FieldDecl& field, FieldProto* proto) const {
    if (config_.declarations)
      *proto->mutable_declaration_identity() =
          serializeIdentity(field.declaration);
    proto->set_name(field.name);
    *proto->mutable_type() = serializeTypeAnnotation(field.type);
    if (config_.include_location) {
      *proto->mutable_location() = serializePosition(field.location);
    }
    proto->set_visibility(toProto(field.visibility));
    proto->set_doc(field.doc);
  }

  // Individual node type serializers (dispatch by ASTNodeType)
  void serializeNumber(const sun::ast::NumberExprAST& expr,
                       pbc::ASTNode* node) const;
  void serializeCharLiteral(const sun::ast::CharLiteralAST& expr,
                            pbc::ASTNode* node) const;
  void serializeString(const sun::ast::StringLiteralAST& expr,
                       pbc::ASTNode* node) const;
  void serializeBool(const sun::ast::BoolLiteralAST& expr,
                     pbc::ASTNode* node) const;
  void serializeArray(const sun::ast::ArrayLiteralAST& expr,
                      pbc::ASTNode* node) const;
  void serializeStructLiteral(const sun::ast::StructLiteralAST& expr,
                              pbc::ASTNode* node) const;

  void serializeSlice(const sun::ast::SliceExprAST& expr,
                      pbc::ASTNode* node) const;
  void serializeIndex(const sun::ast::IndexAST& expr, pbc::ASTNode* node) const;
  void serializeArrayIndex(const sun::ast::ArrayIndexAST& expr,
                           pbc::ASTNode* node) const;

  void serializeVariableRef(const sun::ast::VariableReferenceAST& expr,
                            pbc::ASTNode* node) const;
  void serializeVariableCreation(const sun::ast::VariableCreationAST& expr,
                                 pbc::ASTNode* node) const;
  void serializeVariableAssignment(const sun::ast::VariableAssignmentAST& expr,
                                   pbc::ASTNode* node) const;
  void serializeReferenceCreation(const sun::ast::ReferenceCreationAST& expr,
                                  pbc::ASTNode* node) const;
  void serializeIndexedAssignment(const sun::ast::IndexedAssignmentAST& expr,
                                  pbc::ASTNode* node) const;
  void serializeMemberAssignment(const sun::ast::MemberAssignmentAST& expr,
                                 pbc::ASTNode* node) const;
  void serializeCompoundAssignment(const sun::ast::CompoundAssignmentAST& expr,
                                   pbc::ASTNode* node) const;

  void serializeBinary(const sun::ast::BinaryExprAST& expr,
                       pbc::ASTNode* node) const;
  void serializeUnary(const sun::ast::UnaryExprAST& expr,
                      pbc::ASTNode* node) const;
  void serializeTernary(const sun::ast::TernaryExprAST& expr,
                        pbc::ASTNode* node) const;
  void serializeParen(const sun::ast::ParenExprAST& expr,
                      pbc::ASTNode* node) const;
  void serializeInterpolatedString(const sun::ast::InterpolatedStringAST& expr,
                                   pbc::ASTNode* node) const;
  void serializePackExpansion(const sun::ast::PackExpansionAST& expr,
                              pbc::ASTNode* node) const;

  void serializeBlock(const BlockExprAST& expr, pbc::ASTNode* node) const;
  void serializeIf(const sun::ast::IfExprAST& expr, pbc::ASTNode* node) const;
  void serializeMatch(const sun::ast::MatchExprAST& expr,
                      pbc::ASTNode* node) const;
  void serializeFor(const sun::ast::ForExprAST& expr, pbc::ASTNode* node) const;
  void serializeForIn(const sun::ast::ForInExprAST& expr,
                      pbc::ASTNode* node) const;
  void serializeWhile(const sun::ast::WhileExprAST& expr,
                      pbc::ASTNode* node) const;
  void serializeReturn(const sun::ast::ReturnExprAST& expr,
                       pbc::ASTNode* node) const;
  void serializeUnsafeBlock(const sun::ast::UnsafeBlockAST& expr,
                            pbc::ASTNode* node) const;

  void serializeFunction(const sun::ast::FunctionAST& expr,
                         pbc::ASTNode* node) const;
  void serializeLambda(const sun::ast::LambdaAST& expr,
                       pbc::ASTNode* node) const;
  void serializeCall(const sun::ast::CallExprAST& expr,
                     pbc::ASTNode* node) const;
  void serializeGenericCall(const sun::ast::GenericCallAST& expr,
                            pbc::ASTNode* node) const;

  void serializeManifest(const sun::ast::ManifestAST& expr,
                         pbc::ASTNode* node) const;
  void serializeModule(const sun::ast::ModuleAST& expr,
                       pbc::ASTNode* node) const;
  void serializeUsing(const sun::ast::UsingAST& expr, pbc::ASTNode* node) const;
  void serializeQualifiedName(const sun::ast::QualifiedNameAST& expr,
                              pbc::ASTNode* node) const;

  void serializeClassDef(const sun::ast::ClassDefinitionAST& expr,
                         pbc::ASTNode* node) const;
  void serializeInterfaceDef(const sun::ast::InterfaceDefinitionAST& expr,
                             pbc::ASTNode* node) const;
  void serializeEnumDef(const sun::ast::EnumDefinitionAST& expr,
                        pbc::ASTNode* node) const;
  void serializeMemberAccess(const sun::ast::MemberAccessAST& expr,
                             pbc::ASTNode* node) const;

  void serializeTryCatch(const sun::ast::TryCatchExprAST& expr,
                         pbc::ASTNode* node) const;
  void serializeThrow(const sun::ast::ThrowExprAST& expr,
                      pbc::ASTNode* node) const;
  void serializeDeclareType(const sun::ast::DeclareTypeAST& expr,
                            pbc::ASTNode* node) const;
};

}  // namespace sun::serialization
