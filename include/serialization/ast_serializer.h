// ast_serializer.h — Serialize AST nodes to protobuf format

#pragma once

#include <memory>
#include <string>

#include "ast.h"
#include "ast.pb.h"

/** Converts syntax trees to and from the compiler protobuf representation. */
namespace sun::serialization {
namespace pbc = sun::proto::ast;

using sun::ast::BlockExprAST;
using sun::ast::ExprAST;

/** Converts compiler visibility to its serialized representation. */
inline pbc::Visibility toProto(sun::semantic_analysis::Visibility v) {
  return v == sun::semantic_analysis::Visibility::Public ? pbc::PUBLIC
                                                         : pbc::PRIVATE;
}

/**
 * Configuration for AST serialization
 */
struct SerializerConfig {
  const sun::semantic_analysis::DeclarationTable* declarations = nullptr;
  bool include_location = true;  // Include source locations
};

/**
 * Serialize AST nodes to protobuf format
 */
class ASTSerializer {
 public:
  /** Creates a syntax-tree encoder using the supplied serialization settings.
   */
  explicit ASTSerializer(SerializerConfig config = {}) : config_(config) {}

  /**
   * Serialize a complete program (root block)
   */
  pbc::Program serializeProgram(const BlockExprAST& root) const;

  /**
   * Serialize any expression node
   */
  pbc::ASTNode serialize(const ExprAST& expr) const;

  /**
   * Serialize a prototype (non-ExprAST node)
   */
  pbc::Prototype serializePrototype(const sun::ast::PrototypeAST& proto) const;

  /**
   * Serialize to bytes (convenience)
   */
  std::string serializeToString(const ExprAST& expr) const;
  /** Encodes a program syntax tree as a protobuf byte string. */
  std::string serializeProgramToString(const BlockExprAST& root) const;

 private:
  SerializerConfig config_;

  /**
   * Store a parameter and its optional constraint for any declaration kind.
   */
  void serializeTypeParameterInto(const sun::ast::TypeParameter& parameter,
                                  pbc::TypeParameter* proto) const;

  /**
   * Export declaration and binder keys only at the artifact boundary.
   */
  pbc::DeclarationIdentity serializeIdentity(
      const sun::semantic_analysis::DeclarationIdentity& identity) const;

  /**
   * Type annotation serialization
   */
  pbc::TypeAnnotation serializeTypeAnnotation(
      const sun::ast::TypeAnnotation& type) const;

  /**
   * Position serialization
   */
  pbc::Position serializePosition(const sun::support::Position& pos) const;

  /**
   * Token serialization (for operators)
   */
  pbc::Token serializeToken(const sun::parsing::Token& token) const;

  /**
   * Set common ExprAST fields on the proto message
   */
  void serializeExprBase(const ExprAST& expr, pbc::ASTNode* node) const;

  /**
   * Statements and span of a block that is stored without a node wrapper
   * (function bodies, module bodies, try and catch blocks)
   */
  void serializeBlockInto(const BlockExprAST& block,
                          pbc::BlockExpr* proto) const;

  /**
   * One index of a subscript: `a[i]` and `a[i..j]` share a slice message
   */
  void serializeSliceInto(const sun::ast::SliceExprAST& slice,
                          pbc::SliceExpr* proto) const;

  /**
   * The function behind a class or interface method
   */
  void serializeMethodFunction(const sun::ast::FunctionAST& function,
                               pbc::FunctionDef* proto) const;

  /**
   * Class and interface fields are declared alike, so they store alike
   */
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

  /**
   * Individual node type serializers (dispatch by ASTNodeType)
   */
  void serializeNumber(const sun::ast::NumberExprAST& expr,
                       pbc::ASTNode* node) const;
  /** Encodes a character literal and its child data in the protobuf syntax
   * representation. */
  void serializeCharLiteral(const sun::ast::CharLiteralAST& expr,
                            pbc::ASTNode* node) const;
  /** Encodes a string literal and its child data in the protobuf syntax
   * representation. */
  void serializeString(const sun::ast::StringLiteralAST& expr,
                       pbc::ASTNode* node) const;
  /** Encodes a boolean literal and its child data in the protobuf syntax
   * representation. */
  void serializeBool(const sun::ast::BoolLiteralAST& expr,
                     pbc::ASTNode* node) const;
  /** Encodes a array literal and its child data in the protobuf syntax
   * representation. */
  void serializeArray(const sun::ast::ArrayLiteralAST& expr,
                      pbc::ASTNode* node) const;
  /**
   * Encodes a field initializer list and its child data in the protobuf syntax
   * representation.
   */
  void serializeStructLiteral(const sun::ast::StructLiteralAST& expr,
                              pbc::ASTNode* node) const;

