// analysis_expressions.cpp — Expressions that produce a value: literals,
// operators, indexing and member access
//
// One handler per AST node kind, called from the dispatcher in
// analysis.cpp.

#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_rules.h"
#include "support/error.h"

using sun::unwrapRef;
using sun::rules::checkCharOperands;
using sun::rules::coerceBinaryLiteralOperands;
using sun::rules::tryCoerceIntegerLiteral;

void SemanticAnalyzer::analyzeNumberLiteral(ExprAST& expr,
                                            sun::TypePtr expectedType) {
  // If we have an expected type, try to use it for integer literals
  if (expectedType && expectedType->isPrimitive()) {
    if (tryCoerceIntegerLiteral(&expr, expectedType, false)) {
      return;
    }
  }
  expr.setResolvedType(types_.inferType(expr));
}

void SemanticAnalyzer::analyzeArrayLiteral(ArrayLiteralAST& arrLit) {
  // Analyze each element
  for (const auto& elem : arrLit.getElements()) {
    analyzeExpr(const_cast<ExprAST&>(*elem));
  }
  // Always use inferType - it will use any expected type hint (from
  // function parameter) to widen element types if needed, while computing
  // proper dimensions
  arrLit.setResolvedType(types_.inferType(arrLit));
}

void SemanticAnalyzer::analyzeIndexExpr(IndexAST& arrIdx) {
  // Analyze the target expression
  analyzeExpr(const_cast<ExprAST&>(*arrIdx.getTarget()));
  // Analyze each index/slice expression and set slice type
  for (const auto& idx : arrIdx.getIndices()) {
    if (idx->hasStart()) {
      analyzeExpr(const_cast<ExprAST&>(*idx->getStart()));
    }
    if (idx->hasEnd()) {
      analyzeExpr(const_cast<ExprAST&>(*idx->getEnd()));
    }
    // Each SliceExprAST resolves to the slice type
    const_cast<SliceExprAST&>(*idx).setResolvedType(sun::Types::Slice());
  }
  // `c[i]` on a class calls __index__ / __slice__, a method like any
  // other: it needs a mutable receiver unless declared const, and a
  // `ref T` result seen through a constant receiver is `const ref T`
  bool receiverImmutable = false;
  sun::TypePtr targetType = unwrapRef(arrIdx.getTarget()->getResolvedType());
  if (targetType && targetType->isClass()) {
    const auto* classType =
        static_cast<const sun::ClassType*>(targetType.get());
    const char* opName = arrIdx.hasSlices() ? "__slice__" : "__index__";
    if (const auto* method = classType->getMethod(opName)) {
      receiverImmutable =
          checkMethodReceiver(*arrIdx.getTarget(), opName, method->isConst,
                              /*isConstructor=*/false, arrIdx.getLocation());
    }
  }
  // Set resolved type (element type of the array)
  sun::TypePtr resultType = types_.inferType(arrIdx);
  if (receiverImmutable) resultType = types_.createConstView(resultType);
  arrIdx.setResolvedType(resultType);
}

void SemanticAnalyzer::analyzeSliceExpr(ExprAST& expr) {
  // SliceExprAST can appear standalone in some contexts
  auto& sliceExpr = static_cast<SliceExprAST&>(expr);
  if (sliceExpr.hasStart()) {
    analyzeExpr(const_cast<ExprAST&>(*sliceExpr.getStart()));
  }
  if (sliceExpr.hasEnd()) {
    analyzeExpr(const_cast<ExprAST&>(*sliceExpr.getEnd()));
  }
  expr.setResolvedType(sun::Types::Slice());
}

void SemanticAnalyzer::analyzeBinaryExpr(BinaryExprAST& binExpr,
                                         sun::TypePtr expectedType) {
  analyzeExpr(const_cast<ExprAST&>(*binExpr.getLHS()));
  analyzeExpr(const_cast<ExprAST&>(*binExpr.getRHS()));

  // Payload enums have no structural equality; match is the eliminator
  TokenKind binOp = binExpr.getOp().kind;
  if (binOp == TokenKind::EQUAL_EQUAL || binOp == TokenKind::NOT_EQUAL) {
    for (const ExprAST* side : {binExpr.getLHS(), binExpr.getRHS()}) {
      sun::TypePtr sideType = unwrapRef(side->getResolvedType());
      if (sideType && sideType->isEnum() &&
          static_cast<sun::EnumType*>(sideType.get())->hasPayload()) {
        logAndThrowError(
            "Cannot compare enum '" +
                static_cast<sun::EnumType*>(sideType.get())->getDisplayName() +
                "' with '==' ; use match to inspect payload enums",
            binExpr.getLocation());
      }
    }
  }
  checkCharOperands(binExpr);
  coerceBinaryLiteralOperands(binExpr, expectedType);
  binExpr.setResolvedType(types_.inferType(binExpr));
}

