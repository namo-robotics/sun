// ast_deserializer.h — Deserialize protobuf messages to AST nodes

#pragma once

#include <memory>
#include <string>

#include "ast.h"
#include "ast.pb.h"

namespace sun {
namespace serialization {

// Configuration for AST deserialization
struct DeserializerConfig {
  bool restore_analysis = false;  // Restore semantic analysis data
};

// Deserialize protobuf messages to AST nodes
class ASTDeserializer {
 public:
  explicit ASTDeserializer(DeserializerConfig config = {}) : config_(config) {}

  // Deserialize a complete program
  std::unique_ptr<BlockExprAST> deserializeProgram(
      const ast::Program& program) const;

  // Deserialize any expression node
  std::unique_ptr<ExprAST> deserialize(const ast::ASTNode& node) const;

  // Deserialize a prototype
  std::unique_ptr<PrototypeAST> deserializePrototype(
      const ast::Prototype& proto) const;

  // Deserialize from bytes (convenience)
  std::unique_ptr<ExprAST> deserializeFromString(const std::string& data) const;
  std::unique_ptr<BlockExprAST> deserializeProgramFromString(
      const std::string& data) const;

 private:
  DeserializerConfig config_;

  // Type annotation deserialization
  TypeAnnotation deserializeTypeAnnotation(
      const ast::TypeAnnotation& type) const;

  // Position deserialization
  Position deserializePosition(const ast::Position& pos) const;

  // Token deserialization
  Token deserializeToken(const ast::Token& token) const;

  // Capture deserialization
  Capture deserializeCapture(const ast::Capture& cap) const;

  // Restore common ExprAST fields from the proto message
  void deserializeExprBase(const ast::ASTNode& node, ExprAST* expr) const;

  // Individual node type deserializers
  std::unique_ptr<ExprAST> deserializeNumber(
      const ast::NumberExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeString(
      const ast::StringLiteral& proto) const;
  std::unique_ptr<ExprAST> deserializeNull(const ast::NullLiteral& proto) const;
  std::unique_ptr<ExprAST> deserializeBool(const ast::BoolLiteral& proto) const;
  std::unique_ptr<ExprAST> deserializeArray(
      const ast::ArrayLiteral& proto) const;

  std::unique_ptr<ExprAST> deserializeSlice(const ast::SliceExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeIndex(const ast::IndexExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeArrayIndex(
      const ast::ArrayIndexExpr& proto) const;

  std::unique_ptr<ExprAST> deserializeVariableRef(
      const ast::VariableReference& proto) const;
  std::unique_ptr<ExprAST> deserializeVariableCreation(
      const ast::VariableCreation& proto) const;
  std::unique_ptr<ExprAST> deserializeVariableAssignment(
      const ast::VariableAssignment& proto) const;
  std::unique_ptr<ExprAST> deserializeReferenceCreation(
      const ast::ReferenceCreation& proto) const;
  std::unique_ptr<ExprAST> deserializeIndexedAssignment(
      const ast::IndexedAssignment& proto) const;
  std::unique_ptr<ExprAST> deserializeMemberAssignment(
      const ast::MemberAssignment& proto) const;

  std::unique_ptr<ExprAST> deserializeBinary(
      const ast::BinaryExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeUnary(const ast::UnaryExpr& proto) const;
  std::unique_ptr<ExprAST> deserializePackExpansion(
      const ast::PackExpansion& proto) const;

  std::unique_ptr<ExprAST> deserializeBlock(const ast::BlockExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeIf(const ast::IfExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeMatch(const ast::MatchExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeFor(const ast::ForExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeForIn(const ast::ForInExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeWhile(const ast::WhileExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeBreak(const ast::BreakStmt& proto) const;
  std::unique_ptr<ExprAST> deserializeContinue(
      const ast::ContinueStmt& proto) const;
  std::unique_ptr<ExprAST> deserializeReturn(
      const ast::ReturnExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeUnsafeBlock(
      const ast::UnsafeBlock& proto) const;

  std::unique_ptr<ExprAST> deserializeFunction(
      const ast::FunctionDef& proto) const;
  std::unique_ptr<ExprAST> deserializeLambda(
      const ast::LambdaExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeCall(const ast::CallExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeGenericCall(
      const ast::GenericCallExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeSpawn(const ast::SpawnExpr& proto) const;

  std::unique_ptr<ExprAST> deserializeManifest(
      const ast::Manifest& proto) const;
  std::unique_ptr<ExprAST> deserializeModule(const ast::ModuleDef& proto) const;
  std::unique_ptr<ExprAST> deserializeUsing(const ast::UsingStmt& proto) const;
  std::unique_ptr<ExprAST> deserializeQualifiedName(
      const ast::QualifiedName& proto) const;

  std::unique_ptr<ExprAST> deserializeClassDef(
      const ast::ClassDef& proto) const;
  std::unique_ptr<ExprAST> deserializeInterfaceDef(
      const ast::InterfaceDef& proto) const;
  std::unique_ptr<ExprAST> deserializeEnumDef(const ast::EnumDef& proto) const;
  std::unique_ptr<ExprAST> deserializeThis(const ast::ThisExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeMemberAccess(
      const ast::MemberAccess& proto) const;

  std::unique_ptr<ExprAST> deserializeTryCatch(
      const ast::TryCatch& proto) const;
  std::unique_ptr<ExprAST> deserializeThrow(const ast::ThrowExpr& proto) const;
  std::unique_ptr<ExprAST> deserializeDeclareType(
      const ast::DeclareType& proto) const;

  // Helper to deserialize a BlockExpr specifically
  std::unique_ptr<BlockExprAST> deserializeBlockExpr(
      const ast::BlockExpr& proto) const;
};

}  // namespace serialization
}  // namespace sun