  /** Encodes a slice expression and its child data in the protobuf syntax
   * representation. */
  void serializeSlice(const sun::ast::SliceExprAST& expr,
                      pbc::ASTNode* node) const;
  /** Encodes a index expression and its child data in the protobuf syntax
   * representation. */
  void serializeIndex(const sun::ast::IndexAST& expr, pbc::ASTNode* node) const;
  /** Encodes a array index and its child data in the protobuf syntax
   * representation. */
  void serializeArrayIndex(const sun::ast::ArrayIndexAST& expr,
                           pbc::ASTNode* node) const;

  /**
   * Encodes a variable reference and its child data in the protobuf syntax
   * representation.
   */
  void serializeVariableRef(const sun::ast::VariableReferenceAST& expr,
                            pbc::ASTNode* node) const;
  /**
   * Encodes a variable declaration and its child data in the protobuf syntax
   * representation.
   */
  void serializeVariableCreation(const sun::ast::VariableCreationAST& expr,
                                 pbc::ASTNode* node) const;
  /**
   * Encodes a variable assignment and its child data in the protobuf syntax
   * representation.
   */
  void serializeVariableAssignment(const sun::ast::VariableAssignmentAST& expr,
                                   pbc::ASTNode* node) const;
  /**
   * Encodes a reference creation and its child data in the protobuf syntax
   * representation.
   */
  void serializeReferenceCreation(const sun::ast::ReferenceCreationAST& expr,
                                  pbc::ASTNode* node) const;
  /**
   * Encodes a indexed assignment and its child data in the protobuf syntax
   * representation.
   */
  void serializeIndexedAssignment(const sun::ast::IndexedAssignmentAST& expr,
                                  pbc::ASTNode* node) const;
  /** Encodes a member assignment and its child data in the protobuf syntax
   * representation. */
  void serializeMemberAssignment(const sun::ast::MemberAssignmentAST& expr,
                                 pbc::ASTNode* node) const;
  /**
   * Encodes a compound assignment and its child data in the protobuf syntax
   * representation.
   */
  void serializeCompoundAssignment(const sun::ast::CompoundAssignmentAST& expr,
                                   pbc::ASTNode* node) const;

  /** Encodes a binary operation and its child data in the protobuf syntax
   * representation. */
  void serializeBinary(const sun::ast::BinaryExprAST& expr,
                       pbc::ASTNode* node) const;
  /** Encodes a unary operation and its child data in the protobuf syntax
   * representation. */
  void serializeUnary(const sun::ast::UnaryExprAST& expr,
                      pbc::ASTNode* node) const;
  /**
   * Encodes a conditional expression and its child data in the protobuf syntax
   * representation.
   */
  void serializeTernary(const sun::ast::TernaryExprAST& expr,
                        pbc::ASTNode* node) const;
  /**
   * Encodes a parenthesized expression and its child data in the protobuf
   * syntax representation.
   */
  void serializeParen(const sun::ast::ParenExprAST& expr,
                      pbc::ASTNode* node) const;
  /**
   * Encodes a interpolated string and its child data in the protobuf syntax
   * representation.
   */
  void serializeInterpolatedString(const sun::ast::InterpolatedStringAST& expr,
                                   pbc::ASTNode* node) const;
  /**
   * Encodes a variadic argument expansion and its child data in the protobuf
   * syntax representation.
   */
  void serializePackExpansion(const sun::ast::PackExpansionAST& expr,
                              pbc::ASTNode* node) const;

