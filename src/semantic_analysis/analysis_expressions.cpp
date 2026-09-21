// analysis_expressions.cpp — Expressions that produce a value: literals,
// operators, indexing and member access
//
// One handler per AST node kind, called from the dispatcher in
// analysis.cpp.

#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "semantic_analysis/type_analysis/type_rules.h"
#include "support/error.h"

using sun::types::TypePtr;
using sun::types::Types;

using sun::ast::ExprAST;
using sun::parsing::TokenKind;
using sun::support::logAndThrowError;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
using sun::types::ArrayType;
using sun::types::ClassType;

using sun::semantic_analysis::type_analysis::checkCharOperands;
using sun::semantic_analysis::type_analysis::coerceBinaryLiteralOperands;
using sun::semantic_analysis::type_analysis::tryCoerceIntegerLiteral;
using sun::types::unwrapRef;

void SemanticAnalyzer::analyzeNumberLiteral(ExprAST& expr,
                                            TypePtr expectedType) {
  // A suffixed literal (21u8, 1.5f32) is already typed: it ignores the
  // expected type, and an integer value that does not fit its suffix is an
  // error here. Floats skip the range check — rounding is inherent to them.
  const auto& num = static_cast<const sun::ast::NumberExprAST&>(expr);
  if (num.hasSuffix()) {
    TypePtr suffixType = Types::fromString(num.getSuffix());
    if (num.isInteger()) {
      tryCoerceIntegerLiteral(&expr, suffixType, /*throwOnFail=*/true);
    } else {
      expr.setResolvedType(suffixType);
    }
    return;
  }

  // If we have an expected type, try to use it for integer literals
  if (expectedType && expectedType->isPrimitive()) {
    if (tryCoerceIntegerLiteral(&expr, expectedType, false)) {
      return;
    }
  }
  expr.setResolvedType(requireInferredType(
      sun::semantic_analysis::type_analysis::TypeInferer::number(
          num.isInteger(), num.isInteger() ? num.getMagnitude() : 0,
          num.isInteger() && num.isNegative()),
      expr.getLocation(), "Cannot determine numeric literal type"));
  if (num.isInteger() && num.isNegative()) {
    // A negative magnitude beyond the signed range cannot default to u64.
    tryCoerceIntegerLiteral(&expr, expr.getResolvedType(),
                            /*throwOnFail=*/true);
  }
}

void SemanticAnalyzer::analyzeArrayLiteral(sun::ast::ArrayLiteralAST& arrLit,
                                           TypePtr expectedType) {
  // Analyze each element; a compound element moves into the literal
  for (const auto& elem : arrLit.getElements()) {
    analyzeExpr(const_cast<ExprAST&>(*elem));
    checkMoveSource(*elem, arrLit.getLocation());
  }
  resolveArrayLiteralResult(arrLit, expectedType);
}

void SemanticAnalyzer::resolveArrayLiteralResult(
    sun::ast::ArrayLiteralAST& arrLit, TypePtr expectedType) {
  TypePtr first = arrLit.getElements().empty()
                      ? nullptr
                      : requireResolvedType(*arrLit.getElements().front());
  TypePtr expectedElement;
  if (auto hint = unwrapRef(expectedType); hint && hint->isArray())
    expectedElement = static_cast<const ArrayType&>(*hint).getElementType();
  bool widen = first && expectedElement &&
               ((expectedElement->isInt64() && first->isInt32()) ||
                (expectedElement->isFloat64() && first->isFloat32()) ||
                expectedElement->equals(*first));
  arrLit.setResolvedType(requireInferredType(
      sun::semantic_analysis::type_analysis::TypeInferer::array(
          first, arrLit.getElements().size(), expectedElement, widen),
      arrLit.getLocation(),
      arrLit.getElements().empty() ? "Cannot infer type of empty array literal"
                                   : "Cannot infer array element type"));
}

