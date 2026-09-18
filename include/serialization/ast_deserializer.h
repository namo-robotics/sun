// ast_deserializer.h — Deserialize protobuf messages to AST nodes

#pragma once

#include <memory>
#include <string>

#include "ast.h"
#include "ast.pb.h"

namespace sun::serialization {
namespace pbc = sun::proto::ast;

using sun::ast::BlockExprAST;
using sun::ast::ExprAST;
using sun::semantic_analysis::Visibility;

inline Visibility fromProto(pbc::Visibility v) {
  return v == pbc::PUBLIC ? Visibility::Public : Visibility::Private;
}

// Configuration for AST deserialization
struct DeserializerConfig {
  bool import_declarations = false;
  // File given to positions that carry none (a .moon bundle stores the
  // module's source path once rather than on every position)
  std::string default_file_path;
};

// Deserialize protobuf messages to AST nodes
class ASTDeserializer {
 public:
  explicit ASTDeserializer(DeserializerConfig config = {}) : config_(config) {}

  // Deserialize a complete program
  std::unique_ptr<BlockExprAST> deserializeProgram(
      const pbc::Program& program) const;

  // Deserialize any expression node
  std::unique_ptr<ExprAST> deserialize(const pbc::ASTNode& node) const;

  // Deserialize a prototype
  std::unique_ptr<sun::ast::PrototypeAST> deserializePrototype(
      const pbc::Prototype& proto) const;

  // Deserialize from bytes (convenience)
  std::unique_ptr<ExprAST> deserializeFromString(const std::string& data) const;
  std::unique_ptr<BlockExprAST> deserializeProgramFromString(
      const std::string& data) const;

 private:
  DeserializerConfig config_;

  // Restore parameters from current metadata or the legacy names-only field.
  template <typename Owner>
  std::vector<sun::ast::TypeParameter> deserializeTypeParameters(
      const Owner& owner) const;

  // Retain validated portable identities until the declaration pass interns
  // them.
  void deserializeIdentity(
      const pbc::DeclarationIdentity& proto,
      sun::semantic_analysis::DeclarationIdentity& identity) const;

  // Type annotation deserialization
  sun::ast::TypeAnnotation deserializeTypeAnnotation(
      const pbc::TypeAnnotation& type) const;

  // Position deserialization
  sun::support::Position deserializePosition(const pbc::Position& pos) const;

  // Token deserialization
  sun::parsing::Token deserializeToken(const pbc::Token& token) const;

  // Restore common ExprAST fields from the proto message
  void deserializeExprBase(const pbc::ASTNode& node, ExprAST* expr) const;

  // The function behind a class or interface method. An interface method
  // with no statements is a bare declaration, so it comes back without a
  // body; a class method with no statements has an empty body, and dropping
  // that would leave the method looking like an extern declaration.
  std::unique_ptr<sun::ast::FunctionAST> deserializeMethodFunction(
      const pbc::FunctionDef& proto, bool emptyBodyMeansNone) const;

  // Class and interface fields are declared alike, so they load alike
  template <typename FieldDecl, typename FieldProto>
  FieldDecl deserializeField(const FieldProto& proto) const {
    FieldDecl field;
    if (proto.has_declaration_identity())
      deserializeIdentity(proto.declaration_identity(), field.declaration);
    field.name = proto.name();
    field.type = deserializeTypeAnnotation(proto.type());
    if (proto.has_location()) {
      field.location = deserializePosition(proto.location());
    }
    field.visibility = fromProto(proto.visibility());
    field.doc = proto.doc();
    return field;
  }

