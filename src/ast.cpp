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
  if (isInteger()) {
    auto copy = std::make_unique<NumberExprAST>(getIntVal());
    copy->setLocation(location_);
    copy->setResolvedType(resolvedType);
    return copy;
  }
  auto copy = std::make_unique<NumberExprAST>(getFloatVal());
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> StringLiteralAST::clone() const {
  auto copy = std::make_unique<StringLiteralAST>(Value);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> NullLiteralAST::clone() const {
  auto copy = std::make_unique<NullLiteralAST>();
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> BoolLiteralAST::clone() const {
  auto copy = std::make_unique<BoolLiteralAST>(Value);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> ArrayLiteralAST::clone() const {
  auto copy = std::make_unique<ArrayLiteralAST>(cloneExprVector(elements));
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

// -------------------------------------------------------------------
// Index/Slice AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> ArrayIndexAST::clone() const {
  auto copy =
      std::make_unique<ArrayIndexAST>(array->clone(), cloneExprVector(indices));
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> SliceExprAST::clone() const {
  std::unique_ptr<ExprAST> startClone = start_ ? start_->clone() : nullptr;
  std::unique_ptr<ExprAST> endClone = end_ ? end_->clone() : nullptr;
  std::unique_ptr<SliceExprAST> copy;
  if (isRange_) {
    copy = std::make_unique<SliceExprAST>(std::move(startClone),
                                          std::move(endClone), true);
  } else {
    copy = std::make_unique<SliceExprAST>(std::move(startClone));
  }
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
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
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

// -------------------------------------------------------------------
// Variable AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> VariableReferenceAST::clone() const {
  auto copy = std::make_unique<VariableReferenceAST>(Name);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> VariableCreationAST::clone() const {
  std::optional<TypeAnnotation> typeClone;
  if (typeAnnotation) {
    typeClone = *typeAnnotation;  // TypeAnnotation has copy constructor
  }
  auto copy = std::make_unique<VariableCreationAST>(name, value->clone(),
                                                    std::move(typeClone));
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> VariableAssignmentAST::clone() const {
  auto copy = std::make_unique<VariableAssignmentAST>(name, value->clone());
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> ReferenceCreationAST::clone() const {
  auto copy = std::make_unique<ReferenceCreationAST>(name, target->clone(),
                                                     mutable_, location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

// -------------------------------------------------------------------
// Expression AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> BinaryExprAST::clone() const {
  auto copy = std::make_unique<BinaryExprAST>(op, LHS->clone(), RHS->clone());
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> UnaryExprAST::clone() const {
  auto copy = std::make_unique<UnaryExprAST>(op, Operand->clone());
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> CallExprAST::clone() const {
  auto copy =
      std::make_unique<CallExprAST>(Callee->clone(), cloneExprVector(Args));
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> PackExpansionAST::clone() const {
  auto copy = std::make_unique<PackExpansionAST>(packName);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

// -------------------------------------------------------------------
// Control flow AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> IfExprAST::clone() const {
  auto copy = std::make_unique<IfExprAST>(Cond->clone(), Then->clone(),
                                          Else ? Else->clone() : nullptr);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
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
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> ForExprAST::clone() const {
  auto copy = std::make_unique<ForExprAST>(
      Init ? Init->clone() : nullptr, Condition ? Condition->clone() : nullptr,
      Increment ? Increment->clone() : nullptr, Body->clone());
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> ForInExprAST::clone() const {
  auto copy = std::make_unique<ForInExprAST>(LoopVar, LoopVarType,
                                             Iterable->clone(), Body->clone());
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  copy->setResolvedLoopVarType(resolvedLoopVarType_);
  return copy;
}

std::unique_ptr<ExprAST> WhileExprAST::clone() const {
  auto copy = std::make_unique<WhileExprAST>(Condition->clone(), Body->clone());
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> BlockExprAST::clone() const {
  auto copy = std::make_unique<BlockExprAST>(cloneExprVector(Body));
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> IndexedAssignmentAST::clone() const {
  auto copy =
      std::make_unique<IndexedAssignmentAST>(target->clone(), value->clone());
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
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

  // Copy resolved types for specialized generic functions
  if (resolvedParamTypesSet_) {
    copy->setResolvedParamTypes(resolvedParamTypes_);
  }
  if (resolvedReturnType_) {
    copy->setResolvedReturnType(resolvedReturnType_);
  }
  if (!resolvedVariadicTypes_.empty()) {
    copy->setResolvedVariadicTypes(resolvedVariadicTypes_);
  }
  if (!typeBindings_.empty()) {
    copy->setTypeBindings(typeBindings_);
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
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  copy->setSourceText(sourceText_);
  copy->setPrecompiled(precompiled_);
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
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  copy->setPrecompiled(precompiled_);
  return copy;
}

std::unique_ptr<ExprAST> ReturnExprAST::clone() const {
  auto copy = std::make_unique<ReturnExprAST>(Value ? Value->clone() : nullptr);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> BreakAST::clone() const {
  auto copy = std::make_unique<BreakAST>();
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> ContinueAST::clone() const {
  auto copy = std::make_unique<ContinueAST>();
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

// -------------------------------------------------------------------
// Exception handling AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> ThrowExprAST::clone() const {
  auto copy = std::make_unique<ThrowExprAST>(errorExpr->clone());
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
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
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

// -------------------------------------------------------------------
// Module/namespace AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> ImportAST::clone() const {
  auto copy = std::make_unique<ImportAST>(path);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> ImportScopeAST::clone() const {
  auto bodyClone = std::unique_ptr<BlockExprAST>(
      static_cast<BlockExprAST*>(body->clone().release()));
  auto copy =
      std::make_unique<ImportScopeAST>(sourceFile, std::move(bodyClone));
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> QualifiedNameAST::clone() const {
  auto copy = std::make_unique<QualifiedNameAST>(parts);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> NamespaceAST::clone() const {
  auto bodyClone = body->clone();
  std::unique_ptr<BlockExprAST> bodyBlockClone(
      static_cast<BlockExprAST*>(bodyClone.release()));
  auto copy = std::make_unique<NamespaceAST>(name, std::move(bodyBlockClone));
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> UsingAST::clone() const {
  auto copy = std::make_unique<UsingAST>(namespacePath, target);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
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
      std::move(methodsClone), precompiled_);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
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
      name, typeParameters, std::move(fieldsClone), std::move(methodsClone),
      precompiled_);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> EnumDefinitionAST::clone() const {
  // Clone variants
  std::vector<EnumVariantDecl> variantsClone;
  variantsClone.reserve(variants.size());
  for (const auto& variant : variants) {
    variantsClone.push_back({variant.name, variant.value, variant.location});
  }

  auto copy = std::make_unique<EnumDefinitionAST>(
      name, std::move(variantsClone), precompiled_);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

// -------------------------------------------------------------------
// Member access AST nodes
// -------------------------------------------------------------------

std::unique_ptr<ExprAST> MemberAccessAST::clone() const {
  auto copy = std::make_unique<MemberAccessAST>(
      object->clone(), memberName, cloneTypeAnnotationVector(typeArguments));
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  copy->setResolvedTypeArgs(resolvedTypeArgs_);
  return copy;
}

std::unique_ptr<ExprAST> ThisExprAST::clone() const {
  auto copy = std::make_unique<ThisExprAST>();
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> GenericCallAST::clone() const {
  auto copy = std::make_unique<GenericCallAST>(
      functionName, cloneTypeAnnotationVector(typeArguments),
      cloneExprVector(args));
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  copy->setGenericFunctionAST(genericFunctionAST_);
  copy->setResolvedTypeArgs(resolvedTypeArgs_);
  return copy;
}

std::unique_ptr<ExprAST> MemberAssignmentAST::clone() const {
  auto copy = std::make_unique<MemberAssignmentAST>(object->clone(), memberName,
                                                    value->clone());
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  return copy;
}

std::unique_ptr<ExprAST> DeclareTypeAST::clone() const {
  auto copy = std::make_unique<DeclareTypeAST>(typeAnnotation, aliasName);
  copy->setLocation(location_);
  copy->setResolvedType(resolvedType);
  copy->setResolvedDeclaredType(resolvedDeclaredType_);
  return copy;
}