void SemanticAnalyzer::analyzeIndexExpr(sun::ast::IndexAST& arrIdx) {
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
    const_cast<sun::ast::SliceExprAST&>(*idx).setResolvedType(Types::Slice());
  }
  // `c[i]` on a class calls __index__ / __slice__, a method like any
  // other: it needs a mutable receiver unless declared const, and a
  // `ref T` result seen through a constant receiver is `const ref T`
  bool receiverImmutable = false;
  TypePtr targetType = unwrapRef(arrIdx.getTarget()->getResolvedType());
  if (targetType && targetType->isClass()) {
    const auto* classType =
        static_cast<const sun::types::ClassType*>(targetType.get());
    const char* opName = arrIdx.hasSlices() ? "__slice__" : "__index__";
    if (const auto* method = classType->getMethod(opName)) {
      checkUnsafeCall(method->isUnsafe, opName, arrIdx.getLocation());
      receiverImmutable =
          checkMethodReceiver(*arrIdx.getTarget(), opName, method->isConst,
                              /*isConstructor=*/false, arrIdx.getLocation());
    }
  }
  // Set resolved type (element type of the array)
  TypePtr resultType;
  if (targetType && targetType->isClass()) {
    const auto& cls = static_cast<const ClassType&>(*targetType);
    const char* methodName = arrIdx.hasSlices() ? "__slice__" : "__index__";
    const auto* method =
        ctx_.accessibleMethod(cls, methodName, arrIdx.getLocation());
    if (!method)
      logAndThrowError(
          "Class " + cls.getDisplayName() + " does not implement " +
              methodName +
              (arrIdx.hasSlices() ? " for slicing" : " for indexing"),
          arrIdx.getLocation());
    arrIdx.setTargetDeclarationId(method->declarationId);
    resultType = method->returnType;
  } else {
    auto result = sun::semantic_analysis::type_analysis::TypeInferer::index(
        targetType, arrIdx.getIndices().size());
    const auto* failure =
        std::get_if<sun::semantic_analysis::type_analysis::InferenceFailure>(
            &result);
    bool wrongDimensions =
        failure && failure->kind == sun::semantic_analysis::type_analysis::
                                        InferenceFailure::Kind::IndexDimensions;
    resultType = requireInferredType(
        std::move(result), arrIdx.getLocation(),
        wrongDimensions ? "Array index count does not match dimensions"
                        : "Cannot index non-array type");
  }
  if (receiverImmutable) resultType = resolver_.createConstView(resultType);
  arrIdx.setResolvedType(resultType);
}

void SemanticAnalyzer::analyzeSliceExpr(ExprAST& expr) {
  // SliceExprAST can appear standalone in some contexts
  auto& sliceExpr = static_cast<sun::ast::SliceExprAST&>(expr);
  if (sliceExpr.hasStart()) {
    analyzeExpr(const_cast<ExprAST&>(*sliceExpr.getStart()));
  }
  if (sliceExpr.hasEnd()) {
    analyzeExpr(const_cast<ExprAST&>(*sliceExpr.getEnd()));
  }
  expr.setResolvedType(Types::Slice());
}

void SemanticAnalyzer::analyzeBinaryExpr(sun::ast::BinaryExprAST& binExpr,
                                         TypePtr expectedType) {
  analyzeExpr(const_cast<ExprAST&>(*binExpr.getLHS()));
  analyzeExpr(const_cast<ExprAST&>(*binExpr.getRHS()));

  // Payload enums have no structural equality; match is the eliminator
  TokenKind binOp = binExpr.getOp().kind;
  if (binOp == TokenKind::EQUAL_EQUAL || binOp == TokenKind::NOT_EQUAL) {
    for (const ExprAST* side : {binExpr.getLHS(), binExpr.getRHS()}) {
      TypePtr sideType = unwrapRef(side->getResolvedType());
      if (sideType && sideType->isEnum() &&
          static_cast<sun::types::EnumType*>(sideType.get())->hasPayload()) {
        logAndThrowError("Cannot compare enum '" +
                             static_cast<sun::types::EnumType*>(sideType.get())
                                 ->getDisplayName() +
                             "' with '==' ; use match to inspect payload enums",
                         binExpr.getLocation());
      }
    }
  }
  checkCharOperands(binExpr);
  coerceBinaryLiteralOperands(binExpr, expectedType);
  bool comparison =
      binOp == TokenKind::LESS || binOp == TokenKind::GREATER ||
      binOp == TokenKind::LESS_EQUAL || binOp == TokenKind::GREATER_EQUAL ||
      binOp == TokenKind::EQUAL_EQUAL || binOp == TokenKind::NOT_EQUAL;
  binExpr.setResolvedType(
      comparison
          ? Types::Bool()
          : requireInferredType(
                sun::semantic_analysis::type_analysis::TypeInferer::numeric(
                    requireResolvedType(*binExpr.getLHS()),
                    requireResolvedType(*binExpr.getRHS())),
                binExpr.getLocation(),
                "Cannot determine binary operand types"));
}

void SemanticAnalyzer::analyzeUnaryExpr(sun::ast::UnaryExprAST& unaryExpr) {
  analyzeExpr(const_cast<ExprAST&>(*unaryExpr.getOperand()));

  TokenKind op = unaryExpr.getOp().kind;
  auto operandType =
      sun::types::unwrapRef(requireResolvedType(*unaryExpr.getOperand()));

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

  unaryExpr.setResolvedType(requireInferredType(
      sun::semantic_analysis::type_analysis::TypeInferer::unary(
          operandType, op == TokenKind::NOT),
      unaryExpr.getLocation(), "Cannot determine unary operand type"));
}

