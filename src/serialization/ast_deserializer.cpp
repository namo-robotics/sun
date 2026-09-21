// ast_deserializer.cpp — Implementation of protobuf to AST deserialization

#include "serialization/ast_deserializer.h"

#include "ast.h"
#include "ast.pb.h"
#include "serialization/token_kind_proto_map.h"
#include "types.pb.h"

using sun::semantic_analysis::PortableDeclarationKey;

using sun::ast::BlockExprAST;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::ast::InterpolatedStringAST;
using sun::ast::NumberExprAST;
using sun::ast::SliceExprAST;
using sun::ast::StructLiteralAST;
using sun::parsing::Token;
using sun::support::Position;

/** Converts syntax trees to and from the compiler protobuf representation. */
namespace sun::serialization {
namespace pbc = sun::proto::ast;

/**
 * Copy a repeated string field into a plain vector
 */
template <typename Repeated>
static std::vector<std::string> toStringVector(const Repeated& field) {
  return std::vector<std::string>(field.begin(), field.end());
}

/**
 * Lifetime parameters, from the names the bundle carries. Bundles written
 * before lifetimes existed have none, which reads back as fully elided.
 */
template <typename Owner>
static std::vector<sun::ast::LifetimeParameter> toLifetimeParameters(
    const Owner& owner) {
  std::vector<sun::ast::LifetimeParameter> params;
  params.reserve(owner.lifetime_params_size());
  for (const auto& name : owner.lifetime_params()) {
    params.emplace_back(name);
  }
  return params;
}

template <typename Owner>
std::vector<sun::ast::TypeParameter> ASTDeserializer::deserializeTypeParameters(
    const Owner& owner) const {
  std::vector<sun::ast::TypeParameter> params;
  if (owner.type_params_size() > 0) {
    params.reserve(owner.type_params_size());
    for (const auto& tp : owner.type_params()) {
      auto& parameter = params.emplace_back(tp.name());
      if (!tp.has_constraint()) continue;

      auto& constraint = parameter.constraint.emplace(tp.constraint());
      for (const auto& argument : tp.constraint_arguments())
        constraint.typeArguments.push_back(deserializeTypeAnnotation(argument));
      if (tp.has_declaration_key())
        constraint.declarationKey =
            PortableDeclarationKey::parseOriginal(tp.declaration_key());
    }
    return params;
  }
  params.reserve(owner.type_parameters_size());
  for (const auto& name : owner.type_parameters()) {
    params.emplace_back(name);
  }
  return params;
}

void ASTDeserializer::deserializeIdentity(
    const pbc::DeclarationIdentity& proto,
    sun::semantic_analysis::DeclarationIdentity& identity) const {
  if (!config_.import_declarations) return;
  if (proto.declaration().empty())
    sun::support::logAndThrowError(
        "Imported declaration has no portable identity");
  auto validate = [](const std::string& value) {
    PortableDeclarationKey::parseOriginal(value);
    return value;
  };
  sun::semantic_analysis::ImportedDeclarationIdentity imported;
  imported.declaration = validate(proto.declaration());
  for (const auto& value : proto.parameters())
    imported.parameters.push_back(validate(value));
  for (const auto& value : proto.type_parameters())
    imported.typeParameters.push_back(validate(value));
  for (const auto& value : proto.lifetime_parameters())
    imported.lifetimeParameters.push_back(validate(value));
  identity.imported = std::move(imported);
}

Position ASTDeserializer::deserializePosition(const pbc::Position& pos) const {
  Position result;
  result.line = pos.line();
  result.column = pos.column();
  result.offset = pos.offset();
  if (pos.has_file_path()) {
    result.filePath = pos.file_path();
  } else if (!config_.default_file_path.empty()) {
    result.filePath = config_.default_file_path;
  }
  if (pos.has_end_line()) {
    result.endLine = pos.end_line();
  }
  if (pos.has_end_column()) {
    result.endColumn = pos.end_column();
  }
  if (pos.has_end_offset()) {
    result.endOffset = pos.end_offset();
  }
  return result;
}

Token ASTDeserializer::deserializeToken(const pbc::Token& token) const {
  Token result;
  result.kind = fromProtoTokenKind(token.kind());
  result.text = token.text();
  return result;
}

sun::ast::TypeAnnotation ASTDeserializer::deserializeTypeAnnotation(
    const pbc::TypeAnnotation& type) const {
  sun::ast::TypeAnnotation result;
  result.baseName = type.base_name();
  if (type.has_declaration_key())
    result.declarationKey =
        PortableDeclarationKey::parseOriginal(type.declaration_key());

  if (type.has_element_type()) {
    result.elementType = std::make_unique<sun::ast::TypeAnnotation>(
        deserializeTypeAnnotation(type.element_type()));
  }

  for (const auto& param : type.param_types()) {
    result.paramTypes.push_back(std::make_unique<sun::ast::TypeAnnotation>(
        deserializeTypeAnnotation(param)));
  }

  if (type.has_return_type()) {
    result.returnType = std::make_unique<sun::ast::TypeAnnotation>(
        deserializeTypeAnnotation(type.return_type()));
  }

  for (const auto& arg : type.type_arguments()) {
    result.typeArguments.push_back(std::make_unique<sun::ast::TypeAnnotation>(
        deserializeTypeAnnotation(arg)));
  }

  for (const auto& dim : type.array_dimensions()) {
    sun::ast::ArrayDimension dimension;
    if (dim.has_size()) dimension.size = dim.size();
    dimension.constantName = dim.constant_name();
    result.arrayDimensions.push_back(std::move(dimension));
  }

  result.canError = type.can_error();
  result.requiresUnsafe = type.requires_unsafe();
  result.constRef = type.const_ref();
  result.refEnv = type.ref_env();
  result.lifetimeName = type.lifetime_name();
  if (result.refEnv && result.lifetimeName.empty()) {
    result.lifetimeName = "_";
  }
  for (const auto& lifetime : type.lifetime_arguments()) {
    result.lifetimeArguments.push_back(lifetime);
  }
  return result;
}

void ASTDeserializer::deserializeExprBase(const pbc::ASTNode& node,
                                          ExprAST* expr) const {
  if (node.has_location()) {
    expr->setLocation(deserializePosition(node.location()));
  }
  if (node.has_declaration_identity())
    deserializeIdentity(node.declaration_identity(),
                        expr->declarationIdentity());
  expr->setSourceFileId(node.source_file_id());
  if (node.has_module_declaration_key())
    expr->setModuleDeclaration(
        PortableDeclarationKey::parseOriginal(node.module_declaration_key()));
  expr->setPrecompiled(node.precompiled());
  expr->setSkipCodegen(node.skip_codegen());
  expr->setSymbolPrefix(node.symbol_prefix());
  // A serialized tree carries no analysis: what comes back is a parse tree,
  // and the semantic analyzer runs over it again.
}

std::unique_ptr<BlockExprAST> ASTDeserializer::deserializeProgram(
    const pbc::Program& program) const {
  return deserializeBlockExpr(program.body());
}

std::unique_ptr<sun::ast::PrototypeAST> ASTDeserializer::deserializePrototype(
    const pbc::Prototype& proto) const {
  std::vector<std::pair<std::string, sun::ast::TypeAnnotation>> args;
  for (const auto& arg : proto.args()) {
    args.emplace_back(arg.name(), deserializeTypeAnnotation(arg.type()));
  }

  std::optional<sun::ast::TypeAnnotation> returnType;
  if (proto.has_return_type()) {
    returnType = deserializeTypeAnnotation(proto.return_type());
  }

  std::optional<sun::ast::VariadicParam> variadicParam;
  if (proto.has_variadic_param_name()) {
    variadicParam.emplace(proto.variadic_param_name());
    if (proto.has_variadic_type_annotation()) {
      variadicParam->typeAnnotation =
          deserializeTypeAnnotation(proto.variadic_type_annotation());
    }
  }

  auto result = std::make_unique<sun::ast::PrototypeAST>(
      proto.name(), std::move(args), std::move(returnType),
      deserializeTypeParameters(proto), std::move(variadicParam));
  if (proto.has_declaration_identity())
    deserializeIdentity(proto.declaration_identity(),
                        result->declarationIdentity());
  result->setLifetimeParameters(toLifetimeParameters(proto));

  // Restore the declared capture list, as written
  result->setRefCaptureNames(toStringVector(proto.ref_captures()));
  result->setConstRefCaptureNames(toStringVector(proto.const_ref_captures()));
  result->setOwnedCaptureNames(toStringVector(proto.owned_captures()));

  if (proto.has_location()) {
    result->setLocation(deserializePosition(proto.location()));
  }

  result->setCVariadic(proto.c_variadic());
  result->setConstMethod(proto.is_const_method());
  result->setUnsafeMethod(proto.is_unsafe_method());
  if (proto.has_link_name()) {
    result->setLinkName(proto.link_name());
  }
  result->setDoc(proto.doc());

  return result;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserialize(
    const pbc::ASTNode& node) const {
  std::unique_ptr<ExprAST> result;

  switch (node.node_case()) {
    case pbc::ASTNode::kNumberExpr:
      result = deserializeNumber(node.number_expr());
      break;
    case pbc::ASTNode::kCharLiteral:
      result = deserializeCharLiteral(node.char_literal());
      break;
    case pbc::ASTNode::kStringLiteral:
      result = deserializeString(node.string_literal());
      break;
    case pbc::ASTNode::kNullLiteral:
      result = std::make_unique<sun::ast::NullLiteralAST>();
      break;
    case pbc::ASTNode::kBoolLiteral:
      result = deserializeBool(node.bool_literal());
      break;
    case pbc::ASTNode::kArrayLiteral:
      result = deserializeArray(node.array_literal());
      break;
    case pbc::ASTNode::kStructLiteral:
      result = deserializeStructLiteral(node.struct_literal());
      break;
    case pbc::ASTNode::kSliceExpr:
      result = deserializeSlice(node.slice_expr());
      break;
    case pbc::ASTNode::kIndexExpr:
      result = deserializeIndex(node.index_expr());
      break;
    case pbc::ASTNode::kArrayIndexExpr:
      result = deserializeArrayIndex(node.array_index_expr());
      break;
    case pbc::ASTNode::kVariableReference:
      result = deserializeVariableRef(node.variable_reference());
      break;
    case pbc::ASTNode::kVariableCreation:
      result = deserializeVariableCreation(node.variable_creation());
      break;
    case pbc::ASTNode::kVariableAssignment:
      result = deserializeVariableAssignment(node.variable_assignment());
      break;
    case pbc::ASTNode::kReferenceCreation:
      result = deserializeReferenceCreation(node.reference_creation());
      break;
    case pbc::ASTNode::kIndexedAssignment:
      result = deserializeIndexedAssignment(node.indexed_assignment());
      break;
    case pbc::ASTNode::kCompoundAssignment:
      result = deserializeCompoundAssignment(node.compound_assignment());
      break;
    case pbc::ASTNode::kMemberAssignment:
      result = deserializeMemberAssignment(node.member_assignment());
      break;
    case pbc::ASTNode::kBinaryExpr:
      result = deserializeBinary(node.binary_expr());
      break;
    case pbc::ASTNode::kTernaryExpr:
      result = deserializeTernary(node.ternary_expr());
      break;
    case pbc::ASTNode::kParenExpr:
      result = deserializeParen(node.paren_expr());
      break;
    case pbc::ASTNode::kInterpolatedString:
      result = deserializeInterpolatedString(node.interpolated_string());
      break;
    case pbc::ASTNode::kUnaryExpr:
      result = deserializeUnary(node.unary_expr());
      break;
    case pbc::ASTNode::kPackExpansion:
      result = deserializePackExpansion(node.pack_expansion());
      break;
    case pbc::ASTNode::kBlockExpr:
      result = deserializeBlock(node.block_expr());
      break;
    case pbc::ASTNode::kIfExpr:
      result = deserializeIf(node.if_expr());
      break;
    case pbc::ASTNode::kMatchExpr:
      result = deserializeMatch(node.match_expr());
      break;
    case pbc::ASTNode::kForExpr:
      result = deserializeFor(node.for_expr());
      break;
    case pbc::ASTNode::kForInExpr:
      result = deserializeForIn(node.for_in_expr());
      break;
    case pbc::ASTNode::kWhileExpr:
      result = deserializeWhile(node.while_expr());
      break;
    case pbc::ASTNode::kBreakStmt:
      result = std::make_unique<sun::ast::BreakAST>();
      break;
    case pbc::ASTNode::kContinueStmt:
      result = std::make_unique<sun::ast::ContinueAST>();
      break;
    case pbc::ASTNode::kReturnExpr:
      result = deserializeReturn(node.return_expr());
      break;
    case pbc::ASTNode::kUnsafeBlock:
      result = deserializeUnsafeBlock(node.unsafe_block());
      break;
    case pbc::ASTNode::kFunctionDef:
      result = deserializeFunction(node.function_def());
      break;
    case pbc::ASTNode::kLambdaExpr:
      result = deserializeLambda(node.lambda_expr());
      break;
    case pbc::ASTNode::kCallExpr:
      result = deserializeCall(node.call_expr());
      break;
    case pbc::ASTNode::kGenericCallExpr:
      result = deserializeGenericCall(node.generic_call_expr());
      break;
    case pbc::ASTNode::kModuleDef:
      result = deserializeModule(node.module_def());
      break;
    case pbc::ASTNode::kManifest:
      result = deserializeManifest(node.manifest());
      break;
    case pbc::ASTNode::kUsingStmt:
      result = deserializeUsing(node.using_stmt());
      break;
    case pbc::ASTNode::kQualifiedName:
      result = deserializeQualifiedName(node.qualified_name());
      break;
    case pbc::ASTNode::kClassDef:
      result = deserializeClassDef(node.class_def());
      break;
    case pbc::ASTNode::kInterfaceDef:
      result = deserializeInterfaceDef(node.interface_def());
      break;
    case pbc::ASTNode::kEnumDef:
      result = deserializeEnumDef(node.enum_def());
      break;
    case pbc::ASTNode::kThisExpr:
      result = std::make_unique<sun::ast::ThisExprAST>();
      break;
    case pbc::ASTNode::kMemberAccess:
      result = deserializeMemberAccess(node.member_access());
      break;
    case pbc::ASTNode::kTryCatch:
      result = deserializeTryCatch(node.try_catch());
      break;
    case pbc::ASTNode::kThrowExpr:
      result = deserializeThrow(node.throw_expr());
      break;
    case pbc::ASTNode::kDeclareType:
      result = deserializeDeclareType(node.declare_type());
      break;
    default:
      return nullptr;
  }

  if (result) {
    deserializeExprBase(node, result.get());
  }

  return result;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeFromString(
    const std::string& data) const {
  pbc::ASTNode node;
  if (!node.ParseFromString(data)) {
    return nullptr;
  }
  return deserialize(node);
}

std::unique_ptr<BlockExprAST> ASTDeserializer::deserializeProgramFromString(
    const std::string& data) const {
  pbc::Program program;
  if (!program.ParseFromString(data)) {
    return nullptr;
  }
  return deserializeProgram(program);
}

// =============================================================================
// Individual node deserializers
// =============================================================================

std::unique_ptr<BlockExprAST> ASTDeserializer::deserializeBlockExpr(
    const pbc::BlockExpr& proto) const {
  std::vector<std::unique_ptr<ExprAST>> body;
  for (const auto& stmt : proto.body()) {
    body.push_back(deserialize(stmt));
  }
  auto block = std::make_unique<BlockExprAST>(std::move(body));
  block->setSourceFileId(proto.source_file_id());
  if (proto.has_block_kind()) {
    block->setKind(static_cast<sun::ast::BlockKind>(proto.block_kind()));
  }
  if (proto.has_location()) {
    block->setLocation(deserializePosition(proto.location()));
  }
  return block;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeNumber(
    const pbc::NumberExpr& proto) const {
  if (proto.has_int_magnitude()) {
    return std::make_unique<NumberExprAST>(proto.int_magnitude(),
                                           proto.int_negative()
                                               ? NumberExprAST::Sign::Negative
                                               : NumberExprAST::Sign::Positive,
                                           proto.suffix());
  }
  return std::make_unique<NumberExprAST>(proto.float_value(), proto.suffix());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeCharLiteral(
    const pbc::CharLiteral& proto) const {
  return std::make_unique<sun::ast::CharLiteralAST>(proto.value(),
                                                    proto.is_byte());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeString(
    const pbc::StringLiteral& proto) const {
  return std::make_unique<sun::ast::StringLiteralAST>(proto.value());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeBool(
    const pbc::BoolLiteral& proto) const {
  return std::make_unique<sun::ast::BoolLiteralAST>(proto.value());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeStructLiteral(
    const pbc::StructLiteral& proto) const {
  std::vector<sun::ast::StructLiteralAST::FieldInit> fields;
  fields.reserve(proto.fields().size());
  for (const auto& field : proto.fields()) {
    sun::ast::StructLiteralAST::FieldInit init;
    init.name = field.name();
    init.value = deserialize(field.value());
    if (field.has_location()) {
      init.location = deserializePosition(field.location());
    }
    fields.push_back(std::move(init));
  }
  return std::make_unique<StructLiteralAST>(std::move(fields));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeArray(
    const pbc::ArrayLiteral& proto) const {
  std::vector<std::unique_ptr<ExprAST>> elements;
  for (const auto& elem : proto.elements()) {
    elements.push_back(deserialize(elem));
  }
  return std::make_unique<sun::ast::ArrayLiteralAST>(std::move(elements));
}

std::unique_ptr<SliceExprAST> ASTDeserializer::deserializeSliceExpr(
    const pbc::SliceExpr& proto) const {
  std::unique_ptr<ExprAST> start;
  std::unique_ptr<ExprAST> end;
  if (proto.has_start()) {
    start = deserialize(proto.start());
  }
  if (proto.has_end()) {
    end = deserialize(proto.end());
  }
  return std::make_unique<SliceExprAST>(std::move(start), std::move(end),
                                        proto.is_range());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeSlice(
    const pbc::SliceExpr& proto) const {
  return deserializeSliceExpr(proto);
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeIndex(
    const pbc::IndexExpr& proto) const {
  auto target = deserialize(proto.target());
  std::vector<std::unique_ptr<SliceExprAST>> indices;
  for (const auto& idx : proto.indices()) {
    indices.push_back(deserializeSliceExpr(idx));
  }
  return std::make_unique<sun::ast::IndexAST>(std::move(target),
                                              std::move(indices));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeArrayIndex(
    const pbc::ArrayIndexExpr& proto) const {
  auto array = deserialize(proto.array());
  std::vector<std::unique_ptr<ExprAST>> indices;
  for (const auto& idx : proto.indices()) {
    indices.push_back(deserialize(idx));
  }
  return std::make_unique<sun::ast::ArrayIndexAST>(std::move(array),
                                                   std::move(indices));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeVariableRef(
    const pbc::VariableReference& proto) const {
  return std::make_unique<sun::ast::VariableReferenceAST>(proto.name());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeVariableCreation(
    const pbc::VariableCreation& proto) const {
  std::unique_ptr<ExprAST> value;
  if (proto.has_value()) {
    value = deserialize(proto.value());
  }
  std::optional<sun::ast::TypeAnnotation> typeAnnotation;
  if (proto.has_type_annotation()) {
    typeAnnotation = deserializeTypeAnnotation(proto.type_annotation());
  }
  auto var = std::make_unique<sun::ast::VariableCreationAST>(
      proto.name(), std::move(value), std::move(typeAnnotation),
      proto.is_const());
  var->setCExtern(proto.is_c_extern());
  var->setExplicitCAbi(proto.explicit_c_abi());
  if (proto.has_link_name()) var->setLinkName(proto.link_name());
  var->setVisibility(fromProto(proto.visibility()));
  var->setDoc(proto.doc());
  if (proto.has_declaration_identity())
    deserializeIdentity(proto.declaration_identity(),
                        var->declarationIdentity());
  return var;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeVariableAssignment(
    const pbc::VariableAssignment& proto) const {
  return std::make_unique<sun::ast::VariableAssignmentAST>(
      proto.name(), deserialize(proto.value()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeReferenceCreation(
    const pbc::ReferenceCreation& proto) const {
  return std::make_unique<sun::ast::ReferenceCreationAST>(
      proto.name(), deserialize(proto.target()), proto.is_mutable());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeIndexedAssignment(
    const pbc::IndexedAssignment& proto) const {
  return std::make_unique<sun::ast::IndexedAssignmentAST>(
      deserialize(proto.target()), deserialize(proto.value()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeMemberAssignment(
    const pbc::MemberAssignment& proto) const {
  return std::make_unique<sun::ast::MemberAssignmentAST>(
      deserialize(proto.object()), proto.member_name(),
      deserialize(proto.value()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeCompoundAssignment(
    const pbc::CompoundAssignment& proto) const {
  return std::make_unique<sun::ast::CompoundAssignmentAST>(
      deserialize(proto.target()), deserializeToken(proto.op()),
      deserialize(proto.value()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeBinary(
    const pbc::BinaryExpr& proto) const {
  Token token = deserializeToken(proto.op());
  return std::make_unique<sun::ast::BinaryExprAST>(
      token, deserialize(proto.lhs()), deserialize(proto.rhs()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeTernary(
    const pbc::TernaryExpr& proto) const {
  return std::make_unique<sun::ast::TernaryExprAST>(
      deserialize(proto.cond()), deserialize(proto.then_expr()),
      deserialize(proto.else_expr()), Position{});
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeParen(
    const pbc::ParenExpr& proto) const {
  return std::make_unique<sun::ast::ParenExprAST>(deserialize(proto.inner()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeInterpolatedString(
    const pbc::InterpolatedString& proto) const {
  std::vector<sun::ast::InterpolatedStringAST::Segment> segments;
  for (const auto& seg : proto.segments()) {
    sun::ast::InterpolatedStringAST::Segment segment;
    segment.isLiteral = seg.is_literal();
    segment.rawText = seg.raw_text();
    segment.cookedText = seg.cooked_text();
    segment.sourceOffset = seg.source_offset();
    if (seg.has_expression()) {
      segment.expression = deserialize(seg.expression());
    }
    segments.push_back(std::move(segment));
  }
  return std::make_unique<InterpolatedStringAST>(proto.raw_content(),
                                                 std::move(segments));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeUnary(
    const pbc::UnaryExpr& proto) const {
  Token token = deserializeToken(proto.op());
  return std::make_unique<sun::ast::UnaryExprAST>(token,
                                                  deserialize(proto.operand()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializePackExpansion(
    const pbc::PackExpansion& proto) const {
  return std::make_unique<sun::ast::PackExpansionAST>(proto.pack_name());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeBlock(
    const pbc::BlockExpr& proto) const {
  return deserializeBlockExpr(proto);
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeIf(
    const pbc::IfExpr& proto) const {
  auto cond = deserialize(proto.condition());
  auto then = deserialize(proto.then_branch());
  std::unique_ptr<ExprAST> elseExpr;
  if (proto.has_else_branch()) {
    elseExpr = deserialize(proto.else_branch());
  }
  return std::make_unique<sun::ast::IfExprAST>(std::move(cond), std::move(then),
                                               std::move(elseExpr));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeMatch(
    const pbc::MatchExpr& proto) const {
  auto discriminant = deserialize(proto.discriminant());
  std::vector<sun::ast::MatchArm> arms;
  for (const auto& armProto : proto.arms()) {
    std::unique_ptr<ExprAST> pattern;
    if (armProto.has_pattern()) {
      pattern = deserialize(armProto.pattern());
    }
    auto body = deserialize(armProto.body());
    arms.emplace_back(std::move(pattern), armProto.is_wildcard(),
                      std::move(body));
    arms.back().hasPayloadParens = armProto.has_payload_parens();
    for (const auto& bindingProto : armProto.bindings()) {
      sun::ast::PatternBinding binding;
      if (bindingProto.has_declaration_identity())
        deserializeIdentity(bindingProto.declaration_identity(),
                            binding.declaration);
      binding.name = bindingProto.name();
      binding.isWildcard = bindingProto.is_wildcard();
      if (bindingProto.has_location()) {
        binding.location = deserializePosition(bindingProto.location());
      }
      arms.back().bindings.push_back(std::move(binding));
    }
  }
  return std::make_unique<sun::ast::MatchExprAST>(std::move(discriminant),
                                                  std::move(arms));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeFor(
    const pbc::ForExpr& proto) const {
  std::unique_ptr<ExprAST> init;
  std::unique_ptr<ExprAST> cond;
  std::unique_ptr<ExprAST> incr;
  if (proto.has_init()) {
    init = deserialize(proto.init());
  }
  if (proto.has_condition()) {
    cond = deserialize(proto.condition());
  }
  if (proto.has_increment()) {
    incr = deserialize(proto.increment());
  }
  auto body = deserialize(proto.body());
  return std::make_unique<sun::ast::ForExprAST>(
      std::move(init), std::move(cond), std::move(incr), std::move(body));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeForIn(
    const pbc::ForInExpr& proto) const {
  auto type = deserializeTypeAnnotation(proto.loop_var_type());
  auto iterable = deserialize(proto.iterable());
  auto body = deserialize(proto.body());
  return std::make_unique<sun::ast::ForInExprAST>(
      proto.loop_var(), std::move(type), std::move(iterable), std::move(body),
      proto.is_const());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeWhile(
    const pbc::WhileExpr& proto) const {
  return std::make_unique<sun::ast::WhileExprAST>(
      deserialize(proto.condition()), deserialize(proto.body()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeReturn(
    const pbc::ReturnExpr& proto) const {
  std::unique_ptr<ExprAST> value;
  if (proto.has_value()) {
    value = deserialize(proto.value());
  }
  return std::make_unique<sun::ast::ReturnExprAST>(std::move(value));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeUnsafeBlock(
    const pbc::UnsafeBlock& proto) const {
  auto body = deserializeBlockExpr(proto.body());
  return std::make_unique<sun::ast::UnsafeBlockAST>(std::move(body),
                                                    proto.expression_form());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeFunction(
    const pbc::FunctionDef& proto) const {
  auto prototype = deserializePrototype(proto.proto());
  // A declaration has no body at all. Handing FunctionAST an empty block
  // instead would make isExtern() false, so a C extern would be emitted as
  // an ordinary Sun function and its C symbol lost.
  std::unique_ptr<BlockExprAST> body;
  if (proto.body_present()) {
    body = deserializeBlockExpr(proto.body());
  }
  auto func =
      std::make_unique<FunctionAST>(std::move(prototype), std::move(body));
  func->setCExtern(proto.is_c_extern());
  func->setIsTest(proto.is_test());
  func->setFieldInitializerCount(proto.field_initializer_count());
  func->setSynthesizedConstructor(proto.synthesized_constructor());
  func->setVisibility(fromProto(proto.visibility()));
  return func;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeLambda(
    const pbc::LambdaExpr& proto) const {
  auto prototype = deserializePrototype(proto.proto());
  auto body = deserializeBlockExpr(proto.body());
  return std::make_unique<sun::ast::LambdaAST>(std::move(prototype),
                                               std::move(body));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeCall(
    const pbc::CallExpr& proto) const {
  auto callee = deserialize(proto.callee());
  std::vector<std::unique_ptr<ExprAST>> args;
  for (const auto& arg : proto.args()) {
    args.push_back(deserialize(arg));
  }
  return std::make_unique<sun::ast::CallExprAST>(std::move(callee),
                                                 std::move(args));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeGenericCall(
    const pbc::GenericCallExpr& proto) const {
  std::vector<std::unique_ptr<sun::ast::TypeAnnotation>> typeArgs;
  for (const auto& typeArg : proto.type_arguments()) {
    typeArgs.push_back(std::make_unique<sun::ast::TypeAnnotation>(
        deserializeTypeAnnotation(typeArg)));
  }
  std::vector<std::unique_ptr<ExprAST>> args;
  for (const auto& arg : proto.args()) {
    args.push_back(deserialize(arg));
  }
  return std::make_unique<sun::ast::GenericCallAST>(
      proto.function_name(), std::move(typeArgs), std::move(args));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeManifest(
    const pbc::Manifest& proto) const {
  std::vector<sun::ast::ManifestSunDependency> suns;
  for (const auto& sunProto : proto.suns()) {
    sun::ast::ManifestSunDependency sun;
    sun.path = sunProto.path();

    if (sunProto.has_hash()) {
      sun.hash = sunProto.hash();
    }

    suns.push_back(std::move(sun));
  }

  std::vector<sun::ast::ManifestMoonDependency> moons;
  for (const auto& moonProto : proto.moons()) {
    sun::ast::ManifestMoonDependency moon;
    moon.path = moonProto.path();

    if (moonProto.has_url()) {
      moon.url = moonProto.url();
    }

    if (moonProto.has_hash()) {
      moon.hash = moonProto.hash();
    }

    if (moonProto.has_rename_module()) {
      moon.rename = moonProto.rename_module();
    }

    moons.push_back(std::move(moon));
  }

  std::vector<sun::ast::ManifestProtoDependency> protos;
  for (const auto& protoDep : proto.protos()) {
    sun::ast::ManifestProtoDependency dep;
    dep.path = protoDep.path();
    protos.push_back(std::move(dep));
  }

  return std::make_unique<sun::ast::ManifestAST>(
      std::move(suns), std::move(moons), std::move(protos));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeModule(
    const pbc::ModuleDef& proto) const {
  auto body = deserializeBlockExpr(proto.body());
  auto mod =
      std::make_unique<sun::ast::ModuleAST>(proto.name(), std::move(body));
  mod->setVisibility(fromProto(proto.visibility()));
  mod->setDoc(proto.doc());
  if (proto.has_name_location())
    mod->setNameLocation(deserializePosition(proto.name_location()));
  if (proto.has_declaration_identity())
    deserializeIdentity(proto.declaration_identity(),
                        mod->declarationIdentity());
  return mod;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeUsing(
    const pbc::UsingStmt& proto) const {
  auto result = std::make_unique<sun::ast::UsingAST>(
      toStringVector(proto.namespace_path()), proto.target());
  result->setModuleImport(proto.is_module_import());
  return result;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeQualifiedName(
    const pbc::QualifiedNameExpr& proto) const {
  return std::make_unique<sun::ast::QualifiedNameAST>(
      toStringVector(proto.parts()));
}

std::unique_ptr<FunctionAST> ASTDeserializer::deserializeMethodFunction(
    const pbc::FunctionDef& proto, bool emptyBodyMeansNone) const {
  auto prototype = deserializePrototype(proto.proto());
  std::unique_ptr<BlockExprAST> body;
  if (!emptyBodyMeansNone || proto.body().body_size() > 0) {
    body = deserializeBlockExpr(proto.body());
  }
  auto function =
      std::make_unique<FunctionAST>(std::move(prototype), std::move(body));
  function->setFieldInitializerCount(proto.field_initializer_count());
  function->setSynthesizedConstructor(proto.synthesized_constructor());
  function->setSourceFileId(proto.source_file_id());
  function->setVisibility(fromProto(proto.visibility()));
  if (proto.has_location()) {
    function->setLocation(deserializePosition(proto.location()));
  }
  return function;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeClassDef(
    const pbc::ClassDef& proto) const {
  std::vector<sun::ast::ImplementedInterfaceAST> interfaces;
  for (const auto& ifaceProto : proto.implemented_interfaces()) {
    sun::ast::ImplementedInterfaceAST iface;
    iface.name = ifaceProto.name();
    if (ifaceProto.has_declaration_key())
      iface.declarationKey =
          PortableDeclarationKey::parseOriginal(ifaceProto.declaration_key());
    for (const auto& typeArg : ifaceProto.type_arguments()) {
      iface.typeArguments.push_back(deserializeTypeAnnotation(typeArg));
    }
    interfaces.push_back(std::move(iface));
  }

  std::vector<sun::ast::ClassFieldDecl> fields;
  for (const auto& fieldProto : proto.fields()) {
    auto field = deserializeField<sun::ast::ClassFieldDecl>(fieldProto);
    if (fieldProto.has_initializer())
      field.initializer = deserialize(fieldProto.initializer());
    fields.push_back(std::move(field));
  }

  std::vector<sun::ast::ClassMethodDecl> methods;
  for (const auto& methodProto : proto.methods()) {
    sun::ast::ClassMethodDecl method;
    method.function = deserializeMethodFunction(methodProto.function(),
                                                /*emptyBodyMeansNone=*/false);
    method.isConstructor = methodProto.is_constructor();
    method.isConst = methodProto.is_const();
    methods.push_back(std::move(method));
  }

  // Note: the 6th ctor parameter is `precompiled`, not `isPartial` - set the
  // class modifiers explicitly so they actually round-trip. Packing in
  // particular changes layout, so losing it would silently corrupt memory;
  // losing visibility would make every bundled item private.
  auto classDef = std::make_unique<sun::ast::ClassDefinitionAST>(
      proto.name(), deserializeTypeParameters(proto), std::move(interfaces),
      std::move(fields), std::move(methods));
  classDef->setLifetimeParameters(toLifetimeParameters(proto));
  for (const auto& name : proto.compiled_specializations()) {
    classDef->addCompiledSpecialization(name.declaration_key());
  }
  classDef->setIsPartial(proto.is_partial());
  classDef->setIsPacked(proto.is_packed());
  classDef->setVisibility(fromProto(proto.visibility()));
  classDef->setDoc(proto.doc());
  if (proto.has_declaration_identity())
    deserializeIdentity(proto.declaration_identity(),
                        classDef->declarationIdentity());
  return classDef;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeInterfaceDef(
    const pbc::InterfaceDef& proto) const {
  std::vector<sun::ast::InterfaceFieldDecl> fields;
  for (const auto& fieldProto : proto.fields()) {
    fields.push_back(
        deserializeField<sun::ast::InterfaceFieldDecl>(fieldProto));
  }

  std::vector<sun::ast::InterfaceMethodDecl> methods;
  for (const auto& methodProto : proto.methods()) {
    sun::ast::InterfaceMethodDecl method;
    method.function = deserializeMethodFunction(methodProto.function(),
                                                /*emptyBodyMeansNone=*/true);
    method.hasDefaultImpl = methodProto.has_default_impl();
    method.isConst = methodProto.is_const();
    methods.push_back(std::move(method));
  }

  auto iface = std::make_unique<sun::ast::InterfaceDefinitionAST>(
      proto.name(), deserializeTypeParameters(proto), std::move(fields),
      std::move(methods));
  iface->setLifetimeParameters(toLifetimeParameters(proto));
  iface->setVisibility(fromProto(proto.visibility()));
  iface->setDoc(proto.doc());
  if (proto.has_declaration_identity())
    deserializeIdentity(proto.declaration_identity(),
                        iface->declarationIdentity());
  return iface;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeEnumDef(
    const pbc::EnumDef& proto) const {
  std::vector<sun::ast::EnumVariantDecl> variants;
  for (const auto& variantProto : proto.variants()) {
    sun::ast::EnumVariantDecl variant;
    if (variantProto.has_declaration_identity())
      deserializeIdentity(variantProto.declaration_identity(),
                          variant.declaration);
    variant.name = variantProto.name();
    variant.value = variantProto.value();
    variant.hasExplicitValue = variantProto.has_explicit_value();
    if (variantProto.has_location()) {
      variant.location = deserializePosition(variantProto.location());
    }
    variant.doc = variantProto.doc();
    for (const auto& payloadProto : variantProto.payload_types()) {
      variant.payloadTypes.push_back(deserializeTypeAnnotation(payloadProto));
    }
    variants.push_back(std::move(variant));
  }
  auto enumDef = std::make_unique<sun::ast::EnumDefinitionAST>(
      proto.name(), std::move(variants), /*precompiled=*/false,
      deserializeTypeParameters(proto), proto.underlying_type());
  enumDef->setVisibility(fromProto(proto.visibility()));
  enumDef->setDoc(proto.doc());
  if (proto.has_declaration_identity())
    deserializeIdentity(proto.declaration_identity(),
                        enumDef->declarationIdentity());
  return enumDef;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeMemberAccess(
    const pbc::MemberAccess& proto) const {
  auto object = deserialize(proto.object());
  std::vector<std::unique_ptr<sun::ast::TypeAnnotation>> typeArgs;
  for (const auto& typeArg : proto.type_arguments()) {
    typeArgs.push_back(std::make_unique<sun::ast::TypeAnnotation>(
        deserializeTypeAnnotation(typeArg)));
  }
  return std::make_unique<sun::ast::MemberAccessAST>(
      std::move(object), proto.member_name(), std::move(typeArgs));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeTryCatch(
    const pbc::TryCatch& proto) const {
  auto tryBlock = deserializeBlockExpr(proto.try_block());

  std::vector<sun::ast::CatchClause> catchClauses;
  for (const auto& cc : proto.catch_clauses()) {
    sun::ast::CatchClause catchClause;
    if (cc.has_declaration_identity())
      deserializeIdentity(cc.declaration_identity(), catchClause.declaration);
    catchClause.bindingName = cc.binding_name();
    if (cc.has_binding_type()) {
      catchClause.bindingType = deserializeTypeAnnotation(cc.binding_type());
    }
    catchClause.body = deserializeBlockExpr(cc.body());
    catchClauses.push_back(std::move(catchClause));
  }

  return std::make_unique<sun::ast::TryCatchExprAST>(std::move(tryBlock),
                                                     std::move(catchClauses));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeThrow(
    const pbc::ThrowExpr& proto) const {
  return std::make_unique<sun::ast::ThrowExprAST>(
      deserialize(proto.error_expr()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeDeclareType(
    const pbc::DeclareType& proto) const {
  std::optional<std::string> aliasName;
  if (proto.has_alias_name()) {
    aliasName = proto.alias_name();
  }
  auto typeAnnotation = deserializeTypeAnnotation(proto.type_annotation());
  // Note: DeclareTypeAST constructor takes type first, then alias
  auto decl = std::make_unique<sun::ast::DeclareTypeAST>(
      std::move(typeAnnotation), std::move(aliasName));
  decl->setVisibility(fromProto(proto.visibility()));
  return decl;
}

}  // namespace sun::serialization
