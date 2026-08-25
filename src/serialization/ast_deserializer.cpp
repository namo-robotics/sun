// ast_deserializer.cpp — Implementation of protobuf to AST deserialization

#include "serialization/ast_deserializer.h"

#include "ast.h"
#include "ast.pb.h"
#include "serialization/token_kind_proto_map.h"
#include "types.pb.h"

namespace sun {
namespace serialization {

static sun::Visibility fromProto(ast::Visibility v) {
  return v == ast::PUBLIC ? sun::Visibility::Public : sun::Visibility::Private;
}

Position ASTDeserializer::deserializePosition(const ast::Position& pos) const {
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

Token ASTDeserializer::deserializeToken(const ast::Token& token) const {
  Token result;
  result.kind = fromProtoTokenKind(token.kind());
  result.text = token.text();
  return result;
}

TypeAnnotation ASTDeserializer::deserializeTypeAnnotation(
    const ast::TypeAnnotation& type) const {
  TypeAnnotation result;
  result.baseName = type.base_name();

  if (type.has_element_type()) {
    result.elementType = std::make_unique<TypeAnnotation>(
        deserializeTypeAnnotation(type.element_type()));
  }

  for (const auto& param : type.param_types()) {
    result.paramTypes.push_back(
        std::make_unique<TypeAnnotation>(deserializeTypeAnnotation(param)));
  }

  if (type.has_return_type()) {
    result.returnType = std::make_unique<TypeAnnotation>(
        deserializeTypeAnnotation(type.return_type()));
  }

  for (const auto& arg : type.type_arguments()) {
    result.typeArguments.push_back(
        std::make_unique<TypeAnnotation>(deserializeTypeAnnotation(arg)));
  }

  for (auto dim : type.array_dimensions()) {
    result.arrayDimensions.push_back(dim);
  }

  result.canError = type.can_error();
  result.constRef = type.const_ref();
  return result;
}

Capture ASTDeserializer::deserializeCapture(const ast::Capture& cap) const {
  Capture result;
  result.name = cap.name();
  result.isConst = cap.is_const();
  switch (cap.kind()) {
    case ast::CAPTURE_OWNED:
      result.kind = CaptureKind::Owned;
      break;
    case ast::CAPTURE_BORROW:
      result.kind = CaptureKind::Borrow;
      break;
    default:
      result.kind = CaptureKind::ByValue;
      break;
  }
  // Note: type is stored as string signature, not reconstructed as TypePtr
  // The semantic analyzer will need to re-resolve the type
  return result;
}

void ASTDeserializer::deserializeExprBase(const ast::ASTNode& node,
                                          ExprAST* expr) const {
  if (node.has_location()) {
    expr->setLocation(deserializePosition(node.location()));
  }
  expr->setPrecompiled(node.precompiled());
  expr->setSkipCodegen(node.skip_codegen());
  expr->setSymbolPrefix(node.symbol_prefix());

  // Note: Analysis data restoration is deferred - the semantic analyzer
  // should re-analyze rather than rely on serialized analysis state
}

std::unique_ptr<BlockExprAST> ASTDeserializer::deserializeProgram(
    const ast::Program& program) const {
  return deserializeBlockExpr(program.body());
}

std::unique_ptr<PrototypeAST> ASTDeserializer::deserializePrototype(
    const ast::Prototype& proto) const {
  std::vector<std::pair<std::string, TypeAnnotation>> args;
  for (const auto& arg : proto.args()) {
    args.emplace_back(arg.name(), deserializeTypeAnnotation(arg.type()));
  }

  std::optional<TypeAnnotation> returnType;
  if (proto.has_return_type()) {
    returnType = deserializeTypeAnnotation(proto.return_type());
  }

  std::vector<std::string> typeParams;
  for (const auto& tp : proto.type_parameters()) {
    typeParams.push_back(tp);
  }

  std::optional<std::string> variadicParam;
  if (proto.has_variadic_param_name()) {
    variadicParam = proto.variadic_param_name();
  }

  std::optional<TypeAnnotation> variadicConstraint;
  if (proto.has_variadic_constraint()) {
    variadicConstraint = deserializeTypeAnnotation(proto.variadic_constraint());
  }

  auto result = std::make_unique<PrototypeAST>(
      proto.name(), std::move(args), std::move(returnType),
      std::move(typeParams), std::move(variadicParam),
      std::move(variadicConstraint));

  // Restore captures
  std::vector<Capture> captures;
  for (const auto& cap : proto.captures()) {
    captures.push_back(deserializeCapture(cap));
  }
  result->setCaptures(captures);

  // Restore the declared [ref x, ...] capture list
  std::vector<std::string> refCaptureNames;
  for (const auto& refName : proto.ref_captures()) {
    refCaptureNames.push_back(refName);
  }
  result->setRefCaptureNames(std::move(refCaptureNames));

  std::vector<std::string> constRefCaptureNames;
  for (const auto& refName : proto.const_ref_captures()) {
    constRefCaptureNames.push_back(refName);
  }
  result->setConstRefCaptureNames(std::move(constRefCaptureNames));

  std::vector<std::string> ownedCaptureNames;
  for (const auto& ownedName : proto.owned_captures()) {
    ownedCaptureNames.push_back(ownedName);
  }
  result->setOwnedCaptureNames(std::move(ownedCaptureNames));

  if (proto.has_location()) {
    result->setLocation(deserializePosition(proto.location()));
  }

  result->setCVariadic(proto.c_variadic());
  result->setConstMethod(proto.is_const_method());
  if (proto.has_link_name()) {
    result->setLinkName(proto.link_name());
  }
  result->setDoc(proto.doc());

  return result;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserialize(
    const ast::ASTNode& node) const {
  std::unique_ptr<ExprAST> result;

  switch (node.node_case()) {
    case ast::ASTNode::kNumberExpr:
      result = deserializeNumber(node.number_expr());
      break;
    case ast::ASTNode::kCharLiteral:
      result = deserializeCharLiteral(node.char_literal());
      break;
    case ast::ASTNode::kStringLiteral:
      result = deserializeString(node.string_literal());
      break;
    case ast::ASTNode::kNullLiteral:
      result = deserializeNull(node.null_literal());
      break;
    case ast::ASTNode::kBoolLiteral:
      result = deserializeBool(node.bool_literal());
      break;
    case ast::ASTNode::kArrayLiteral:
      result = deserializeArray(node.array_literal());
      break;
    case ast::ASTNode::kStructLiteral:
      result = deserializeStructLiteral(node.struct_literal());
      break;
    case ast::ASTNode::kSliceExpr:
      result = deserializeSlice(node.slice_expr());
      break;
    case ast::ASTNode::kIndexExpr:
      result = deserializeIndex(node.index_expr());
      break;
    case ast::ASTNode::kArrayIndexExpr:
      result = deserializeArrayIndex(node.array_index_expr());
      break;
    case ast::ASTNode::kVariableReference:
      result = deserializeVariableRef(node.variable_reference());
      break;
    case ast::ASTNode::kVariableCreation:
      result = deserializeVariableCreation(node.variable_creation());
      break;
    case ast::ASTNode::kVariableAssignment:
      result = deserializeVariableAssignment(node.variable_assignment());
      break;
    case ast::ASTNode::kReferenceCreation:
      result = deserializeReferenceCreation(node.reference_creation());
      break;
    case ast::ASTNode::kIndexedAssignment:
      result = deserializeIndexedAssignment(node.indexed_assignment());
      break;
    case ast::ASTNode::kCompoundAssignment:
      result = deserializeCompoundAssignment(node.compound_assignment());
      break;
    case ast::ASTNode::kMemberAssignment:
      result = deserializeMemberAssignment(node.member_assignment());
      break;
    case ast::ASTNode::kBinaryExpr:
      result = deserializeBinary(node.binary_expr());
      break;
    case ast::ASTNode::kTernaryExpr:
      result = deserializeTernary(node.ternary_expr());
      break;
    case ast::ASTNode::kParenExpr:
      result = deserializeParen(node.paren_expr());
      break;
    case ast::ASTNode::kInterpolatedString:
      result = deserializeInterpolatedString(node.interpolated_string());
      break;
    case ast::ASTNode::kUnaryExpr:
      result = deserializeUnary(node.unary_expr());
      break;
    case ast::ASTNode::kPackExpansion:
      result = deserializePackExpansion(node.pack_expansion());
      break;
    case ast::ASTNode::kBlockExpr:
      result = deserializeBlock(node.block_expr());
      break;
    case ast::ASTNode::kIfExpr:
      result = deserializeIf(node.if_expr());
      break;
    case ast::ASTNode::kMatchExpr:
      result = deserializeMatch(node.match_expr());
      break;
    case ast::ASTNode::kForExpr:
      result = deserializeFor(node.for_expr());
      break;
    case ast::ASTNode::kForInExpr:
      result = deserializeForIn(node.for_in_expr());
      break;
    case ast::ASTNode::kWhileExpr:
      result = deserializeWhile(node.while_expr());
      break;
    case ast::ASTNode::kBreakStmt:
      result = deserializeBreak(node.break_stmt());
      break;
    case ast::ASTNode::kContinueStmt:
      result = deserializeContinue(node.continue_stmt());
      break;
    case ast::ASTNode::kReturnExpr:
      result = deserializeReturn(node.return_expr());
      break;
    case ast::ASTNode::kUnsafeBlock:
      result = deserializeUnsafeBlock(node.unsafe_block());
      break;
    case ast::ASTNode::kFunctionDef:
      result = deserializeFunction(node.function_def());
      break;
    case ast::ASTNode::kLambdaExpr:
      result = deserializeLambda(node.lambda_expr());
      break;
    case ast::ASTNode::kCallExpr:
      result = deserializeCall(node.call_expr());
      break;
    case ast::ASTNode::kGenericCallExpr:
      result = deserializeGenericCall(node.generic_call_expr());
      break;
    case ast::ASTNode::kSpawnExpr:
      result = deserializeSpawn(node.spawn_expr());
      break;
    case ast::ASTNode::kModuleDef:
      result = deserializeModule(node.module_def());
      break;
    case ast::ASTNode::kManifest:
      result = deserializeManifest(node.manifest());
      break;
    case ast::ASTNode::kUsingStmt:
      result = deserializeUsing(node.using_stmt());
      break;
    case ast::ASTNode::kQualifiedName:
      result = deserializeQualifiedName(node.qualified_name());
      break;
    case ast::ASTNode::kClassDef:
      result = deserializeClassDef(node.class_def());
      break;
    case ast::ASTNode::kInterfaceDef:
      result = deserializeInterfaceDef(node.interface_def());
      break;
    case ast::ASTNode::kEnumDef:
      result = deserializeEnumDef(node.enum_def());
      break;
    case ast::ASTNode::kThisExpr:
      result = deserializeThis(node.this_expr());
      break;
    case ast::ASTNode::kMemberAccess:
      result = deserializeMemberAccess(node.member_access());
      break;
    case ast::ASTNode::kTryCatch:
      result = deserializeTryCatch(node.try_catch());
      break;
    case ast::ASTNode::kThrowExpr:
      result = deserializeThrow(node.throw_expr());
      break;
    case ast::ASTNode::kDeclareType:
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
  ast::ASTNode node;
  if (!node.ParseFromString(data)) {
    return nullptr;
  }
  return deserialize(node);
}

std::unique_ptr<BlockExprAST> ASTDeserializer::deserializeProgramFromString(
    const std::string& data) const {
  ast::Program program;
  if (!program.ParseFromString(data)) {
    return nullptr;
  }
  return deserializeProgram(program);
}

// =============================================================================
// Individual node deserializers
// =============================================================================

std::unique_ptr<BlockExprAST> ASTDeserializer::deserializeBlockExpr(
    const ast::BlockExpr& proto) const {
  std::vector<std::unique_ptr<ExprAST>> body;
  for (const auto& stmt : proto.body()) {
    body.push_back(deserialize(stmt));
  }
  auto block = std::make_unique<BlockExprAST>(std::move(body));
  if (proto.has_location()) {
    block->setLocation(deserializePosition(proto.location()));
  }
  return block;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeNumber(
    const ast::NumberExpr& proto) const {
  if (proto.has_int_value()) {
    return std::make_unique<NumberExprAST>(proto.int_value());
  }
  return std::make_unique<NumberExprAST>(proto.float_value());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeCharLiteral(
    const ast::CharLiteral& proto) const {
  return std::make_unique<CharLiteralAST>(proto.value(), proto.is_byte());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeString(
    const ast::StringLiteral& proto) const {
  return std::make_unique<StringLiteralAST>(proto.value());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeNull(
    const ast::NullLiteral& proto) const {
  return std::make_unique<NullLiteralAST>();
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeBool(
    const ast::BoolLiteral& proto) const {
  return std::make_unique<BoolLiteralAST>(proto.value());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeStructLiteral(
    const ast::StructLiteral& proto) const {
  std::vector<StructLiteralAST::FieldInit> fields;
  fields.reserve(proto.fields().size());
  for (const auto& field : proto.fields()) {
    StructLiteralAST::FieldInit init;
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
    const ast::ArrayLiteral& proto) const {
  std::vector<std::unique_ptr<ExprAST>> elements;
  for (const auto& elem : proto.elements()) {
    elements.push_back(deserialize(elem));
  }
  return std::make_unique<ArrayLiteralAST>(std::move(elements));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeSlice(
    const ast::SliceExpr& proto) const {
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

std::unique_ptr<ExprAST> ASTDeserializer::deserializeIndex(
    const ast::IndexExpr& proto) const {
  auto target = deserialize(proto.target());
  std::vector<std::unique_ptr<SliceExprAST>> indices;
  for (const auto& idx : proto.indices()) {
    std::unique_ptr<ExprAST> start;
    std::unique_ptr<ExprAST> end;
    if (idx.has_start()) {
      start = deserialize(idx.start());
    }
    if (idx.has_end()) {
      end = deserialize(idx.end());
    }
    indices.push_back(std::make_unique<SliceExprAST>(
        std::move(start), std::move(end), idx.is_range()));
  }
  return std::make_unique<IndexAST>(std::move(target), std::move(indices));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeArrayIndex(
    const ast::ArrayIndexExpr& proto) const {
  auto array = deserialize(proto.array());
  std::vector<std::unique_ptr<ExprAST>> indices;
  for (const auto& idx : proto.indices()) {
    indices.push_back(deserialize(idx));
  }
  return std::make_unique<ArrayIndexAST>(std::move(array), std::move(indices));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeVariableRef(
    const ast::VariableReference& proto) const {
  return std::make_unique<VariableReferenceAST>(proto.name());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeVariableCreation(
    const ast::VariableCreation& proto) const {
  std::unique_ptr<ExprAST> value;
  if (proto.has_value()) {
    value = deserialize(proto.value());
  }
  std::optional<TypeAnnotation> typeAnnotation;
  if (proto.has_type_annotation()) {
    typeAnnotation = deserializeTypeAnnotation(proto.type_annotation());
  }
  auto var = std::make_unique<VariableCreationAST>(
      proto.name(), std::move(value), std::move(typeAnnotation),
      proto.is_const());
  var->setVisibility(fromProto(proto.visibility()));
  var->setDoc(proto.doc());
  return var;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeVariableAssignment(
    const ast::VariableAssignment& proto) const {
  return std::make_unique<VariableAssignmentAST>(proto.name(),
                                                 deserialize(proto.value()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeReferenceCreation(
    const ast::ReferenceCreation& proto) const {
  return std::make_unique<ReferenceCreationAST>(
      proto.name(), deserialize(proto.target()), proto.is_mutable());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeIndexedAssignment(
    const ast::IndexedAssignment& proto) const {
  return std::make_unique<IndexedAssignmentAST>(deserialize(proto.target()),
                                                deserialize(proto.value()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeMemberAssignment(
    const ast::MemberAssignment& proto) const {
  return std::make_unique<MemberAssignmentAST>(deserialize(proto.object()),
                                               proto.member_name(),
                                               deserialize(proto.value()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeCompoundAssignment(
    const ast::CompoundAssignment& proto) const {
  return std::make_unique<CompoundAssignmentAST>(deserialize(proto.target()),
                                                 deserializeToken(proto.op()),
                                                 deserialize(proto.value()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeBinary(
    const ast::BinaryExpr& proto) const {
  Token token = deserializeToken(proto.op());
  return std::make_unique<BinaryExprAST>(token, deserialize(proto.lhs()),
                                         deserialize(proto.rhs()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeTernary(
    const ast::TernaryExpr& proto) const {
  return std::make_unique<TernaryExprAST>(
      deserialize(proto.cond()), deserialize(proto.then_expr()),
      deserialize(proto.else_expr()), Position{});
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeParen(
    const ast::ParenExpr& proto) const {
  return std::make_unique<ParenExprAST>(deserialize(proto.inner()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeInterpolatedString(
    const ast::InterpolatedString& proto) const {
  std::vector<InterpolatedStringAST::Segment> segments;
  for (const auto& seg : proto.segments()) {
    InterpolatedStringAST::Segment segment;
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
    const ast::UnaryExpr& proto) const {
  Token token = deserializeToken(proto.op());
  return std::make_unique<UnaryExprAST>(token, deserialize(proto.operand()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializePackExpansion(
    const ast::PackExpansion& proto) const {
  return std::make_unique<PackExpansionAST>(proto.pack_name());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeBlock(
    const ast::BlockExpr& proto) const {
  return deserializeBlockExpr(proto);
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeIf(
    const ast::IfExpr& proto) const {
  auto cond = deserialize(proto.condition());
  auto then = deserialize(proto.then_branch());
  std::unique_ptr<ExprAST> elseExpr;
  if (proto.has_else_branch()) {
    elseExpr = deserialize(proto.else_branch());
  }
  return std::make_unique<IfExprAST>(std::move(cond), std::move(then),
                                     std::move(elseExpr));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeMatch(
    const ast::MatchExpr& proto) const {
  auto discriminant = deserialize(proto.discriminant());
  std::vector<MatchArm> arms;
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
      PatternBinding binding;
      binding.name = bindingProto.name();
      binding.isWildcard = bindingProto.is_wildcard();
      binding.location = deserializePosition(bindingProto.location());
      arms.back().bindings.push_back(std::move(binding));
    }
  }
  return std::make_unique<MatchExprAST>(std::move(discriminant),
                                        std::move(arms));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeFor(
    const ast::ForExpr& proto) const {
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
  return std::make_unique<ForExprAST>(std::move(init), std::move(cond),
                                      std::move(incr), std::move(body));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeForIn(
    const ast::ForInExpr& proto) const {
  auto type = deserializeTypeAnnotation(proto.loop_var_type());
  auto iterable = deserialize(proto.iterable());
  auto body = deserialize(proto.body());
  return std::make_unique<ForInExprAST>(proto.loop_var(), std::move(type),
                                        std::move(iterable), std::move(body),
                                        proto.is_const());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeWhile(
    const ast::WhileExpr& proto) const {
  return std::make_unique<WhileExprAST>(deserialize(proto.condition()),
                                        deserialize(proto.body()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeBreak(
    const ast::BreakStmt& proto) const {
  return std::make_unique<BreakAST>();
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeContinue(
    const ast::ContinueStmt& proto) const {
  return std::make_unique<ContinueAST>();
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeReturn(
    const ast::ReturnExpr& proto) const {
  std::unique_ptr<ExprAST> value;
  if (proto.has_value()) {
    value = deserialize(proto.value());
  }
  return std::make_unique<ReturnExprAST>(std::move(value));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeUnsafeBlock(
    const ast::UnsafeBlock& proto) const {
  auto body = deserializeBlockExpr(proto.body());
  return std::make_unique<UnsafeBlockAST>(std::move(body));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeFunction(
    const ast::FunctionDef& proto) const {
  auto prototype = deserializePrototype(proto.proto());
  // A declaration has no body at all. Handing FunctionAST an empty block
  // instead would make isExtern() false, so a C extern would be re-mangled as
  // an ordinary Sun function and its symbol lost.
  std::unique_ptr<BlockExprAST> body;
  if (proto.body_present()) {
    body = deserializeBlockExpr(proto.body());
  }
  auto func =
      std::make_unique<FunctionAST>(std::move(prototype), std::move(body));
  func->setCExtern(proto.is_c_extern());
  func->setVisibility(fromProto(proto.visibility()));
  return func;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeLambda(
    const ast::LambdaExpr& proto) const {
  auto prototype = deserializePrototype(proto.proto());
  auto body = deserializeBlockExpr(proto.body());
  return std::make_unique<LambdaAST>(std::move(prototype), std::move(body));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeCall(
    const ast::CallExpr& proto) const {
  auto callee = deserialize(proto.callee());
  std::vector<std::unique_ptr<ExprAST>> args;
  for (const auto& arg : proto.args()) {
    args.push_back(deserialize(arg));
  }
  return std::make_unique<CallExprAST>(std::move(callee), std::move(args));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeGenericCall(
    const ast::GenericCallExpr& proto) const {
  std::vector<std::unique_ptr<TypeAnnotation>> typeArgs;
  for (const auto& typeArg : proto.type_arguments()) {
    typeArgs.push_back(
        std::make_unique<TypeAnnotation>(deserializeTypeAnnotation(typeArg)));
  }
  std::vector<std::unique_ptr<ExprAST>> args;
  for (const auto& arg : proto.args()) {
    args.push_back(deserialize(arg));
  }
  return std::make_unique<GenericCallAST>(proto.function_name(),
                                          std::move(typeArgs), std::move(args));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeSpawn(
    const ast::SpawnExpr& proto) const {
  return std::make_unique<SpawnExprAST>(deserialize(proto.lambda()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeManifest(
    const ast::Manifest& proto) const {
  std::vector<ManifestSunDependency> suns;
  for (const auto& sunProto : proto.suns()) {
    ManifestSunDependency sun;
    sun.path = sunProto.path();

    if (sunProto.has_hash()) {
      sun.hash = sunProto.hash();
    }

    suns.push_back(std::move(sun));
  }

  std::vector<ManifestMoonDependency> moons;
  for (const auto& moonProto : proto.moons()) {
    ManifestMoonDependency moon;
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

  std::vector<ManifestProtoDependency> protos;
  for (const auto& protoDep : proto.protos()) {
    ManifestProtoDependency dep;
    dep.path = protoDep.path();
    protos.push_back(std::move(dep));
  }

  return std::make_unique<ManifestAST>(std::move(suns), std::move(moons),
                                       std::move(protos));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeModule(
    const ast::ModuleDef& proto) const {
  auto body = deserializeBlockExpr(proto.body());
  auto mod = std::make_unique<ModuleAST>(proto.name(), std::move(body));
  mod->setVisibility(fromProto(proto.visibility()));
  return mod;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeUsing(
    const ast::UsingStmt& proto) const {
  std::vector<std::string> path;
  for (const auto& part : proto.namespace_path()) {
    path.push_back(part);
  }
  // Note: UsingAST computes isModuleImport_ from target == "*"
  return std::make_unique<UsingAST>(std::move(path), proto.target());
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeQualifiedName(
    const ast::QualifiedName& proto) const {
  std::vector<std::string> parts;
  for (const auto& part : proto.parts()) {
    parts.push_back(part);
  }
  return std::make_unique<QualifiedNameAST>(std::move(parts));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeClassDef(
    const ast::ClassDef& proto) const {
  std::vector<std::string> typeParams;
  for (const auto& tp : proto.type_parameters()) {
    typeParams.push_back(tp);
  }

  std::vector<ImplementedInterfaceAST> interfaces;
  for (const auto& ifaceProto : proto.implemented_interfaces()) {
    ImplementedInterfaceAST iface;
    iface.name = ifaceProto.name();
    for (const auto& typeArg : ifaceProto.type_arguments()) {
      iface.typeArguments.push_back(deserializeTypeAnnotation(typeArg));
    }
    interfaces.push_back(std::move(iface));
  }

  std::vector<ClassFieldDecl> fields;
  for (const auto& fieldProto : proto.fields()) {
    ClassFieldDecl field;
    field.name = fieldProto.name();
    field.type = deserializeTypeAnnotation(fieldProto.type());
    field.location = deserializePosition(fieldProto.location());
    field.visibility = fromProto(fieldProto.visibility());
    field.doc = fieldProto.doc();
    fields.push_back(std::move(field));
  }

  std::vector<ClassMethodDecl> methods;
  for (const auto& methodProto : proto.methods()) {
    ClassMethodDecl method;
    auto funcProto = methodProto.function();
    auto prototype = deserializePrototype(funcProto.proto());
    auto body = deserializeBlockExpr(funcProto.body());
    method.function =
        std::make_unique<FunctionAST>(std::move(prototype), std::move(body));
    method.function->setVisibility(fromProto(funcProto.visibility()));
    if (funcProto.has_location()) {
      method.function->setLocation(deserializePosition(funcProto.location()));
    }
    method.isConstructor = methodProto.is_constructor();
    method.isConst = methodProto.is_const();
    methods.push_back(std::move(method));
  }

  // Note: the 6th ctor parameter is `precompiled`, not `isPartial` - set the
  // class modifiers explicitly so they actually round-trip. Packing in
  // particular changes layout, so losing it would silently corrupt memory;
  // losing visibility would make every bundled item private.
  auto classDef = std::make_unique<ClassDefinitionAST>(
      proto.name(), std::move(typeParams), std::move(interfaces),
      std::move(fields), std::move(methods));
  classDef->setIsPartial(proto.is_partial());
  classDef->setIsPacked(proto.is_packed());
  classDef->setVisibility(fromProto(proto.visibility()));
  classDef->setDoc(proto.doc());
  return classDef;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeInterfaceDef(
    const ast::InterfaceDef& proto) const {
  std::vector<std::string> typeParams;
  for (const auto& tp : proto.type_parameters()) {
    typeParams.push_back(tp);
  }

  std::vector<InterfaceFieldDecl> fields;
  for (const auto& fieldProto : proto.fields()) {
    InterfaceFieldDecl field;
    field.name = fieldProto.name();
    field.type = deserializeTypeAnnotation(fieldProto.type());
    field.location = deserializePosition(fieldProto.location());
    field.visibility = fromProto(fieldProto.visibility());
    field.doc = fieldProto.doc();
    fields.push_back(std::move(field));
  }

  std::vector<InterfaceMethodDecl> methods;
  for (const auto& methodProto : proto.methods()) {
    InterfaceMethodDecl method;
    auto funcProto = methodProto.function();
    auto prototype = deserializePrototype(funcProto.proto());
    std::unique_ptr<BlockExprAST> body;
    if (funcProto.body().body_size() > 0) {
      body = deserializeBlockExpr(funcProto.body());
    }
    method.function =
        std::make_unique<FunctionAST>(std::move(prototype), std::move(body));
    method.function->setVisibility(fromProto(funcProto.visibility()));
    if (funcProto.has_location()) {
      method.function->setLocation(deserializePosition(funcProto.location()));
    }
    method.hasDefaultImpl = methodProto.has_default_impl();
    method.isConst = methodProto.is_const();
    methods.push_back(std::move(method));
  }

  auto iface = std::make_unique<InterfaceDefinitionAST>(
      proto.name(), std::move(typeParams), std::move(fields),
      std::move(methods));
  iface->setVisibility(fromProto(proto.visibility()));
  iface->setDoc(proto.doc());
  return iface;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeEnumDef(
    const ast::EnumDef& proto) const {
  std::vector<EnumVariantDecl> variants;
  for (const auto& variantProto : proto.variants()) {
    EnumVariantDecl variant;
    variant.name = variantProto.name();
    variant.value = variantProto.value();
    variant.location = deserializePosition(variantProto.location());
    variant.doc = variantProto.doc();
    for (const auto& payloadProto : variantProto.payload_types()) {
      variant.payloadTypes.push_back(deserializeTypeAnnotation(payloadProto));
    }
    variants.push_back(std::move(variant));
  }
  std::vector<std::string> typeParams;
  for (const auto& tp : proto.type_parameters()) {
    typeParams.push_back(tp);
  }
  auto enumDef = std::make_unique<EnumDefinitionAST>(
      proto.name(), std::move(variants), /*precompiled=*/false,
      std::move(typeParams));
  enumDef->setVisibility(fromProto(proto.visibility()));
  enumDef->setDoc(proto.doc());
  return enumDef;
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeThis(
    const ast::ThisExpr& proto) const {
  return std::make_unique<ThisExprAST>();
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeMemberAccess(
    const ast::MemberAccess& proto) const {
  auto object = deserialize(proto.object());
  std::vector<std::unique_ptr<TypeAnnotation>> typeArgs;
  for (const auto& typeArg : proto.type_arguments()) {
    typeArgs.push_back(
        std::make_unique<TypeAnnotation>(deserializeTypeAnnotation(typeArg)));
  }
  return std::make_unique<MemberAccessAST>(
      std::move(object), proto.member_name(), std::move(typeArgs));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeTryCatch(
    const ast::TryCatch& proto) const {
  auto tryBlock = deserializeBlockExpr(proto.try_block());

  auto deserializeClause = [&](const ast::CatchClause& cc) {
    CatchClause catchClause;
    catchClause.bindingName = cc.binding_name();
    if (cc.has_binding_type()) {
      catchClause.bindingType = deserializeTypeAnnotation(cc.binding_type());
    }
    catchClause.body = deserializeBlockExpr(cc.body());
    return catchClause;
  };

  std::vector<CatchClause> catchClauses;
  for (const auto& cc : proto.catch_clauses()) {
    catchClauses.push_back(deserializeClause(cc));
  }

  return std::make_unique<TryCatchExprAST>(std::move(tryBlock),
                                           std::move(catchClauses));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeThrow(
    const ast::ThrowExpr& proto) const {
  return std::make_unique<ThrowExprAST>(deserialize(proto.error_expr()));
}

std::unique_ptr<ExprAST> ASTDeserializer::deserializeDeclareType(
    const ast::DeclareType& proto) const {
  std::optional<std::string> aliasName;
  if (proto.has_alias_name()) {
    aliasName = proto.alias_name();
  }
  auto typeAnnotation = deserializeTypeAnnotation(proto.type_annotation());
  // Note: DeclareTypeAST constructor takes type first, then alias
  auto decl = std::make_unique<DeclareTypeAST>(std::move(typeAnnotation),
                                               std::move(aliasName));
  decl->setVisibility(fromProto(proto.visibility()));
  return decl;
}

}  // namespace serialization
}  // namespace sun
