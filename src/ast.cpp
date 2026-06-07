// ast.cpp — AST clone implementations for deep copying AST nodes

#include "ast.h"

// Helper to clone a vector of unique_ptr<ExprAST>
static std::vector<std::unique_ptr<ExprAST>> cloneExprVector(
    const std::vector<std::unique_ptr<ExprAST>>& vec) {
  std::vector<std::unique_ptr<ExprAST>> result;
  result.reserve(vec.size());
  for (const auto& expr : vec) {
    result.push_back(expr->clone());
  }
  return result;
}

// Helper to clone a vector of unique_ptr<TypeAnnotation>
static std::vector<std::unique_ptr<TypeAnnotation>> cloneTypeAnnotationVector(
    const std::vector<std::unique_ptr<TypeAnnotation>>& vec) {
  std::vector<std::unique_ptr<TypeAnnotation>> result;
  result.reserve(vec.size());
  for (const auto& ta : vec) {
    result.push_back(std::make_unique<TypeAnnotation>(*ta));
  }
  return result;
}

// -------------------------------------------------------------------
// Literal AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> NumberExprAST::clone() const {
  auto copy = isInteger() ? std::make_unique<NumberExprAST>(getIntVal())
                          : std::make_unique<NumberExprAST>(getFloatVal());
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> StringLiteralAST::clone() const {
  auto copy = std::make_unique<StringLiteralAST>(Value);
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> NullLiteralAST::clone() const {
  auto copy = std::make_unique<NullLiteralAST>();
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> BoolLiteralAST::clone() const {
  auto copy = std::make_unique<BoolLiteralAST>(Value);
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> ArrayLiteralAST::clone() const {
  auto copy = std::make_unique<ArrayLiteralAST>(cloneExprVector(elements));
  cloneBase(*copy);
  return copy;
}

// -------------------------------------------------------------------
// Index/Slice AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> ArrayIndexAST::clone() const {
  auto copy =
      std::make_unique<ArrayIndexAST>(array->clone(), cloneExprVector(indices));
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> SliceExprAST::clone() const {
  std::unique_ptr<ExprAST> startClone = start_ ? start_->clone() : nullptr;
  std::unique_ptr<ExprAST> endClone = end_ ? end_->clone() : nullptr;
  auto copy = isRange_ ? std::make_unique<SliceExprAST>(
                             std::move(startClone), std::move(endClone), true)
                       : std::make_unique<SliceExprAST>(std::move(startClone));
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> IndexAST::clone() const {
  std::vector<std::unique_ptr<SliceExprAST>> indicesClone;
  indicesClone.reserve(indices.size());
  for (const auto& idx : indices) {
    auto cloned = idx->clone();
    indicesClone.push_back(std::unique_ptr<SliceExprAST>(
        static_cast<SliceExprAST*>(cloned.release())));
  }
  auto copy =
      std::make_unique<IndexAST>(target->clone(), std::move(indicesClone));
  cloneBase(*copy);
  return copy;
}

// -------------------------------------------------------------------
// Variable AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> VariableReferenceAST::clone() const {
  auto copy = std::make_unique<VariableReferenceAST>(Name);
  cloneBase(*copy);
  if (hasQualifiedName()) {
    copy->setQualifiedName(getQualifiedName());
  }
  return copy;
}

std::unique_ptr<ExprAST> VariableCreationAST::clone() const {
  std::optional<TypeAnnotation> typeClone;
  if (typeAnnotation) {
    typeClone = *typeAnnotation;  // TypeAnnotation has copy constructor
  }
  auto copy = std::make_unique<VariableCreationAST>(name, value->clone(),
                                                    std::move(typeClone));
  cloneBase(*copy);
  if (hasQualifiedName()) {
    copy->setQualifiedName(getQualifiedName());
  }
  return copy;
}

std::unique_ptr<ExprAST> VariableAssignmentAST::clone() const {
  auto copy = std::make_unique<VariableAssignmentAST>(name, value->clone());
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> ReferenceCreationAST::clone() const {
  auto copy =
      std::make_unique<ReferenceCreationAST>(name, target->clone(), mutable_);
  cloneBase(*copy);
  if (hasQualifiedName()) {
    copy->setQualifiedName(getQualifiedName());
  }
  return copy;
}

// -------------------------------------------------------------------
// Expression AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> BinaryExprAST::clone() const {
  auto copy = std::make_unique<BinaryExprAST>(op, LHS->clone(), RHS->clone());
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> UnaryExprAST::clone() const {
  auto copy = std::make_unique<UnaryExprAST>(op, Operand->clone());
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> CallExprAST::clone() const {
  auto copy =
      std::make_unique<CallExprAST>(Callee->clone(), cloneExprVector(Args));
  cloneBase(*copy);
  return copy;
}

std::string CallExprAST::dotLabel() const {
  // Try to get a readable name from the callee
  if (Callee->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto* ref = static_cast<const VariableReferenceAST*>(Callee.get());
    return "Call\n" + ref->getName() + "()";
  }
  if (Callee->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto* ma = static_cast<const MemberAccessAST*>(Callee.get());
    return "Call\n." + ma->getMemberName() + "()";
  }
  return "Call";
}

std::unique_ptr<ExprAST> PackExpansionAST::clone() const {
  auto copy = std::make_unique<PackExpansionAST>(packName);
  cloneBase(*copy);
  return copy;
}

// -------------------------------------------------------------------
// Control flow AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> IfExprAST::clone() const {
  auto copy = std::make_unique<IfExprAST>(Cond->clone(), Then->clone(),
                                          Else ? Else->clone() : nullptr);
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> MatchExprAST::clone() const {
  std::vector<MatchArm> armsClone;
  armsClone.reserve(arms.size());
  for (const auto& arm : arms) {
    armsClone.emplace_back(arm.pattern ? arm.pattern->clone() : nullptr,
                           arm.isWildcard, arm.body->clone());
  }
  auto copy = std::make_unique<MatchExprAST>(discriminant->clone(),
                                             std::move(armsClone));
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> ForExprAST::clone() const {
  auto copy = std::make_unique<ForExprAST>(
      Init ? Init->clone() : nullptr, Condition ? Condition->clone() : nullptr,
      Increment ? Increment->clone() : nullptr, Body->clone());
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> ForInExprAST::clone() const {
  auto copy = std::make_unique<ForInExprAST>(LoopVar, LoopVarType,
                                             Iterable->clone(), Body->clone());
  cloneBase(*copy);
  if (hasResolvedLoopVarType()) {
    copy->setResolvedLoopVarType(getResolvedLoopVarType());
  }
  return copy;
}

std::unique_ptr<ExprAST> WhileExprAST::clone() const {
  auto copy = std::make_unique<WhileExprAST>(Condition->clone(), Body->clone());
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> BlockExprAST::clone() const {
  auto copy = std::make_unique<BlockExprAST>(cloneExprVector(Body));
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> UnsafeBlockAST::clone() const {
  auto bodyClone = std::unique_ptr<BlockExprAST>(
      static_cast<BlockExprAST*>(body->clone().release()));
  auto copy = std::make_unique<UnsafeBlockAST>(std::move(bodyClone));
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> IndexedAssignmentAST::clone() const {
  auto copy =
      std::make_unique<IndexedAssignmentAST>(target->clone(), value->clone());
  cloneBase(*copy);
  return copy;
}

// -------------------------------------------------------------------
// Function and Prototype AST nodes
// -------------------------------------------------------------------

std::unique_ptr<PrototypeAST> PrototypeAST::clone() const {
  // Clone args
  std::vector<std::pair<std::string, TypeAnnotation>> argsClone;
  argsClone.reserve(args.size());
  for (const auto& [argName, argType] : args) {
    argsClone.emplace_back(argName, argType);  // TypeAnnotation has copy ctor
  }

  // Clone return type
  std::optional<TypeAnnotation> retTypeClone;
  if (returnType) {
    retTypeClone = *returnType;
  }

  // Clone variadic constraint
  std::optional<TypeAnnotation> variadicConstraintClone;
  if (variadicConstraint_) {
    variadicConstraintClone = *variadicConstraint_;
  }

  auto copy = std::make_unique<PrototypeAST>(
      Name, std::move(argsClone), std::move(retTypeClone), typeParameters,
      variadicParamName_, std::move(variadicConstraintClone));
  copy->setCaptures(captures);

  // Copy analysis data
  if (hasAnalysis()) {
    const auto* a = getAnalysis();
    if (a->resolvedParamTypesSet) {
      copy->setResolvedParamTypes(a->resolvedParamTypes);
    }
    if (a->resolvedReturnType) {
      copy->setResolvedReturnType(a->resolvedReturnType);
    }
    if (!a->resolvedVariadicTypes.empty()) {
      copy->setResolvedVariadicTypes(a->resolvedVariadicTypes);
    }
    if (!a->typeBindings.empty()) {
      copy->setTypeBindings(a->typeBindings);
    }
    if (!a->qualifiedName.empty()) {
      copy->setQualifiedName(a->qualifiedName);
    }
  }

  return copy;
}

std::unique_ptr<ExprAST> FunctionAST::clone() const {
  auto protoClone = Proto->clone();
  std::unique_ptr<BlockExprAST> bodyClone;
  if (Body) {
    auto bodyExpr = Body->clone();
    bodyClone.reset(static_cast<BlockExprAST*>(bodyExpr.release()));
  }
  auto copy = std::make_unique<FunctionAST>(std::move(protoClone),
                                            std::move(bodyClone));
  cloneBase(*copy);
  copy->setSourceText(sourceText_);
  return copy;
}

std::unique_ptr<ExprAST> LambdaAST::clone() const {
  auto protoClone = Proto->clone();
  std::unique_ptr<BlockExprAST> bodyClone;
  if (Body) {
    auto bodyExpr = Body->clone();
    bodyClone.reset(static_cast<BlockExprAST*>(bodyExpr.release()));
  }
  auto copy =
      std::make_unique<LambdaAST>(std::move(protoClone), std::move(bodyClone));
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> SpawnExprAST::clone() const {
  auto copy = std::make_unique<SpawnExprAST>(Lambda->clone());
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> ReturnExprAST::clone() const {
  auto copy = std::make_unique<ReturnExprAST>(Value ? Value->clone() : nullptr);
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> BreakAST::clone() const {
  auto copy = std::make_unique<BreakAST>();
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> ContinueAST::clone() const {
  auto copy = std::make_unique<ContinueAST>();
  cloneBase(*copy);
  return copy;
}

// -------------------------------------------------------------------
// Exception handling AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> ThrowExprAST::clone() const {
  auto copy = std::make_unique<ThrowExprAST>(errorExpr->clone());
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> TryCatchExprAST::clone() const {
  auto tryClone = tryBlock->clone();
  std::unique_ptr<BlockExprAST> tryBlockClone(
      static_cast<BlockExprAST*>(tryClone.release()));

  CatchClause catchClone;
  catchClone.bindingName = catchClause.bindingName;
  if (catchClause.bindingType) {
    catchClone.bindingType = *catchClause.bindingType;
  }
  auto bodyClone = catchClause.body->clone();
  catchClone.body.reset(static_cast<BlockExprAST*>(bodyClone.release()));

  auto copy = std::make_unique<TryCatchExprAST>(std::move(tryBlockClone),
                                                std::move(catchClone));
  cloneBase(*copy);
  return copy;
}

// -------------------------------------------------------------------
// Module/namespace AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> ImportAST::clone() const {
  auto copy = std::make_unique<ImportAST>(path);
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> ImportScopeAST::clone() const {
  auto bodyClone = std::unique_ptr<BlockExprAST>(
      static_cast<BlockExprAST*>(body->clone().release()));
  auto copy = std::make_unique<ImportScopeAST>(sourceFile, std::move(bodyClone),
                                               contentHash);
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> QualifiedNameAST::clone() const {
  auto copy = std::make_unique<QualifiedNameAST>(parts);
  cloneBase(*copy);
  if (hasAnalysis()) {
    copy->setResolvedMangledName(
        static_cast<QualifiedNameExprAnalysis&>(*analysis_)
            .resolvedMangledName);
  }
  return copy;
}

std::unique_ptr<ExprAST> ModuleAST::clone() const {
  auto bodyClone = body->clone();
  std::unique_ptr<BlockExprAST> bodyBlockClone(
      static_cast<BlockExprAST*>(bodyClone.release()));
  auto copy = std::make_unique<ModuleAST>(name, std::move(bodyBlockClone));
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> UsingAST::clone() const {
  auto copy = std::make_unique<UsingAST>(namespacePath, target);
  cloneBase(*copy);
  return copy;
}

// -------------------------------------------------------------------
// Class/Interface/Enum definition AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> ClassDefinitionAST::clone() const {
  // Clone fields
  std::vector<ClassFieldDecl> fieldsClone;
  fieldsClone.reserve(fields.size());
  for (const auto& field : fields) {
    fieldsClone.push_back({field.name, field.type, field.location});
  }

  // Clone methods
  std::vector<ClassMethodDecl> methodsClone;
  methodsClone.reserve(methods.size());
  for (const auto& method : methods) {
    ClassMethodDecl methodClone;
    auto funcClone = method.function->clone();
    methodClone.function.reset(static_cast<FunctionAST*>(funcClone.release()));
    methodClone.isConstructor = method.isConstructor;
    methodsClone.push_back(std::move(methodClone));
  }

  // Clone implemented interfaces
  std::vector<ImplementedInterfaceAST> interfacesClone;
  interfacesClone.reserve(implementedInterfaces.size());
  for (const auto& iface : implementedInterfaces) {
    ImplementedInterfaceAST ifaceClone;
    ifaceClone.name = iface.name;
    for (const auto& ta : iface.typeArguments) {
      ifaceClone.typeArguments.push_back(ta);  // TypeAnnotation has copy ctor
    }
    interfacesClone.push_back(std::move(ifaceClone));
  }

  auto copy = std::make_unique<ClassDefinitionAST>(
      name, typeParameters, std::move(interfacesClone), std::move(fieldsClone),
      std::move(methodsClone));
  cloneBase(*copy);
  if (hasQualifiedName()) {
    copy->setQualifiedName(getQualifiedName());
  }
  return copy;
}

std::unique_ptr<ExprAST> InterfaceDefinitionAST::clone() const {
  // Clone fields
  std::vector<InterfaceFieldDecl> fieldsClone;
  fieldsClone.reserve(fields.size());
  for (const auto& field : fields) {
    fieldsClone.push_back({field.name, field.type, field.location});
  }

  // Clone methods
  std::vector<InterfaceMethodDecl> methodsClone;
  methodsClone.reserve(methods.size());
  for (const auto& method : methods) {
    InterfaceMethodDecl methodClone;
    auto funcClone = method.function->clone();
    methodClone.function.reset(static_cast<FunctionAST*>(funcClone.release()));
    methodClone.hasDefaultImpl = method.hasDefaultImpl;
    methodsClone.push_back(std::move(methodClone));
  }

  auto copy = std::make_unique<InterfaceDefinitionAST>(
      name, typeParameters, std::move(fieldsClone), std::move(methodsClone));
  cloneBase(*copy);
  if (hasQualifiedName()) {
    copy->setQualifiedName(getQualifiedName());
  }
  return copy;
}

std::unique_ptr<ExprAST> EnumDefinitionAST::clone() const {
  // Clone variants
  std::vector<EnumVariantDecl> variantsClone;
  variantsClone.reserve(variants.size());
  for (const auto& variant : variants) {
    variantsClone.push_back({variant.name, variant.value, variant.location});
  }

  auto copy =
      std::make_unique<EnumDefinitionAST>(name, std::move(variantsClone));
  cloneBase(*copy);
  return copy;
}

// -------------------------------------------------------------------
// Member access AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> MemberAccessAST::clone() const {
  auto copy = std::make_unique<MemberAccessAST>(
      object->clone(), memberName, cloneTypeAnnotationVector(typeArguments));
  cloneBase(*copy);
  if (hasResolvedTypeArgs()) {
    copy->setResolvedTypeArgs(getResolvedTypeArgs());
  }
  if (hasResolvedQualifiedName()) {
    copy->setResolvedQualifiedName(getResolvedQualifiedName());
  }
  return copy;
}

std::unique_ptr<ExprAST> ThisExprAST::clone() const {
  auto copy = std::make_unique<ThisExprAST>();
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> GenericCallAST::clone() const {
  auto copy = std::make_unique<GenericCallAST>(
      functionName, cloneTypeAnnotationVector(typeArguments),
      cloneExprVector(args));
  cloneBase(*copy);
  if (hasGenericFunctionAST()) {
    copy->setGenericFunctionAST(getGenericFunctionAST());
  }
  if (hasResolvedTypeArgs()) {
    copy->setResolvedTypeArgs(getResolvedTypeArgs());
  }
  return copy;
}

std::unique_ptr<ExprAST> MemberAssignmentAST::clone() const {
  auto copy = std::make_unique<MemberAssignmentAST>(object->clone(), memberName,
                                                    value->clone());
  cloneBase(*copy);
  return copy;
}

std::unique_ptr<ExprAST> DeclareTypeAST::clone() const {
  auto copy = std::make_unique<DeclareTypeAST>(typeAnnotation, aliasName);
  cloneBase(*copy);
  if (hasResolvedDeclaredType()) {
    copy->setResolvedDeclaredType(getResolvedDeclaredType());
  }
  return copy;
}
