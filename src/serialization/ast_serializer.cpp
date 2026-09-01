// ast_serializer.cpp — Implementation of AST to protobuf serialization

#include "serialization/ast_serializer.h"

#include "ast.h"
#include "ast.pb.h"
#include "serialization/token_kind_proto_map.h"
#include "types.pb.h"

namespace sun {
namespace serialization {

ast::Position ASTSerializer::serializePosition(const Position& pos) const {
  ast::Position proto;
  proto.set_line(pos.line);
  proto.set_column(pos.column);
  proto.set_offset(pos.offset);
  if (pos.filePath) {
    proto.set_file_path(*pos.filePath);
  }
  if (pos.endLine) {
    proto.set_end_line(*pos.endLine);
  }
  if (pos.endColumn) {
    proto.set_end_column(*pos.endColumn);
  }
  if (pos.endOffset) {
    proto.set_end_offset(*pos.endOffset);
  }
  return proto;
}

ast::Token ASTSerializer::serializeToken(const Token& token) const {
  ast::Token proto;
  proto.set_kind(toProtoTokenKind(token.kind));
  proto.set_text(token.text);
  return proto;
}

ast::TypeAnnotation ASTSerializer::serializeTypeAnnotation(
    const TypeAnnotation& type) const {
  ast::TypeAnnotation proto;
  proto.set_base_name(type.baseName);

  if (type.elementType) {
    *proto.mutable_element_type() = serializeTypeAnnotation(*type.elementType);
  }

  for (const auto& param : type.paramTypes) {
    *proto.add_param_types() = serializeTypeAnnotation(*param);
  }

  if (type.returnType) {
    *proto.mutable_return_type() = serializeTypeAnnotation(*type.returnType);
  }

  for (const auto& arg : type.typeArguments) {
    *proto.add_type_arguments() = serializeTypeAnnotation(*arg);
  }

  for (auto dim : type.arrayDimensions) {
    proto.add_array_dimensions(dim);
  }

  proto.set_can_error(type.canError);
  proto.set_const_ref(type.constRef);
  proto.set_ref_env(type.refEnv);
  proto.set_lifetime_name(type.lifetimeName);
  for (const auto& lifetime : type.lifetimeArguments) {
    proto.add_lifetime_arguments(lifetime);
  }
  return proto;
}

ast::Capture ASTSerializer::serializeCapture(const Capture& cap) const {
  ast::Capture proto;
  proto.set_name(cap.name);
  proto.set_is_const(cap.isConst);
  switch (cap.kind) {
    case CaptureKind::Owned:
      proto.set_kind(ast::CAPTURE_OWNED);
      break;
    case CaptureKind::Borrow:
      proto.set_kind(ast::CAPTURE_BORROW);
      break;
    case CaptureKind::ByValue:
      proto.set_kind(ast::CAPTURE_BY_VALUE);
      break;
  }
  return proto;
}

void ASTSerializer::serializeExprBase(const ExprAST& expr,
                                      ast::ASTNode* node) const {
  if (config_.include_location) {
    *node->mutable_location() = serializePosition(expr.getLocation());
  }
  node->set_precompiled(expr.isPrecompiled());
  node->set_skip_codegen(expr.shouldSkipCodegen());
  node->set_symbol_prefix(expr.getSymbolPrefix());
}

ast::Program ASTSerializer::serializeProgram(const BlockExprAST& root) const {
  ast::Program program;
  program.set_version(1);
  serializeBlockInto(root, program.mutable_body());
  return program;
}

void ASTSerializer::serializeBlockInto(const BlockExprAST& block,
                                       ast::BlockExpr* proto) const {
  proto->set_block_kind(static_cast<uint32_t>(block.getKind()));
  for (const auto& stmt : block.getBody()) {
    *proto->add_body() = serialize(*stmt);
  }
  if (config_.include_location && block.getLocation().endOffset) {
    *proto->mutable_location() = serializePosition(block.getLocation());
  }
}

ast::Prototype ASTSerializer::serializePrototype(
    const PrototypeAST& proto) const {
  ast::Prototype result;
  result.set_name(proto.getName());

  for (const auto& tp : proto.getTypeParameters()) {
    auto* out = result.add_type_params();
    out->set_name(tp.name);
    if (tp.constraint) out->set_constraint(tp.constraint->name);
  }

  for (const auto& lp : proto.getLifetimeParameters()) {
    result.add_lifetime_params(lp.name);
  }

  for (const auto& [name, type] : proto.getArgs()) {
    auto* arg = result.add_args();
    arg->set_name(name);
    *arg->mutable_type() = serializeTypeAnnotation(type);
  }

  if (proto.hasReturnType()) {
    *result.mutable_return_type() =
        serializeTypeAnnotation(*proto.getReturnType());
  }

  for (const auto& cap : proto.getCaptures()) {
    *result.add_captures() = serializeCapture(cap);
  }

  for (const auto& refName : proto.getRefCaptureNames()) {
    result.add_ref_captures(refName);
  }

  for (const auto& refName : proto.getConstRefCaptureNames()) {
    result.add_const_ref_captures(refName);
  }

  for (const auto& ownedName : proto.getOwnedCaptureNames()) {
    result.add_owned_captures(ownedName);
  }

  if (proto.hasVariadicParam()) {
    result.set_variadic_param_name(proto.getVariadicParamName());
  }

  if (proto.hasVariadicTypeAnnotation()) {
    *result.mutable_variadic_type_annotation() =
        serializeTypeAnnotation(proto.getVariadicTypeAnnotation());
  }

  result.set_c_variadic(proto.isCVariadic());
  result.set_is_const_method(proto.isConstMethod());
  if (proto.hasLinkName()) {
    result.set_link_name(proto.getLinkName());
  }
  result.set_doc(proto.getDoc());

  if (config_.include_location) {
    *result.mutable_location() = serializePosition(proto.getLocation());
  }

  return result;
}

ast::ASTNode ASTSerializer::serialize(const ExprAST& expr) const {
  ast::ASTNode node;
  serializeExprBase(expr, &node);

  switch (expr.getType()) {
    case ASTNodeType::NUMBER:
      serializeNumber(static_cast<const NumberExprAST&>(expr), &node);
      break;
    case ASTNodeType::CHAR_LITERAL:
      serializeCharLiteral(static_cast<const CharLiteralAST&>(expr), &node);
      break;
    case ASTNodeType::STRING_LITERAL:
      serializeString(static_cast<const StringLiteralAST&>(expr), &node);
      break;
    case ASTNodeType::NULL_LITERAL:
      node.mutable_null_literal();
      break;
    case ASTNodeType::BOOL_LITERAL:
      serializeBool(static_cast<const BoolLiteralAST&>(expr), &node);
      break;
    case ASTNodeType::ARRAY_LITERAL:
      serializeArray(static_cast<const ArrayLiteralAST&>(expr), &node);
      break;
    case ASTNodeType::STRUCT_LITERAL:
      serializeStructLiteral(static_cast<const StructLiteralAST&>(expr), &node);
      break;
    case ASTNodeType::SLICE:
      serializeSlice(static_cast<const SliceExprAST&>(expr), &node);
      break;
    case ASTNodeType::INDEX:
      serializeIndex(static_cast<const IndexAST&>(expr), &node);
      break;
    case ASTNodeType::ARRAY_INDEX:
      serializeArrayIndex(static_cast<const ArrayIndexAST&>(expr), &node);
      break;
    case ASTNodeType::VARIABLE_REFERENCE:
      serializeVariableRef(static_cast<const VariableReferenceAST&>(expr),
                           &node);
      break;
    case ASTNodeType::VARIABLE_CREATION:
      serializeVariableCreation(static_cast<const VariableCreationAST&>(expr),
                                &node);
      break;
    case ASTNodeType::VARIABLE_ASSIGNMENT:
      serializeVariableAssignment(
          static_cast<const VariableAssignmentAST&>(expr), &node);
      break;
    case ASTNodeType::REFERENCE_CREATION:
      serializeReferenceCreation(static_cast<const ReferenceCreationAST&>(expr),
                                 &node);
      break;
    case ASTNodeType::INDEXED_ASSIGNMENT:
      serializeIndexedAssignment(static_cast<const IndexedAssignmentAST&>(expr),
                                 &node);
      break;
    case ASTNodeType::COMPOUND_ASSIGNMENT:
      serializeCompoundAssignment(
          static_cast<const CompoundAssignmentAST&>(expr), &node);
      break;
    case ASTNodeType::MEMBER_ASSIGNMENT:
      serializeMemberAssignment(static_cast<const MemberAssignmentAST&>(expr),
                                &node);
      break;
    case ASTNodeType::BINARY:
      serializeBinary(static_cast<const BinaryExprAST&>(expr), &node);
      break;
    case ASTNodeType::UNARY:
      serializeUnary(static_cast<const UnaryExprAST&>(expr), &node);
      break;
    case ASTNodeType::TERNARY:
      serializeTernary(static_cast<const TernaryExprAST&>(expr), &node);
      break;
    case ASTNodeType::PAREN_EXPR:
      serializeParen(static_cast<const ParenExprAST&>(expr), &node);
      break;
    case ASTNodeType::INTERPOLATED_STRING:
      serializeInterpolatedString(
          static_cast<const InterpolatedStringAST&>(expr), &node);
      break;
    case ASTNodeType::PACK_EXPANSION:
      serializePackExpansion(static_cast<const PackExpansionAST&>(expr), &node);
      break;
    case ASTNodeType::BLOCK:
      serializeBlock(static_cast<const BlockExprAST&>(expr), &node);
      break;
    case ASTNodeType::IF:
      serializeIf(static_cast<const IfExprAST&>(expr), &node);
      break;
    case ASTNodeType::MATCH:
      serializeMatch(static_cast<const MatchExprAST&>(expr), &node);
      break;
    case ASTNodeType::FOR_LOOP:
      serializeFor(static_cast<const ForExprAST&>(expr), &node);
      break;
    case ASTNodeType::FOR_IN_LOOP:
      serializeForIn(static_cast<const ForInExprAST&>(expr), &node);
      break;
    case ASTNodeType::WHILE_LOOP:
      serializeWhile(static_cast<const WhileExprAST&>(expr), &node);
      break;
    case ASTNodeType::BREAK_STMT:
      node.mutable_break_stmt();
      break;
    case ASTNodeType::CONTINUE_STMT:
      node.mutable_continue_stmt();
      break;
    case ASTNodeType::RETURN:
      serializeReturn(static_cast<const ReturnExprAST&>(expr), &node);
      break;
    case ASTNodeType::UNSAFE_BLOCK:
      serializeUnsafeBlock(static_cast<const UnsafeBlockAST&>(expr), &node);
      break;
    case ASTNodeType::FUNCTION:
      serializeFunction(static_cast<const FunctionAST&>(expr), &node);
      break;
    case ASTNodeType::LAMBDA:
      serializeLambda(static_cast<const LambdaAST&>(expr), &node);
      break;
    case ASTNodeType::CALL:
      serializeCall(static_cast<const CallExprAST&>(expr), &node);
      break;
    case ASTNodeType::GENERIC_CALL:
      serializeGenericCall(static_cast<const GenericCallAST&>(expr), &node);
      break;
    case ASTNodeType::MODULE:
      serializeModule(static_cast<const ModuleAST&>(expr), &node);
      break;
    case ASTNodeType::MANIFEST:
      serializeManifest(static_cast<const ManifestAST&>(expr), &node);
      break;
    case ASTNodeType::MOON_SCOPE:
      // An ephemeral wrapper around already-precompiled imports, so it never
      // reaches a serialized tree. Stand an empty block in for it.
      node.mutable_block_expr();
      break;
    case ASTNodeType::USING:
      serializeUsing(static_cast<const UsingAST&>(expr), &node);
      break;
    case ASTNodeType::QUALIFIED_NAME:
      serializeQualifiedName(static_cast<const QualifiedNameAST&>(expr), &node);
      break;
    case ASTNodeType::CLASS_DEFINITION:
      serializeClassDef(static_cast<const ClassDefinitionAST&>(expr), &node);
      break;
    case ASTNodeType::INTERFACE_DEFINITION:
      serializeInterfaceDef(static_cast<const InterfaceDefinitionAST&>(expr),
                            &node);
      break;
    case ASTNodeType::ENUM_DEFINITION:
      serializeEnumDef(static_cast<const EnumDefinitionAST&>(expr), &node);
      break;
    case ASTNodeType::THIS:
      node.mutable_this_expr();
      break;
    case ASTNodeType::MEMBER_ACCESS:
      serializeMemberAccess(static_cast<const MemberAccessAST&>(expr), &node);
      break;
    case ASTNodeType::TRY_CATCH:
      serializeTryCatch(static_cast<const TryCatchExprAST&>(expr), &node);
      break;
    case ASTNodeType::THROW:
      serializeThrow(static_cast<const ThrowExprAST&>(expr), &node);
      break;
    case ASTNodeType::DECLARE_TYPE:
      serializeDeclareType(static_cast<const DeclareTypeAST&>(expr), &node);
      break;
    case ASTNodeType::PROTOTYPE:
      // Prototypes are not standalone expressions
      break;
    case ASTNodeType::IMPORT:
    case ASTNodeType::IMPORT_SCOPE:
      // Imports are resolved by the parser and never reach a serialized tree
      break;
  }

  return node;
}

std::string ASTSerializer::serializeToString(const ExprAST& expr) const {
  return serialize(expr).SerializeAsString();
}

std::string ASTSerializer::serializeProgramToString(
    const BlockExprAST& root) const {
  return serializeProgram(root).SerializeAsString();
}

// =============================================================================
// Individual node serializers
// =============================================================================

void ASTSerializer::serializeNumber(const NumberExprAST& expr,
                                    ast::ASTNode* node) const {
  auto* num = node->mutable_number_expr();
  if (expr.isInteger()) {
    num->set_int_value(expr.getIntVal());
  } else {
    num->set_float_value(expr.getFloatVal());
  }
  if (expr.hasSuffix()) {
    num->set_suffix(expr.getSuffix());
  }
}

void ASTSerializer::serializeCharLiteral(const CharLiteralAST& expr,
                                         ast::ASTNode* node) const {
  auto* lit = node->mutable_char_literal();
  lit->set_value(expr.getValue());
  lit->set_is_byte(expr.isByte());
}

void ASTSerializer::serializeString(const StringLiteralAST& expr,
                                    ast::ASTNode* node) const {
  node->mutable_string_literal()->set_value(expr.getValue());
}

void ASTSerializer::serializeBool(const BoolLiteralAST& expr,
                                  ast::ASTNode* node) const {
  node->mutable_bool_literal()->set_value(expr.getValue());
}

void ASTSerializer::serializeStructLiteral(const StructLiteralAST& expr,
                                           ast::ASTNode* node) const {
  auto* literal = node->mutable_struct_literal();
  for (const auto& field : expr.getFields()) {
    auto* out = literal->add_fields();
    out->set_name(field.name);
    *out->mutable_value() = serialize(*field.value);
    if (config_.include_location) {
      *out->mutable_location() = serializePosition(field.location);
    }
  }
}

void ASTSerializer::serializeArray(const ArrayLiteralAST& expr,
                                   ast::ASTNode* node) const {
  auto* arr = node->mutable_array_literal();
  for (const auto& elem : expr.getElements()) {
    *arr->add_elements() = serialize(*elem);
  }
}

void ASTSerializer::serializeSliceInto(const SliceExprAST& slice,
                                       ast::SliceExpr* proto) const {
  if (slice.getStart()) {
    *proto->mutable_start() = serialize(*slice.getStart());
  }
  if (slice.getEnd()) {
    *proto->mutable_end() = serialize(*slice.getEnd());
  }
  proto->set_is_range(slice.isRange());
}

void ASTSerializer::serializeSlice(const SliceExprAST& expr,
                                   ast::ASTNode* node) const {
  serializeSliceInto(expr, node->mutable_slice_expr());
}

void ASTSerializer::serializeIndex(const IndexAST& expr,
                                   ast::ASTNode* node) const {
  auto* idx = node->mutable_index_expr();
  *idx->mutable_target() = serialize(*expr.getTarget());
  for (const auto& slice : expr.getIndices()) {
    serializeSliceInto(*slice, idx->add_indices());
  }
}

void ASTSerializer::serializeArrayIndex(const ArrayIndexAST& expr,
                                        ast::ASTNode* node) const {
  auto* idx = node->mutable_array_index_expr();
  *idx->mutable_array() = serialize(*expr.getArray());
  for (const auto& index : expr.getIndices()) {
    *idx->add_indices() = serialize(*index);
  }
}

void ASTSerializer::serializeVariableRef(const VariableReferenceAST& expr,
                                         ast::ASTNode* node) const {
  node->mutable_variable_reference()->set_name(expr.getName());
}

void ASTSerializer::serializeVariableCreation(const VariableCreationAST& expr,
                                              ast::ASTNode* node) const {
  auto* var = node->mutable_variable_creation();
  var->set_name(expr.getName());
  var->set_visibility(toProto(expr.getVisibility()));
  var->set_is_const(expr.isConst());
  var->set_is_c_extern(expr.isCExtern());
  var->set_explicit_c_abi(expr.hasExplicitCAbi());
  if (expr.hasLinkName()) var->set_link_name(expr.getLinkName());
  var->set_doc(expr.getDoc());
  if (expr.getValue()) {
    *var->mutable_value() = serialize(*expr.getValue());
  }
  if (expr.hasTypeAnnotation()) {
    *var->mutable_type_annotation() =
        serializeTypeAnnotation(*expr.getTypeAnnotation());
  }
}

void ASTSerializer::serializeVariableAssignment(
    const VariableAssignmentAST& expr, ast::ASTNode* node) const {
  auto* assign = node->mutable_variable_assignment();
  assign->set_name(expr.getName());
  *assign->mutable_value() = serialize(*expr.getValue());
}

void ASTSerializer::serializeReferenceCreation(const ReferenceCreationAST& expr,
                                               ast::ASTNode* node) const {
  auto* ref = node->mutable_reference_creation();
  ref->set_name(expr.getName());
  *ref->mutable_target() = serialize(*expr.getTarget());
  ref->set_is_mutable(expr.isMutable());
}

void ASTSerializer::serializeIndexedAssignment(const IndexedAssignmentAST& expr,
                                               ast::ASTNode* node) const {
  auto* assign = node->mutable_indexed_assignment();
  *assign->mutable_target() = serialize(*expr.getTarget());
  *assign->mutable_value() = serialize(*expr.getValue());
}

void ASTSerializer::serializeMemberAssignment(const MemberAssignmentAST& expr,
                                              ast::ASTNode* node) const {
  auto* assign = node->mutable_member_assignment();
  *assign->mutable_object() = serialize(*expr.getObject());
  assign->set_member_name(expr.getMemberName());
  *assign->mutable_value() = serialize(*expr.getValue());
}

void ASTSerializer::serializeCompoundAssignment(
    const CompoundAssignmentAST& expr, ast::ASTNode* node) const {
  auto* assign = node->mutable_compound_assignment();
  *assign->mutable_target() = serialize(*expr.getTarget());
  *assign->mutable_op() = serializeToken(expr.getOp());
  *assign->mutable_value() = serialize(*expr.getValue());
}

void ASTSerializer::serializeBinary(const BinaryExprAST& expr,
                                    ast::ASTNode* node) const {
  auto* bin = node->mutable_binary_expr();
  *bin->mutable_op() = serializeToken(expr.getOp());
  *bin->mutable_lhs() = serialize(*expr.getLHS());
  *bin->mutable_rhs() = serialize(*expr.getRHS());
}

void ASTSerializer::serializeTernary(const TernaryExprAST& expr,
                                     ast::ASTNode* node) const {
  auto* tern = node->mutable_ternary_expr();
  *tern->mutable_cond() = serialize(*expr.getCond());
  *tern->mutable_then_expr() = serialize(*expr.getThen());
  *tern->mutable_else_expr() = serialize(*expr.getElse());
}

void ASTSerializer::serializeParen(const ParenExprAST& expr,
                                   ast::ASTNode* node) const {
  auto* paren = node->mutable_paren_expr();
  *paren->mutable_inner() = serialize(*expr.getInner());
}

void ASTSerializer::serializeInterpolatedString(
    const InterpolatedStringAST& expr, ast::ASTNode* node) const {
  auto* interp = node->mutable_interpolated_string();
  interp->set_raw_content(expr.getRawContent());
  for (const auto& segment : expr.getSegments()) {
    auto* seg = interp->add_segments();
    seg->set_is_literal(segment.isLiteral);
    seg->set_raw_text(segment.rawText);
    seg->set_cooked_text(segment.cookedText);
    seg->set_source_offset(segment.sourceOffset);
    if (segment.expression) {
      *seg->mutable_expression() = serialize(*segment.expression);
    }
  }
}

void ASTSerializer::serializeUnary(const UnaryExprAST& expr,
                                   ast::ASTNode* node) const {
  auto* un = node->mutable_unary_expr();
  *un->mutable_op() = serializeToken(expr.getOp());
  *un->mutable_operand() = serialize(*expr.getOperand());
}

void ASTSerializer::serializePackExpansion(const PackExpansionAST& expr,
                                           ast::ASTNode* node) const {
  node->mutable_pack_expansion()->set_pack_name(expr.getPackName());
}

void ASTSerializer::serializeBlock(const BlockExprAST& expr,
                                   ast::ASTNode* node) const {
  serializeBlockInto(expr, node->mutable_block_expr());
}

void ASTSerializer::serializeIf(const IfExprAST& expr,
                                ast::ASTNode* node) const {
  auto* ifExpr = node->mutable_if_expr();
  *ifExpr->mutable_condition() = serialize(*expr.getCond());
  *ifExpr->mutable_then_branch() = serialize(*expr.getThen());
  if (expr.getElse()) {
    *ifExpr->mutable_else_branch() = serialize(*expr.getElse());
  }
}

void ASTSerializer::serializeMatch(const MatchExprAST& expr,
                                   ast::ASTNode* node) const {
  auto* match = node->mutable_match_expr();
  *match->mutable_discriminant() = serialize(*expr.getDiscriminant());
  for (const auto& arm : expr.getArms()) {
    auto* armProto = match->add_arms();
    if (arm.pattern) {
      *armProto->mutable_pattern() = serialize(*arm.pattern);
    }
    armProto->set_is_wildcard(arm.isWildcard);
    *armProto->mutable_body() = serialize(*arm.body);
    armProto->set_has_payload_parens(arm.hasPayloadParens);
    for (const auto& binding : arm.bindings) {
      auto* bindingProto = armProto->add_bindings();
      bindingProto->set_name(binding.name);
      bindingProto->set_is_wildcard(binding.isWildcard);
      if (config_.include_location) {
        *bindingProto->mutable_location() = serializePosition(binding.location);
      }
    }
  }
}

void ASTSerializer::serializeFor(const ForExprAST& expr,
                                 ast::ASTNode* node) const {
  auto* forExpr = node->mutable_for_expr();
  if (expr.getInit()) {
    *forExpr->mutable_init() = serialize(*expr.getInit());
  }
  if (expr.getCondition()) {
    *forExpr->mutable_condition() = serialize(*expr.getCondition());
  }
  if (expr.getIncrement()) {
    *forExpr->mutable_increment() = serialize(*expr.getIncrement());
  }
  *forExpr->mutable_body() = serialize(*expr.getBody());
}

void ASTSerializer::serializeForIn(const ForInExprAST& expr,
                                   ast::ASTNode* node) const {
  auto* forIn = node->mutable_for_in_expr();
  forIn->set_loop_var(expr.getLoopVar());
  *forIn->mutable_loop_var_type() =
      serializeTypeAnnotation(expr.getLoopVarType());
  *forIn->mutable_iterable() = serialize(*expr.getIterable());
  *forIn->mutable_body() = serialize(*expr.getBody());
  forIn->set_is_const(expr.isConst());
}

void ASTSerializer::serializeWhile(const WhileExprAST& expr,
                                   ast::ASTNode* node) const {
  auto* whileExpr = node->mutable_while_expr();
  *whileExpr->mutable_condition() = serialize(*expr.getCondition());
  *whileExpr->mutable_body() = serialize(*expr.getBody());
}

void ASTSerializer::serializeReturn(const ReturnExprAST& expr,
                                    ast::ASTNode* node) const {
  auto* ret = node->mutable_return_expr();
  if (expr.getValue()) {
    *ret->mutable_value() = serialize(*expr.getValue());
  }
}

void ASTSerializer::serializeUnsafeBlock(const UnsafeBlockAST& expr,
                                         ast::ASTNode* node) const {
  auto* unsafe = node->mutable_unsafe_block();
  serializeBlockInto(expr.getBody(), unsafe->mutable_body());
}

void ASTSerializer::serializeFunction(const FunctionAST& expr,
                                      ast::ASTNode* node) const {
  auto* func = node->mutable_function_def();
  *func->mutable_proto() = serializePrototype(expr.getProto());
  func->set_is_c_extern(expr.isCExtern());
  func->set_visibility(toProto(expr.getVisibility()));
  // An empty body and no body are different things: the latter is a
  // declaration, and reconstructing it as the former would give a C extern
  // Sun name mangling and lose its symbol.
  func->set_body_present(expr.hasBody());
  auto* body = func->mutable_body();
  if (expr.hasBody()) serializeBlockInto(expr.getBody(), body);
}

void ASTSerializer::serializeMethodFunction(const FunctionAST& function,
                                            ast::FunctionDef* proto) const {
  *proto->mutable_proto() = serializePrototype(function.getProto());
  if (function.hasBody()) {
    serializeBlockInto(function.getBody(), proto->mutable_body());
  }
  proto->set_visibility(toProto(function.getVisibility()));
  if (config_.include_location && function.getLocation().endOffset) {
    *proto->mutable_location() = serializePosition(function.getLocation());
  }
}

void ASTSerializer::serializeLambda(const LambdaAST& expr,
                                    ast::ASTNode* node) const {
  auto* lambda = node->mutable_lambda_expr();
  *lambda->mutable_proto() = serializePrototype(expr.getProto());
  serializeBlockInto(expr.getBody(), lambda->mutable_body());
}

void ASTSerializer::serializeCall(const CallExprAST& expr,
                                  ast::ASTNode* node) const {
  auto* call = node->mutable_call_expr();
  *call->mutable_callee() = serialize(*expr.getCallee());
  for (const auto& arg : expr.getArgs()) {
    *call->add_args() = serialize(*arg);
  }
}

void ASTSerializer::serializeGenericCall(const GenericCallAST& expr,
                                         ast::ASTNode* node) const {
  auto* call = node->mutable_generic_call_expr();
  call->set_function_name(expr.getFunctionName());
  for (const auto& typeArg : expr.getTypeArguments()) {
    *call->add_type_arguments() = serializeTypeAnnotation(*typeArg);
  }
  for (const auto& arg : expr.getArgs()) {
    *call->add_args() = serialize(*arg);
  }
}

void ASTSerializer::serializeManifest(const ManifestAST& expr,
                                      ast::ASTNode* node) const {
  auto* manifest = node->mutable_manifest();

  // Suns
  auto* suns = manifest->mutable_suns();
  for (const auto& sun : expr.getSuns()) {
    auto* sunProto = suns->Add();
    sunProto->set_path(sun.path);
    if (sun.hash) {
      sunProto->set_hash(*sun.hash);
    }
  }

  // Moons
  auto* moons = manifest->mutable_moons();
  for (const auto& moon : expr.getMoons()) {
    auto* moonProto = moons->Add();
    moonProto->set_path(moon.path);
    if (moon.url) {
      moonProto->set_url(*moon.url);
    }
    if (moon.hash) {
      moonProto->set_hash(*moon.hash);
    }
    if (moon.rename) {
      moonProto->set_rename_module(*moon.rename);
    }
  }

  // Protos
  auto* protos = manifest->mutable_protos();
  for (const auto& proto : expr.getProtos()) {
    protos->Add()->set_path(proto.path);
  }
}

void ASTSerializer::serializeModule(const ModuleAST& expr,
                                    ast::ASTNode* node) const {
  auto* mod = node->mutable_module_def();
  mod->set_name(expr.getName());
  mod->set_visibility(toProto(expr.getVisibility()));
  serializeBlockInto(expr.getBody(), mod->mutable_body());
}

void ASTSerializer::serializeUsing(const UsingAST& expr,
                                   ast::ASTNode* node) const {
  auto* using_ = node->mutable_using_stmt();
  for (const auto& part : expr.getNamespacePath()) {
    using_->add_namespace_path(part);
  }
  using_->set_target(expr.getTarget());
  using_->set_is_module_import(expr.isModuleImport());
}

void ASTSerializer::serializeQualifiedName(const QualifiedNameAST& expr,
                                           ast::ASTNode* node) const {
  auto* qn = node->mutable_qualified_name();
  for (const auto& part : expr.getParts()) {
    qn->add_parts(part);
  }
}

void ASTSerializer::serializeClassDef(const ClassDefinitionAST& expr,
                                      ast::ASTNode* node) const {
  auto* cls = node->mutable_class_def();
  cls->set_name(expr.getName());

  for (const auto& tp : expr.getTypeParameters()) {
    auto* out = cls->add_type_params();
    out->set_name(tp.name);
    if (tp.constraint) out->set_constraint(tp.constraint->name);
  }

  for (const auto& lp : expr.getLifetimeParameters()) {
    cls->add_lifetime_params(lp.name);
  }

  for (const auto& iface : expr.getImplementedInterfaces()) {
    auto* ifaceProto = cls->add_implemented_interfaces();
    ifaceProto->set_name(iface.name);
    for (const auto& typeArg : iface.typeArguments) {
      *ifaceProto->add_type_arguments() = serializeTypeAnnotation(typeArg);
    }
  }

  for (const auto& field : expr.getFields()) {
    serializeFieldInto(field, cls->add_fields());
  }

  for (const auto& method : expr.getMethods()) {
    auto* methodProto = cls->add_methods();
    serializeMethodFunction(*method.function, methodProto->mutable_function());
    methodProto->set_is_constructor(method.isConstructor);
    methodProto->set_is_const(method.isConst);
  }

  cls->set_is_partial(expr.isPartial());
  cls->set_is_packed(expr.isPacked());
  cls->set_visibility(toProto(expr.getVisibility()));
  cls->set_doc(expr.getDoc());
}

void ASTSerializer::serializeInterfaceDef(const InterfaceDefinitionAST& expr,
                                          ast::ASTNode* node) const {
  auto* iface = node->mutable_interface_def();
  iface->set_name(expr.getName());

  for (const auto& tp : expr.getTypeParameters()) {
    auto* out = iface->add_type_params();
    out->set_name(tp.name);
    if (tp.constraint) out->set_constraint(tp.constraint->name);
  }

  for (const auto& lp : expr.getLifetimeParameters()) {
    iface->add_lifetime_params(lp.name);
  }

  for (const auto& field : expr.getFields()) {
    serializeFieldInto(field, iface->add_fields());
  }

  for (const auto& method : expr.getMethods()) {
    auto* methodProto = iface->add_methods();
    serializeMethodFunction(*method.function, methodProto->mutable_function());
    methodProto->set_has_default_impl(method.hasDefaultImpl);
    methodProto->set_is_const(method.isConst);
  }
  iface->set_visibility(toProto(expr.getVisibility()));
  iface->set_doc(expr.getDoc());
}

void ASTSerializer::serializeEnumDef(const EnumDefinitionAST& expr,
                                     ast::ASTNode* node) const {
  auto* enumDef = node->mutable_enum_def();
  enumDef->set_name(expr.getName());
  enumDef->set_visibility(toProto(expr.getVisibility()));
  enumDef->set_doc(expr.getDoc());
  for (const auto& tp : expr.getTypeParameters()) {
    auto* out = enumDef->add_type_params();
    out->set_name(tp.name);
    if (tp.constraint) out->set_constraint(tp.constraint->name);
  }

  for (const auto& variant : expr.getVariants()) {
    auto* variantProto = enumDef->add_variants();
    variantProto->set_name(variant.name);
    variantProto->set_value(variant.value);
    variantProto->set_doc(variant.doc);
    if (config_.include_location) {
      *variantProto->mutable_location() = serializePosition(variant.location);
    }
    for (const auto& payloadType : variant.payloadTypes) {
      *variantProto->add_payload_types() = serializeTypeAnnotation(payloadType);
    }
  }
}

void ASTSerializer::serializeMemberAccess(const MemberAccessAST& expr,
                                          ast::ASTNode* node) const {
  auto* access = node->mutable_member_access();
  *access->mutable_object() = serialize(*expr.getObject());
  access->set_member_name(expr.getMemberName());
  for (const auto& typeArg : expr.getTypeArguments()) {
    *access->add_type_arguments() = serializeTypeAnnotation(*typeArg);
  }
}

void ASTSerializer::serializeTryCatch(const TryCatchExprAST& expr,
                                      ast::ASTNode* node) const {
  auto* tryCatch = node->mutable_try_catch();

  // Serialize try block
  serializeBlockInto(expr.getTryBlock(), tryCatch->mutable_try_block());

  // Serialize catch clauses (source order)
  for (const auto& cc : expr.getCatchClauses()) {
    auto* catchClause = tryCatch->add_catch_clauses();
    catchClause->set_binding_name(cc.bindingName);
    if (cc.bindingType) {
      *catchClause->mutable_binding_type() =
          serializeTypeAnnotation(*cc.bindingType);
    }
    serializeBlockInto(*cc.body, catchClause->mutable_body());
  }
}

void ASTSerializer::serializeThrow(const ThrowExprAST& expr,
                                   ast::ASTNode* node) const {
  auto* throwExpr = node->mutable_throw_expr();
  *throwExpr->mutable_error_expr() = serialize(expr.getErrorExpr());
}

void ASTSerializer::serializeDeclareType(const DeclareTypeAST& expr,
                                         ast::ASTNode* node) const {
  auto* decl = node->mutable_declare_type();
  if (expr.hasAlias()) {
    decl->set_alias_name(expr.getAliasName());
  }
  *decl->mutable_type_annotation() =
      serializeTypeAnnotation(expr.getTypeAnnotation());
  decl->set_visibility(toProto(expr.getVisibility()));
}

}  // namespace serialization
}  // namespace sun