void SemanticAnalyzer::analyzeUnaryExpr(UnaryExprAST& unaryExpr) {
  analyzeExpr(const_cast<ExprAST&>(*unaryExpr.getOperand()));

  TokenKind op = unaryExpr.getOp().kind;
  auto operandType = sun::unwrapRef(types_.inferType(*unaryExpr.getOperand()));

  // Unresolved generic operands are validated again at instantiation
  if (operandType && !operandType->isTypeParameter()) {
    const std::string name = operandType->toDisplayString();
    switch (op) {
      case TokenKind::NOT:
        if (!operandType->isBool()) {
          logAndThrowError("'not' requires a bool operand, got '" + name + "'",
                           unaryExpr.getLocation());
        }
        break;
      case TokenKind::TILDE:
        if (!operandType->isIntegral()) {
          logAndThrowError(
              "Bitwise NOT (~) requires an integer operand, got '" + name + "'",
              unaryExpr.getLocation());
        }
        break;
      case TokenKind::MINUS:
        if (!operandType->isNumeric()) {
          logAndThrowError(
              "Unary minus requires a numeric operand, got '" + name + "'",
              unaryExpr.getLocation());
        }
        if (operandType->isUnsigned()) {
          logAndThrowError(
              "Cannot negate a value of unsigned type '" + name + "'",
              unaryExpr.getLocation());
        }
        break;
      default:
        break;
    }
  }

  // Matches inferType's UNARY rule without re-walking the operand subtree
  unaryExpr.setResolvedType(op == TokenKind::NOT ? sun::Types::Bool()
                                                 : operandType);
}

void SemanticAnalyzer::analyzeMemberAccess(MemberAccessAST& memberAccess,
                                           sun::TypePtr expectedType) {
  // Check for enum variant access: EnumName.VariantName
  // Don't try to analyze the "object" if it's an enum type name
  bool isEnumAccess = false;
  if (memberAccess.getObject()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*memberAccess.getObject());
    if (ctx_.lookupEnum(varRef.getName())) {
      isEnumAccess = true;
    } else if (tryAnalyzeGenericEnumUnitVariant(memberAccess, expectedType)) {
      // Generic enum unit variant (Option.None): resolved from expected
      // type in enums.cpp
      return;
    }
  }

  if (!isEnumAccess) {
    // Analyze the object expression (only if not enum access)
    analyzeExpr(const_cast<ExprAST&>(*memberAccess.getObject()));
  }
  memberAccess.setResolvedType(types_.inferType(memberAccess));
  // A method in value position becomes a bound method reference with
  // lambda type (call-position callees don't route through this case).
  maybeResolveBoundMethodRef(memberAccess, expectedType);
}

void SemanticAnalyzer::analyzeQualifiedName(QualifiedNameAST& qualName) {
  std::string fullName = qualName.getFullName();

  // Look up in namespaced variables first
  VariableInfo* varInfo = ctx_.lookupQualifiedVariable(fullName);
  if (varInfo) {
    // Set resolved mangled name from the variable's qualified name
    if (!varInfo->qualifiedName.empty()) {
      qualName.setResolvedMangledName(varInfo->qualifiedName.mangled());
    }
    qualName.setResolvedType(varInfo->type);
    return;
  }

  // Look up in namespaced functions - this searches all matching module
  // scopes (including same-named modules in different import scopes)
  const FunctionInfo* funcInfo = ctx_.lookupQualifiedFunction(fullName);
  if (funcInfo) {
    // Set resolved mangled name from the function's actual qualified name
    // This handles same-named modules in different import scopes correctly
    if (!funcInfo->qualifiedName.empty()) {
      qualName.setResolvedMangledName(funcInfo->qualifiedName.mangled());
    }
    qualName.setResolvedType(sun::Types::Function(
        funcInfo->returnType, funcInfo->paramTypes, funcInfo->canThrow));
    return;
  }

  // Unknown qualified name - default to f64
  qualName.setResolvedType(sun::Types::Float64());
}

void SemanticAnalyzer::analyzeGenericCallExpr(GenericCallAST& genericCall) {
  const std::string& funcName = genericCall.getFunctionName();

  // Resolve the function/class name through using imports
  sun::QualifiedName resolved = ctx_.resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  // Resolve type arguments to sun::TypePtr
  std::vector<sun::TypePtr> typeArgs;
  for (const auto& ta : genericCall.getTypeArguments()) {
    typeArgs.push_back(types_.typeAnnotationToType(*ta));
  }

  // Store resolved type arguments on the AST for codegen
  genericCall.setResolvedTypeArgs(typeArgs);

  // Validate type args
  for (auto& typeArg : typeArgs) {
    validateTypeParameter(typeArg, genericCall);
  }

  // Dispatch based on call type: intrinsic, generic class, or generic
  // function
  bool isIntrinsicCall = sun::isIntrinsic(funcName);
  auto* genericClassInfo = ctx_.lookupGenericClass(lookupName);
  auto* genFuncInfo = ctx_.lookupGenericFunction(lookupName);

  if (isIntrinsicCall) {
    analyzeIntrinsicCall(genericCall);
  } else if (genericClassInfo) {
    analyzeGenericClassConstruction(genericCall);
  } else if (genFuncInfo) {
    analyzeGenericFunctionCall(genericCall);
  } else {
    logAndThrowError("Unknown generic function or class '" + funcName + "'",
                     genericCall.getLocation());
  }
}
