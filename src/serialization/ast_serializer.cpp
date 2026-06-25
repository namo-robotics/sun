// ast_serializer.cpp — Implementation of AST to protobuf serialization

#include "ast_serializer.h"

#include "ast.h"
#include "ast.pb.h"
#include "types.pb.h"

namespace sun {
namespace serialization {

// Helper to convert C++ TokenKind to proto TokenKind
static ast::TokenKind toProtoTokenKind(TokenKind kind) {
  switch (kind) {
    case TokenKind::PLUS:
      return ast::TOKEN_KIND_PLUS;
    case TokenKind::MINUS:
      return ast::TOKEN_KIND_MINUS;
    case TokenKind::STAR:
      return ast::TOKEN_KIND_STAR;
    case TokenKind::SLASH:
      return ast::TOKEN_KIND_SLASH;
    case TokenKind::LESS:
      return ast::TOKEN_KIND_LESS;
    case TokenKind::LESS_EQUAL:
      return ast::TOKEN_KIND_LESS_EQUAL;
    case TokenKind::GREATER:
      return ast::TOKEN_KIND_GREATER;
    case TokenKind::GREATER_EQUAL:
      return ast::TOKEN_KIND_GREATER_EQUAL;
    case TokenKind::EQUAL_EQUAL:
      return ast::TOKEN_KIND_EQUAL_EQUAL;
    case TokenKind::NOT_EQUAL:
      return ast::TOKEN_KIND_NOT_EQUAL;
    case TokenKind::EQUAL:
      return ast::TOKEN_KIND_EQUAL;
    default:
      return ast::TOKEN_KIND_UNKNOWN;
  }
}

ast::Position ASTSerializer::serializePosition(const Position& pos) const {
  ast::Position proto;
  proto.set_line(pos.line);
  proto.set_column(pos.column);
  proto.set_offset(pos.offset);
  if (pos.filePath) {
    proto.set_file_path(*pos.filePath);
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
  return proto;
}

ast::Capture ASTSerializer::serializeCapture(const Capture& cap) const {
  ast::Capture proto;
  proto.set_name(cap.name);
  if (cap.type) {
    proto.set_type_signature(cap.type->toString());
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

  if (config_.include_analysis && expr.hasAnalysis()) {
    auto* analysis = node->mutable_analysis();
    if (expr.hasResolvedType()) {
      analysis->set_resolved_type(expr.getResolvedType()->toString());
    }
    analysis->set_moved(expr.isMoved());
  }
}

ast::Program ASTSerializer::serializeProgram(const BlockExprAST& root) const {
  ast::Program program;
  program.set_version(1);
  program.set_has_analysis(config_.include_analysis);

  ast::BlockExpr* block = program.mutable_body();
  for (const auto& stmt : root.getBody()) {
    *block->add_body() = serialize(*stmt);
  }

  return program;
}

ast::Prototype ASTSerializer::serializePrototype(
    const PrototypeAST& proto) const {
  ast::Prototype result;
  result.set_name(proto.getName());

  for (const auto& tp : proto.getTypeParameters()) {
    result.add_type_parameters(tp);
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

  if (proto.hasVariadicParam()) {
    result.set_variadic_param_name(*proto.getVariadicParamName());
  }

  if (proto.hasVariadicConstraint()) {
    *result.mutable_variadic_constraint() =
        serializeTypeAnnotation(*proto.getVariadicConstraint());
  }

  // Serialize prototype analysis if requested
  if (config_.include_analysis && proto.hasAnalysis()) {
    auto* analysis = result.mutable_analysis();
    analysis->set_qualified_name(proto.getMangledName());
    // Add resolved types if available
    const auto* protoAnalysis = proto.getAnalysis();
    if (protoAnalysis) {
      for (const auto& pt : protoAnalysis->resolvedParamTypes) {
        if (pt) {
          analysis->add_resolved_param_types(pt->toString());
        }
      }
      if (protoAnalysis->resolvedReturnType) {
        analysis->set_resolved_return_type(
            protoAnalysis->resolvedReturnType->toString());
      }
    }
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
    case ASTNodeType::STRING_LITERAL:
      serializeString(static_cast<const StringLiteralAST&>(expr), &node);
      break;
    case ASTNodeType::NULL_LITERAL:
      serializeNull(static_cast<const NullLiteralAST&>(expr), &node);
      break;
    case ASTNodeType::BOOL_LITERAL:
      serializeBool(static_cast<const BoolLiteralAST&>(expr), &node);
      break;
    case ASTNodeType::ARRAY_LITERAL:
      serializeArray(static_cast<const ArrayLiteralAST&>(expr), &node);
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
      serializeBreak(static_cast<const BreakAST&>(expr), &node);
      break;
    case ASTNodeType::CONTINUE_STMT:
      serializeContinue(static_cast<const ContinueAST&>(expr), &node);
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
    case ASTNodeType::SPAWN:
      serializeSpawn(static_cast<const SpawnExprAST&>(expr), &node);
      break;
    case ASTNodeType::MODULE:
      serializeModule(static_cast<const ModuleAST&>(expr), &node);
      break;
    case ASTNodeType::MOON_SCOPE:
      // MoonScopeAST should never be serialized - it's an ephemeral import
      // wrapper If we reach here, just serialize the contained modules This
      // shouldn't happen in normal use since moon stubs are precompiled
      for (const auto& bodyExpr :
           static_cast<const MoonScopeAST&>(expr).getBody().getBody()) {
        // Serialize each contained module (they'll likely be skipped as
        // precompiled)
        serialize(*bodyExpr);
      }
      // Return empty block_expr as placeholder
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
      serializeThis(static_cast<const ThisExprAST&>(expr), &node);
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
}

void ASTSerializer::serializeString(const StringLiteralAST& expr,
                                    ast::ASTNode* node) const {
  node->mutable_string_literal()->set_value(expr.getValue());
}

void ASTSerializer::serializeNull(const NullLiteralAST& expr,
                                  ast::ASTNode* node) const {
  node->mutable_null_literal();  // Just set the oneof
}

void ASTSerializer::serializeBool(const BoolLiteralAST& expr,
                                  ast::ASTNode* node) const {
  node->mutable_bool_literal()->set_value(expr.getValue());
}

void ASTSerializer::serializeArray(const ArrayLiteralAST& expr,
                                   ast::ASTNode* node) const {
  auto* arr = node->mutable_array_literal();
  for (const auto& elem : expr.getElements()) {
    *arr->add_elements() = serialize(*elem);
  }
}

void ASTSerializer::serializeSlice(const SliceExprAST& expr,
                                   ast::ASTNode* node) const {
  auto* slice = node->mutable_slice_expr();
  if (expr.getStart()) {
    *slice->mutable_start() = serialize(*expr.getStart());
  }
  if (expr.getEnd()) {
    *slice->mutable_end() = serialize(*expr.getEnd());
  }
  slice->set_is_range(expr.isRange());
}

void ASTSerializer::serializeIndex(const IndexAST& expr,
                                   ast::ASTNode* node) const {
  auto* idx = node->mutable_index_expr();
  *idx->mutable_target() = serialize(*expr.getTarget());
  for (const auto& slice : expr.getIndices()) {
    auto* sliceProto = idx->add_indices();
    if (slice->getStart()) {
      *sliceProto->mutable_start() = serialize(*slice->getStart());
    }
    if (slice->getEnd()) {
      *sliceProto->mutable_end() = serialize(*slice->getEnd());
    }
    sliceProto->set_is_range(slice->isRange());
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

void ASTSerializer::serializeBinary(const BinaryExprAST& expr,
                                    ast::ASTNode* node) const {
  auto* bin = node->mutable_binary_expr();
  *bin->mutable_op() = serializeToken(expr.getOp());
  *bin->mutable_lhs() = serialize(*expr.getLHS());
  *bin->mutable_rhs() = serialize(*expr.getRHS());
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
  auto* block = node->mutable_block_expr();
  for (const auto& stmt : expr.getBody()) {
    *block->add_body() = serialize(*stmt);
  }
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
}

void ASTSerializer::serializeWhile(const WhileExprAST& expr,
                                   ast::ASTNode* node) const {
  auto* whileExpr = node->mutable_while_expr();
  *whileExpr->mutable_condition() = serialize(*expr.getCondition());
  *whileExpr->mutable_body() = serialize(*expr.getBody());
}

void ASTSerializer::serializeBreak(const BreakAST& expr,
                                   ast::ASTNode* node) const {
  node->mutable_break_stmt();  // Just set the oneof
}

void ASTSerializer::serializeContinue(const ContinueAST& expr,
                                      ast::ASTNode* node) const {
  node->mutable_continue_stmt();  // Just set the oneof
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
  auto* body = unsafe->mutable_body();
  for (const auto& stmt : expr.getBody().getBody()) {
    *body->add_body() = serialize(*stmt);
  }
}

void ASTSerializer::serializeFunction(const FunctionAST& expr,
                                      ast::ASTNode* node) const {
  auto* func = node->mutable_function_def();
  *func->mutable_proto() = serializePrototype(expr.getProto());
  auto* body = func->mutable_body();
  if (expr.hasBody()) {
    for (const auto& stmt : expr.getBody().getBody()) {
      *body->add_body() = serialize(*stmt);
    }
  }
}

void ASTSerializer::serializeLambda(const LambdaAST& expr,
                                    ast::ASTNode* node) const {
  auto* lambda = node->mutable_lambda_expr();
  *lambda->mutable_proto() = serializePrototype(expr.getProto());
  auto* body = lambda->mutable_body();
  for (const auto& stmt : expr.getBody().getBody()) {
    *body->add_body() = serialize(*stmt);
  }
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

void ASTSerializer::serializeSpawn(const SpawnExprAST& expr,
                                   ast::ASTNode* node) const {
  auto* spawn = node->mutable_spawn_expr();
  *spawn->mutable_lambda() = serialize(expr.getLambda());
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
    if (moon.hash) {
      moonProto->set_hash(*moon.hash);
    }
    if (moon.rename) {
      moonProto->set_rename_module(*moon.rename);
    }
  }
}

void ASTSerializer::serializeModule(const ModuleAST& expr,
                                    ast::ASTNode* node) const {
  auto* mod = node->mutable_module_def();
  mod->set_name(expr.getName());
  auto* body = mod->mutable_body();
  for (const auto& stmt : expr.getBody().getBody()) {
    *body->add_body() = serialize(*stmt);
  }
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
    cls->add_type_parameters(tp);
  }

  for (const auto& iface : expr.getImplementedInterfaces()) {
    auto* ifaceProto = cls->add_implemented_interfaces();
    ifaceProto->set_name(iface.name);
    for (const auto& typeArg : iface.typeArguments) {
      *ifaceProto->add_type_arguments() = serializeTypeAnnotation(typeArg);
    }
  }

  for (const auto& field : expr.getFields()) {
    auto* fieldProto = cls->add_fields();
    fieldProto->set_name(field.name);
    *fieldProto->mutable_type() = serializeTypeAnnotation(field.type);
    *fieldProto->mutable_location() = serializePosition(field.location);
  }

  for (const auto& method : expr.getMethods()) {
    auto* methodProto = cls->add_methods();
    // Serialize the function
    auto* funcProto = methodProto->mutable_function();
    *funcProto->mutable_proto() =
        serializePrototype(method.function->getProto());
    auto* body = funcProto->mutable_body();
    if (method.function->hasBody()) {
      for (const auto& stmt : method.function->getBody().getBody()) {
        *body->add_body() = serialize(*stmt);
      }
    }
    methodProto->set_is_constructor(method.isConstructor);
  }

  cls->set_is_partial(expr.isPartial());
}

void ASTSerializer::serializeInterfaceDef(const InterfaceDefinitionAST& expr,
                                          ast::ASTNode* node) const {
  auto* iface = node->mutable_interface_def();
  iface->set_name(expr.getName());

  for (const auto& tp : expr.getTypeParameters()) {
    iface->add_type_parameters(tp);
  }

  for (const auto& field : expr.getFields()) {
    auto* fieldProto = iface->add_fields();
    fieldProto->set_name(field.name);
    *fieldProto->mutable_type() = serializeTypeAnnotation(field.type);
    *fieldProto->mutable_location() = serializePosition(field.location);
  }

  for (const auto& method : expr.getMethods()) {
    auto* methodProto = iface->add_methods();
    auto* funcProto = methodProto->mutable_function();
    *funcProto->mutable_proto() =
        serializePrototype(method.function->getProto());
    auto* body = funcProto->mutable_body();
    if (method.function->hasBody()) {
      for (const auto& stmt : method.function->getBody().getBody()) {
        *body->add_body() = serialize(*stmt);
      }
    }
    methodProto->set_has_default_impl(method.hasDefaultImpl);
  }
}

void ASTSerializer::serializeEnumDef(const EnumDefinitionAST& expr,
                                     ast::ASTNode* node) const {
  auto* enumDef = node->mutable_enum_def();
  enumDef->set_name(expr.getName());

  for (const auto& variant : expr.getVariants()) {
    auto* variantProto = enumDef->add_variants();
    variantProto->set_name(variant.name);
    variantProto->set_value(variant.value);
    *variantProto->mutable_location() = serializePosition(variant.location);
  }
}

void ASTSerializer::serializeThis(const ThisExprAST& expr,
                                  ast::ASTNode* node) const {
  node->mutable_this_expr();  // Just set the oneof
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
  auto* tryBlock = tryCatch->mutable_try_block();
  for (const auto& stmt : expr.getTryBlock().getBody()) {
    *tryBlock->add_body() = serialize(*stmt);
  }

  // Serialize catch clause
  auto* catchClause = tryCatch->mutable_catch_clause();
  const auto& cc = expr.getCatchClause();
  catchClause->set_binding_name(cc.bindingName);
  if (cc.bindingType) {
    *catchClause->mutable_binding_type() =
        serializeTypeAnnotation(*cc.bindingType);
  }
  auto* catchBody = catchClause->mutable_body();
  for (const auto& stmt : cc.body->getBody()) {
    *catchBody->add_body() = serialize(*stmt);
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
}

}  // namespace serialization
}  // namespace sun