void SemanticAnalyzer::analyzeMemberAccess(
    sun::ast::MemberAccessAST& memberAccess, TypePtr expectedType) {
  // Check for enum variant access: EnumName.VariantName
  // Don't try to analyze the "object" if it's an enum type name
  if (enums_.tryAnalyzeGenericEnumUnitVariant(memberAccess, expectedType))
    return;
  bool isEnumAccess = false;
  if (memberAccess.getObject()->getType() ==
      sun::ast::ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef = static_cast<const sun::ast::VariableReferenceAST&>(
        *memberAccess.getObject());
    if (auto enumType = ctx_.lookupEnum(varRef.getName())) {
      const_cast<ExprAST&>(*memberAccess.getObject()).setResolvedType(enumType);
      isEnumAccess = true;
    } else if (enums_.tryAnalyzeGenericEnumUnitVariant(memberAccess,
                                                       expectedType)) {
      // Generic enum unit variant (Option.None): resolved from expected
      // type in EnumAnalyzer
      return;
    }
  }

  if (!isEnumAccess) {
    // Analyze the object expression (only if not enum access)
    analyzeExpr(const_cast<ExprAST&>(*memberAccess.getObject()));
  }

  // A module-qualified function name can denote an overload set. Select the
  // member from the expected function-pointer signature before ordinary type
  // inference falls back to the first overload.
  TypePtr objectType = memberAccess.getObject()->getResolvedType();
  if (objectType && objectType->isModule()) {
    const auto* moduleType =
        static_cast<const sun::types::ModuleType*>(objectType.get());
    SymbolMatch match = ctx_.findSymbolInModule(moduleType->getModulePath(),
                                                memberAccess.getMemberName(),
                                                SymbolKind::Variable);
    if (match && match.variableInfo) {
      checkExternVariableAccessAllowed(*match.variableInfo, match.display(),
                                       memberAccess.getLocation());
      memberAccess.setQualifiedName(match.variableInfo->qualifiedName);
      memberAccess.setTargetDeclarationId(match.variableInfo->declarationId);
    }
  }
  if (objectType && objectType->isModule() && expectedType &&
      expectedType->isFunction()) {
    const auto* moduleType =
        static_cast<const sun::types::ModuleType*>(objectType.get());
    const auto* expectedFunction =
        static_cast<const sun::types::FunctionType*>(expectedType.get());
    SymbolMatch match = ctx_.findSymbolInModule(
        moduleType->getModulePath(), memberAccess.getMemberName(),
        SymbolKind::Function, &expectedFunction->getParamTypes());
    if (match && match.functionInfo) {
      TypePtr candidate = Types::Function(match.functionInfo->returnType,
                                          match.functionInfo->paramTypes,
                                          match.functionInfo->canThrow);
      if (sun::semantic_analysis::type_analysis::isAssignableTo(candidate,
                                                                expectedType)) {
        memberAccess.setQualifiedName(match.functionInfo->qualifiedName);
        memberAccess.setTargetDeclarationId(match.functionInfo->declarationId);
        memberAccess.setResolvedType(candidate);
        return;
      }
    }
    logAndThrowError("No overload of '" + memberAccess.getMemberName() +
                         "' matches expected type '" +
                         expectedType->toDisplayString() + "'",
                     memberAccess.getLocation());
  }

  memberAccess.setResolvedType(resolveMemberType(memberAccess));
  // A method in value position becomes a bound method reference with
  // lambda type (call-position callees don't route through this case).
  maybeResolveBoundMethodRef(memberAccess, expectedType);
}

void SemanticAnalyzer::analyzeQualifiedName(
    sun::ast::QualifiedNameAST& qualName) {
  std::string fullName = qualName.getFullName();

  // Look up in namespaced variables first
  VariableInfo* varInfo = ctx_.lookupQualifiedVariable(fullName);
  if (varInfo) {
    checkExternVariableAccessAllowed(*varInfo, fullName,
                                     qualName.getLocation());
    qualName.setTargetDeclarationId(varInfo->declarationId);
    qualName.setResolvedType(varInfo->type);
    return;
  }

  // Look up in namespaced functions - this searches all matching module
  // scopes (including same-named modules in different import scopes)
  const FunctionInfo* funcInfo = ctx_.lookupQualifiedFunction(fullName);
  if (funcInfo) {
    qualName.setTargetDeclarationId(funcInfo->declarationId);
    qualName.setResolvedType(Types::Function(
        funcInfo->returnType, funcInfo->paramTypes, funcInfo->canThrow));
    return;
  }

  // Unknown qualified name - default to f64
  qualName.setResolvedType(Types::Float64());
}

}  // namespace sun::semantic_analysis