  // Individual node type deserializers
  std::unique_ptr<ExprAST> deserializeNumber(
      const pbc::NumberExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeCharLiteral(
      const pbc::CharLiteral& proto) const;
  std::unique_ptr<ExprAST> deserializeString(
      const pbc::StringLiteral& proto) const;
  std::unique_ptr<ExprAST> deserializeBool(const pbc::BoolLiteral& proto) const;
  std::unique_ptr<ExprAST> deserializeStructLiteral(
      const pbc::StructLiteral& proto) const;
  std::unique_ptr<ExprAST> deserializeArray(
      const pbc::ArrayLiteral& proto) const;

  std::unique_ptr<sun::ast::SliceExprAST> deserializeSliceExpr(
      const pbc::SliceExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeSlice(const pbc::SliceExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeIndex(const pbc::IndexExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeArrayIndex(
      const pbc::ArrayIndexExpr& proto) const;

  std::unique_ptr<ExprAST> deserializeVariableRef(
      const pbc::VariableReference& proto) const;
  std::unique_ptr<ExprAST> deserializeVariableCreation(
      const pbc::VariableCreation& proto) const;
  std::unique_ptr<ExprAST> deserializeVariableAssignment(
      const pbc::VariableAssignment& proto) const;
  std::unique_ptr<ExprAST> deserializeReferenceCreation(
      const pbc::ReferenceCreation& proto) const;
  std::unique_ptr<ExprAST> deserializeIndexedAssignment(
      const pbc::IndexedAssignment& proto) const;
  std::unique_ptr<ExprAST> deserializeMemberAssignment(
      const pbc::MemberAssignment& proto) const;
  std::unique_ptr<ExprAST> deserializeCompoundAssignment(
      const pbc::CompoundAssignment& proto) const;

  std::unique_ptr<ExprAST> deserializeBinary(
      const pbc::BinaryExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeUnary(const pbc::UnaryExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeTernary(
      const pbc::TernaryExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeParen(const pbc::ParenExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeInterpolatedString(
      const pbc::InterpolatedString& proto) const;
  std::unique_ptr<ExprAST> deserializePackExpansion(
      const pbc::PackExpansion& proto) const;

  std::unique_ptr<ExprAST> deserializeBlock(const pbc::BlockExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeIf(const pbc::IfExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeMatch(const pbc::MatchExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeFor(const pbc::ForExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeForIn(const pbc::ForInExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeWhile(const pbc::WhileExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeReturn(
      const pbc::ReturnExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeUnsafeBlock(
      const pbc::UnsafeBlock& proto) const;

  std::unique_ptr<ExprAST> deserializeFunction(
      const pbc::FunctionDef& proto) const;
  std::unique_ptr<ExprAST> deserializeLambda(
      const pbc::LambdaExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeCall(const pbc::CallExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeGenericCall(
      const pbc::GenericCallExpr& proto) const;

  std::unique_ptr<ExprAST> deserializeManifest(
      const pbc::Manifest& proto) const;
  std::unique_ptr<ExprAST> deserializeModule(const pbc::ModuleDef& proto) const;
  std::unique_ptr<ExprAST> deserializeUsing(const pbc::UsingStmt& proto) const;
  std::unique_ptr<ExprAST> deserializeQualifiedName(
      const pbc::QualifiedNameExpr& proto) const;

  std::unique_ptr<ExprAST> deserializeClassDef(
      const pbc::ClassDef& proto) const;
  std::unique_ptr<ExprAST> deserializeInterfaceDef(
      const pbc::InterfaceDef& proto) const;
  std::unique_ptr<ExprAST> deserializeEnumDef(const pbc::EnumDef& proto) const;
  std::unique_ptr<ExprAST> deserializeMemberAccess(
      const pbc::MemberAccess& proto) const;

  std::unique_ptr<ExprAST> deserializeTryCatch(
      const pbc::TryCatch& proto) const;
  std::unique_ptr<ExprAST> deserializeThrow(const pbc::ThrowExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeDeclareType(
      const pbc::DeclareType& proto) const;

  // Helper to deserialize a BlockExpr specifically
  std::unique_ptr<BlockExprAST> deserializeBlockExpr(
      const pbc::BlockExpr& proto) const;
};

}  // namespace sun::serialization