  /** Encodes a lexical block and its child data in the protobuf syntax
   * representation. */
  void serializeBlock(const BlockExprAST& expr, pbc::ASTNode* node) const;
  /**
   * Encodes a conditional expression and its child data in the protobuf syntax
   * representation.
   */
  void serializeIf(const sun::ast::IfExprAST& expr, pbc::ASTNode* node) const;
  /** Encodes a pattern match and its child data in the protobuf syntax
   * representation. */
  void serializeMatch(const sun::ast::MatchExprAST& expr,
                      pbc::ASTNode* node) const;
  /** Encodes a for loop and its child data in the protobuf syntax
   * representation. */
  void serializeFor(const sun::ast::ForExprAST& expr, pbc::ASTNode* node) const;
  /** Encodes a iteration loop and its child data in the protobuf syntax
   * representation. */
  void serializeForIn(const sun::ast::ForInExprAST& expr,
                      pbc::ASTNode* node) const;
  /** Encodes a while loop and its child data in the protobuf syntax
   * representation. */
  void serializeWhile(const sun::ast::WhileExprAST& expr,
                      pbc::ASTNode* node) const;
  /** Encodes a return statement and its child data in the protobuf syntax
   * representation. */
  void serializeReturn(const sun::ast::ReturnExprAST& expr,
                       pbc::ASTNode* node) const;
  /** Encodes a unsafe block and its child data in the protobuf syntax
   * representation. */
  void serializeUnsafeBlock(const sun::ast::UnsafeBlockAST& expr,
                            pbc::ASTNode* node) const;

  /**
   * Encodes a function definition and its child data in the protobuf syntax
   * representation.
   */
  void serializeFunction(const sun::ast::FunctionAST& expr,
                         pbc::ASTNode* node) const;
  /** Encodes a lambda expression and its child data in the protobuf syntax
   * representation. */
  void serializeLambda(const sun::ast::LambdaAST& expr,
                       pbc::ASTNode* node) const;
  /** Encodes a function call and its child data in the protobuf syntax
   * representation. */
  void serializeCall(const sun::ast::CallExprAST& expr,
                     pbc::ASTNode* node) const;
  /**
   * Encodes a generic function call and its child data in the protobuf syntax
   * representation.
   */
  void serializeGenericCall(const sun::ast::GenericCallAST& expr,
                            pbc::ASTNode* node) const;

  /**
   * Encodes a dependency manifest and its child data in the protobuf syntax
   * representation.
   */
  void serializeManifest(const sun::ast::ManifestAST& expr,
                         pbc::ASTNode* node) const;
  /**
   * Encodes a module declaration and its child data in the protobuf syntax
   * representation.
   */
  void serializeModule(const sun::ast::ModuleAST& expr,
                       pbc::ASTNode* node) const;
  /** Encodes a using declaration and its child data in the protobuf syntax
   * representation. */
  void serializeUsing(const sun::ast::UsingAST& expr, pbc::ASTNode* node) const;
  /** Encodes a qualified name and its child data in the protobuf syntax
   * representation. */
  void serializeQualifiedName(const sun::ast::QualifiedNameAST& expr,
                              pbc::ASTNode* node) const;

  /** Encodes a class declaration and its child data in the protobuf syntax
   * representation. */
  void serializeClassDef(const sun::ast::ClassDefinitionAST& expr,
                         pbc::ASTNode* node) const;
  /**
   * Encodes a interface declaration and its child data in the protobuf syntax
   * representation.
   */
  void serializeInterfaceDef(const sun::ast::InterfaceDefinitionAST& expr,
                             pbc::ASTNode* node) const;
  /** Encodes a enum declaration and its child data in the protobuf syntax
   * representation. */
  void serializeEnumDef(const sun::ast::EnumDefinitionAST& expr,
                        pbc::ASTNode* node) const;
  /** Encodes a member access and its child data in the protobuf syntax
   * representation. */
  void serializeMemberAccess(const sun::ast::MemberAccessAST& expr,
                             pbc::ASTNode* node) const;

  /** Encodes a error handler and its child data in the protobuf syntax
   * representation. */
  void serializeTryCatch(const sun::ast::TryCatchExprAST& expr,
                         pbc::ASTNode* node) const;
  /** Encodes a throw expression and its child data in the protobuf syntax
   * representation. */
  void serializeThrow(const sun::ast::ThrowExprAST& expr,
                      pbc::ASTNode* node) const;
  /**
   * Encodes a forward type declaration and its child data in the protobuf
   * syntax representation.
   */
  void serializeDeclareType(const sun::ast::DeclareTypeAST& expr,
                            pbc::ASTNode* node) const;
};

}  // namespace sun::serialization
