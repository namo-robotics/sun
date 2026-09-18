// type_inferer.cpp — Inferring an expression's type (see type_inferer.h)

#include "semantic_analysis/type_inferer.h"

#include <unordered_set>

#include "ast/control_flow.h"
#include "codegen/intrinsics/intrinsics.h"
#include "semantic_analysis/generic_type_arguments.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/type_rules.h"
#include "semantic_analysis/type_traits.h"
#include "semantic_analysis/visibility.h"
#include "support/error.h"

using sun::semantic_analysis::ClassMethod;
using sun::semantic_analysis::ClassType;
using sun::semantic_analysis::EnumType;
using sun::semantic_analysis::InterfaceField;
using sun::semantic_analysis::InterfaceMethod;
using sun::semantic_analysis::LambdaType;
using sun::semantic_analysis::QualifiedName;
using sun::semantic_analysis::StaticPointerType;
using sun::semantic_analysis::TypePtr;
using sun::semantic_analysis::Types;

using sun::ast::ASTNodeType;
using sun::ast::ExprAST;
using sun::ast::GenericCallAST;
using sun::ast::MemberAccessAST;
using sun::ast::VariableReferenceAST;
using sun::parsing::TokenKind;
using sun::support::logAndThrowError;

namespace sun::semantic_analysis {

using sun::semantic_analysis::promoteBinaryOperands;
using sun::semantic_analysis::unifyTernaryTypes;
using sun::semantic_analysis::unwrapRef;

namespace {
// Resolve an exported module through its portable declaration identity.
TypePtr inferModuleReference(const ExprAST& expr, SemanticContext& context) {
  auto id =
      context.types()->declarations.findPortable(*expr.getModuleDeclaration());
  auto* module = context.lookupModuleScope(id);
  context.requireModuleAccessible(*module, expr.getLocation());
  return Types::Module(
      static_cast<const ModuleScope&>(*module).qualifiedName.lookupName());
}

}  // namespace

TypePtr TypeInferer::inferCallType(const sun::ast::CallExprAST& callExpr) {
  // Analysis records the selected overload on the callee. Reuse that exact
  // signature instead of looking the name up again and taking its first
  // overload, which may have a different return type.
  if (auto selectedType = callExpr.getCallee()->getResolvedType()) {
    if (selectedType->isFunction()) {
      auto returnType =
          static_cast<const sun::semantic_analysis::FunctionType*>(
              selectedType.get())
              ->getReturnType();
      if (!sun::semantic_analysis::mentionsTypeParameter(returnType)) {
        return returnType;
      }
    }
    if (selectedType->isLambda()) {
      auto returnType =
          static_cast<const LambdaType*>(selectedType.get())->getReturnType();
      if (!sun::semantic_analysis::mentionsTypeParameter(returnType)) {
        return returnType;
      }
    }
  }

  // For function calls, check overload resolution FIRST before inferType on
  // callee This avoids errors for overloaded functions referenced by name
  if (callExpr.getCallee()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*callExpr.getCallee());
    // Resolve the name through using imports
    QualifiedName resolved = ctx_.resolveNameWithUsings(varRef.getName());
    // Infer argument types for overload resolution
    std::vector<TypePtr> argTypes;
    for (const auto& arg : callExpr.getArgs()) {
      argTypes.push_back(inferType(*arg));
    }
    std::vector<FunctionArgumentType> lookupTypes;
    lookupTypes.reserve(argTypes.size());
    for (const auto& type : argTypes) lookupTypes.push_back({type, {}});
    auto funcInfo = ctx_.lookupFunction(resolved.baseName, lookupTypes);
    if (funcInfo && funcInfo->returnType) {
      return funcInfo->returnType;
    }
    // Check if this is a stack-allocated class constructor call
    auto classType = ctx_.lookupClass(resolved.baseName);
    if (classType) {
      // Stack-allocated class instantiation: ClassName(args...)
      return classType;
    }
    // A generic function called without type arguments — `identity(42)`.
    // Analysis pinned the callee to the specialization it inferred; the
    // template itself is in no function table to look up.
    if (auto calleeType = varRef.getResolvedType()) {
      if (calleeType->isFunction()) {
        return static_cast<const sun::semantic_analysis::FunctionType*>(
                   calleeType.get())
            ->getReturnType();
      }
    }
  }

  // Module-qualified call (mod.foo(...)): resolve the overload here,
  // where the argument types are known. Inferring the callee alone can
  // only see the first registered overload.
  if (callExpr.getCallee()->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto& memberAccess =
        static_cast<const MemberAccessAST&>(*callExpr.getCallee());
    // Generic enum construction (Option.Some(42)): analysis resolves the
    // specialization; reuse it rather than inferring 'Option' as a value
    if (memberAccess.getObject()->getType() ==
        ASTNodeType::VARIABLE_REFERENCE) {
      const auto& objRef =
          static_cast<const VariableReferenceAST&>(*memberAccess.getObject());
      if (!ctx_.currentScope().lookupVariable(objRef.getName()) &&
          !ctx_.lookupEnum(objRef.getName()) &&
          ctx_.scope()->lookupGenericEnum(objRef.getName())) {
        if (auto resolved = callExpr.getResolvedType()) {
          return resolved;
        }
        if (auto resolvedObj = memberAccess.getObject()->getResolvedType()) {
          return resolvedObj;
        }
        logAndThrowError("Cannot infer type arguments for '" +
                             objRef.getName() + "." +
                             memberAccess.getMemberName() +
                             "'; add a type annotation to the target",
                         callExpr.getLocation());
      }
    }
    TypePtr objectType = inferType(*memberAccess.getObject());
    if (auto* staticPtr = asNonClassStaticPtr(unwrapRef(objectType))) {
      return inferStaticPtrMethodType(*staticPtr, memberAccess.getMemberName(),
                                      callExpr.getArgs().size(),
                                      memberAccess.getLocation());
    }
    if (unwrapRef(objectType) && unwrapRef(objectType)->isArray()) {
      std::vector<TypePtr> argTypes;
      for (const auto& arg : callExpr.getArgs()) {
        argTypes.push_back(inferType(*arg));
      }
      return inferArrayMethodType(memberAccess.getMemberName(), argTypes,
                                  memberAccess.getLocation());
    }
    if (objectType && objectType->isModule()) {
      std::vector<TypePtr> argTypes;
      for (const auto& arg : callExpr.getArgs()) {
        argTypes.push_back(inferType(*arg));
      }
      if (const FunctionInfo* info = sema_.calls().resolveModuleQualifiedCall(
              memberAccess, objectType, argTypes)) {
        return info->returnType;
      }
    }
    // Enum variant construction: EnumName.Variant(args...) has the enum
    // type. Checked in detail by analyzeEnumVariantConstruction.
    if (objectType && objectType->isEnum()) {
      auto* enumType = static_cast<EnumType*>(objectType.get());
      const auto* variant = enumType->getVariant(memberAccess.getMemberName());
      if (variant && variant->hasPayload()) {
        return objectType;
      }
    }
  }

  // Infer the type of the callee expression
  TypePtr calleeType = inferType(*callExpr.getCallee());
  if (calleeType &&
      calleeType->getKind() == sun::semantic_analysis::Type::Kind::Function) {
    const auto* funcType =
        static_cast<const sun::semantic_analysis::FunctionType*>(
            calleeType.get());
    return funcType->getReturnType();
  }
  if (calleeType &&
      calleeType->getKind() == sun::semantic_analysis::Type::Kind::Lambda) {
    const auto* lambdaType = static_cast<const LambdaType*>(calleeType.get());
    return lambdaType->getReturnType();
  }
  // Check if callee type is a class - this is a stack-allocated constructor
  // call
  if (calleeType && calleeType->isClass()) {
    return calleeType;
  }
  // Handle builtin method calls on builtin types (e.g., Thread.join())
  // For these, inferType on the MemberAccess returns the result type
  // directly
  if (callExpr.getCallee()->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto& memberAccess =
        static_cast<const MemberAccessAST&>(*callExpr.getCallee());
    TypePtr objectType = inferType(*memberAccess.getObject());
    const std::string& memberName = memberAccess.getMemberName();

    // If calleeType is non-null but not a function, it might be the return
    // type of a builtin method (e.g., static_ptr.length returns i64)
    if (calleeType) {
      return calleeType;
    }
  }
  // Unknown function - error in strongly typed language
  logAndThrowError("Cannot infer return type for call expression",
                   callExpr.getLocation());
}

TypePtr TypeInferer::inferVariableReferenceType(
    const VariableReferenceAST& varRef) {
  const std::string& name = varRef.getName();

  // Look up as variable
  VariableInfo* info = ctx_.currentScope().lookupVariable(name);
  if (info) {
    varRef.setTargetDeclarationId(info->declarationId);
    // Substitute type parameters to get concrete type (e.g., T -> Box)
    TypePtr originalType = substituteTypeParameters(info->type);

    // Check for type narrowing from _is<T> guards
    // getNarrowedType returns the more specific type (class over interface)
    TypePtr narrowedType = ctx_.getNarrowedType(name, originalType);
    if (narrowedType) {
      return narrowedType;
    }

    return originalType;
  }

  // Check if it's an enum type name (for EnumName.Variant access)
  auto enumType = ctx_.lookupEnum(name);
  if (enumType) {
    return enumType;
  }

  // Resolve the name through using imports (e.g., Vec -> sun_Vec)
  auto resolved = ctx_.resolveNameWithUsings(name);

  // Check if it's a named function
  auto funcs = ctx_.getAllFunctions(resolved.baseName);
  if (funcs.size() == 1) {
    varRef.setTargetDeclarationId(funcs[0].declarationId);
    return Types::Function(funcs[0].returnType, funcs[0].paramTypes,
                           funcs[0].canThrow);
  }
  if (funcs.size() > 1) {
    logAndThrowError("Cannot reference overloaded function '" + name +
                         "' as a value; call it with arguments instead",
                     varRef.getLocation());
  }

  // Check if it's a module name (for mod_x.mod_y.var access)
  if (ctx_.isModuleName(name)) {
    std::string fullPath = ctx_.getFullModulePath(name);
    if (auto* modScope = ctx_.lookupModuleScope(fullPath))
      ctx_.requireModuleAccessible(*modScope, varRef.getLocation());
    return Types::Module(fullPath);
  }

  // Unknown variable - error in strongly typed language
  logAndThrowError("Unknown variable: '" + name + "'", varRef.getLocation());
}

TypePtr TypeInferer::inferIndexType(const sun::ast::IndexAST& arrIdx) {
  TypePtr targetType = inferType(*arrIdx.getTarget());

  // Unwrap reference type if indexing through a reference
  if (targetType && targetType->isReference()) {
    auto* refType = static_cast<const sun::semantic_analysis::ReferenceType*>(
        targetType.get());
    targetType = refType->getReferencedType();
  }

  // Check if target is a class with __index__ or __slice__ method
  if (targetType && targetType->isClass()) {
    auto* classType = static_cast<ClassType*>(targetType.get());
    bool hasSlices = arrIdx.hasSlices();

    if (hasSlices) {
      // Look for __slice__ method
      const ClassMethod* sliceMethod =
          ctx_.accessibleMethod(*classType, "__slice__", arrIdx.getLocation());
      if (sliceMethod) {
        arrIdx.setTargetDeclarationId(sliceMethod->declarationId);
        return sliceMethod->returnType;
      }
      logAndThrowError("Class " + classType->getDisplayName() +
                           " does not implement __slice__ for slicing",
                       arrIdx.getLocation());
      return nullptr;
    } else {
      // Look for __index__ method
      const ClassMethod* indexMethod =
          ctx_.accessibleMethod(*classType, "__index__", arrIdx.getLocation());
      if (indexMethod) {
        arrIdx.setTargetDeclarationId(indexMethod->declarationId);
        return indexMethod->returnType;
      }
      logAndThrowError("Class " + classType->getDisplayName() +
                           " does not implement __index__ for indexing",
                       arrIdx.getLocation());
      return nullptr;
    }
  }

  if (!targetType || !targetType->isArray()) {
    logAndThrowError("Cannot index non-array type", arrIdx.getLocation());
    return nullptr;
  }

  auto* arrayType =
      static_cast<sun::semantic_analysis::ArrayType*>(targetType.get());

  // For unsized arrays, skip dimension count check (any number of indices
  // allowed)
  if (!arrayType->isUnsized()) {
    // Check dimension count matches for sized arrays
    if (arrIdx.getIndices().size() != arrayType->getDimensions().size()) {
      logAndThrowError("Array index count does not match dimensions",
                       arrIdx.getLocation());
      return nullptr;
    }
  }
  // Full indexing returns element type
  return arrayType->getElementType();
}

TypePtr TypeInferer::inferArrayLiteralType(
    const sun::ast::ArrayLiteralAST& arrLit) {
  if (arrLit.getElements().empty()) {
    logAndThrowError("Cannot infer type of empty array literal",
                     arrLit.getLocation());
    return nullptr;
  }

  // Check if there's an expected type set (for type propagation from
  // function parameters)
  TypePtr expectedElemType = nullptr;
  if (arrLit.getResolvedType() && arrLit.getResolvedType()->isArray()) {
    auto* expectedArray = static_cast<sun::semantic_analysis::ArrayType*>(
        arrLit.getResolvedType().get());
    expectedElemType = expectedArray->getElementType();
  }

  // Infer element type from first element
  TypePtr elemType = inferType(*arrLit.getElements()[0]);
  if (!elemType) {
    logAndThrowError("Cannot infer array element type", arrLit.getLocation());
    return nullptr;
  }

  // Use expected element type if available and compatible (allows widening)
  if (expectedElemType) {
    // Allow safe widening: i32 -> i64, f32 -> f64
    bool isCompatible = false;
    if (expectedElemType->isInt64() && elemType->isInt32())
      isCompatible = true;
    else if (expectedElemType->isFloat64() && elemType->isFloat32())
      isCompatible = true;
    else if (expectedElemType->equals(*elemType))
      isCompatible = true;

    if (isCompatible && !elemType->isArray()) {
      elemType = expectedElemType;
    }
  }

  // Build dimensions - if element is also an array, flatten into
  // multidimensional
  std::vector<size_t> dims = {arrLit.getElements().size()};
  TypePtr baseElemType = elemType;
  if (elemType->isArray()) {
    auto* innerArr =
        static_cast<sun::semantic_analysis::ArrayType*>(elemType.get());
    // Append inner dimensions
    for (size_t d : innerArr->getDimensions()) {
      dims.push_back(d);
    }
    baseElemType = innerArr->getElementType();
  }
  return Types::Array(baseElemType, dims);
}

TypePtr TypeInferer::inferType(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::NUMBER: {
      const auto& num = static_cast<const sun::ast::NumberExprAST&>(expr);
      // A suffixed literal (21u8, 1.5f32) names its own type
      if (num.hasSuffix()) {
        return Types::fromString(num.getSuffix());
      }
      if (num.isInteger()) {
        // Default to the narrowest of i32, i64 and u64 that holds the value.
        // The actual type may be refined by assignment context.
        const uint64_t magnitude = num.getMagnitude();
        const bool negative = num.isNegative();
        if (sun::semantic_analysis::literalFitsInType(
                magnitude, negative,
                sun::semantic_analysis::Type::Kind::Int32)) {
          return Types::Int32();
        }
        if (sun::semantic_analysis::literalFitsInType(
                magnitude, negative,
                sun::semantic_analysis::Type::Kind::Int64)) {
          return Types::Int64();
        }
        return Types::UInt64();
      }
      // Floating point literal
      return Types::Float64();
    }

    case ASTNodeType::CHAR_LITERAL: {
      // Unlike an integer literal, this never takes its type from context.
      const auto& lit = static_cast<const sun::ast::CharLiteralAST&>(expr);
      return lit.isByte() ? Types::UInt8() : Types::Char();
    }

    case ASTNodeType::STRING_LITERAL: {
      return Types::String();
    }

    case ASTNodeType::NULL_LITERAL: {
      return Types::NullPointer();
    }

    case ASTNodeType::BOOL_LITERAL: {
      return Types::Bool();
    }

    case ASTNodeType::STRUCT_LITERAL: {
      // Set by analyzeStructLiteral from the expected type; a literal has no
      // type of its own to infer.
      if (auto resolved = expr.getResolvedType()) return resolved;
      logAndThrowError(
          "Cannot infer the type of a '{ field: value }' literal here; "
          "annotate the target type.",
          expr.getLocation());
      return nullptr;
    }

    case ASTNodeType::ARRAY_LITERAL:
      return inferArrayLiteralType(
          static_cast<const sun::ast::ArrayLiteralAST&>(expr));
    case ASTNodeType::INDEX:
      return inferIndexType(static_cast<const sun::ast::IndexAST&>(expr));
    case ASTNodeType::VARIABLE_REFERENCE:
      if (expr.getModuleDeclaration()) return inferModuleReference(expr, ctx_);
      return inferVariableReferenceType(
          static_cast<const VariableReferenceAST&>(expr));
    case ASTNodeType::VARIABLE_CREATION: {
      const auto& varCreate =
          static_cast<const sun::ast::VariableCreationAST&>(expr);
      if (varCreate.hasTypeAnnotation()) {
        TypePtr type = typeAnnotationToType(*varCreate.getTypeAnnotation());
        return type;
      }
      // Infer from value expression
      return inferType(*varCreate.getValue());
    }

    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      const auto& varAssign =
          static_cast<const sun::ast::VariableAssignmentAST&>(expr);
      // Assignment returns the assigned value's type
      return inferType(*varAssign.getValue());
    }

    case ASTNodeType::REFERENCE_CREATION: {
      const auto& refCreate =
          static_cast<const sun::ast::ReferenceCreationAST&>(expr);
      // Reference type is ref(T) where T is the target's (referent's) type
      TypePtr targetType = unwrapRef(inferType(*refCreate.getTarget()));
      return Types::Reference(targetType, refCreate.isMutable());
    }

    case ASTNodeType::BINARY: {
      const auto& binExpr = static_cast<const sun::ast::BinaryExprAST&>(expr);
      // Comparison operators return bool
      switch (binExpr.getOp().kind) {
        case TokenKind::LESS:
        case TokenKind::GREATER:
        case TokenKind::LESS_EQUAL:
        case TokenKind::GREATER_EQUAL:
        case TokenKind::EQUAL_EQUAL:
        case TokenKind::NOT_EQUAL:
          return Types::Bool();
        default: {
          // Arithmetic operators - the operands' promoted type, matching the
          // widening codegen applies. Refs behave like values.
          // A literal analysis already retyped from context keeps that type.
          auto operandType = [this](const ExprAST& operand) {
            auto resolved = operand.getResolvedType();
            return resolved ? resolved : inferType(operand);
          };
          return promoteBinaryOperands(operandType(*binExpr.getLHS()),
                                       operandType(*binExpr.getRHS()));
        }
      }
    }

    case ASTNodeType::UNARY: {
      const auto& unaryExpr = static_cast<const sun::ast::UnaryExprAST&>(expr);
      // Logical not yields bool; - and ~ preserve the operand type
      if (unaryExpr.getOp().kind == TokenKind::NOT) {
        return Types::Bool();
      }
      return unwrapRef(inferType(*unaryExpr.getOperand()));
    }

    case ASTNodeType::CALL:
      return inferCallType(static_cast<const sun::ast::CallExprAST&>(expr));
    case ASTNodeType::IF: {
      const auto& ifExpr = static_cast<const sun::ast::IfExprAST&>(expr);
      return Types::Void();
    }

    case ASTNodeType::TERNARY: {
      const auto& ternary = static_cast<const sun::ast::TernaryExprAST&>(expr);
      auto thenType =
          sun::semantic_analysis::unwrapRef(inferType(*ternary.getThen()));
      auto elseType =
          sun::semantic_analysis::unwrapRef(inferType(*ternary.getElse()));
      return unifyTernaryTypes(thenType, elseType, expr.getLocation());
    }

    case ASTNodeType::MATCH: {
      const auto& matchExpr = static_cast<const sun::ast::MatchExprAST&>(expr);
      // Only arms that reach the merge contribute to the result type.
      // Bindings have already left their semantic scopes, so use the body's
      // resolved type instead of looking their names up again.
      std::unordered_set<int64_t> coveredTags;
      for (const auto& arm : matchExpr.getArms()) {
        if (!arm.isWildcard && arm.pattern && arm.pattern->getResolvedType() &&
            arm.pattern->getResolvedType()->isEnum() &&
            !coveredTags.insert(arm.resolvedVariantTag).second)
          continue;
        if (!sun::ast::exprDiverges(*arm.body)) {
          if (auto resolved = arm.body->getResolvedType()) {
            return unwrapRef(resolved);
          }
          return unwrapRef(inferType(*arm.body));
        }
        if (arm.isWildcard) break;
      }
      return Types::Void();
    }

    case ASTNodeType::FOR_LOOP: {
      // For loops always return 0.0 (f64)
      return Types::Float64();
    }

    case ASTNodeType::FOR_IN_LOOP: {
      // For-in loops always return 0.0 (f64)
      return Types::Float64();
    }

    case ASTNodeType::WHILE_LOOP: {
      // While loops always return 0.0 (f64)
      return Types::Float64();
    }

    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT: {
      // Break and continue don't return a meaningful value
      return Types::Void();
    }

    case ASTNodeType::BLOCK: {
      if (auto resolved = expr.getResolvedType()) return resolved;
      const auto& block = static_cast<const sun::ast::BlockExprAST&>(expr);
      if (block.isEmpty()) {
        return Types::Void();
      }
      // Look for return statements in the block to determine return type
      TypePtr returnType = nullptr;
      for (const auto& stmt : block.getBody()) {
        if (stmt->isReturn()) {
          const auto& retExpr =
              static_cast<const sun::ast::ReturnExprAST&>(*stmt);
          if (retExpr.hasValue()) {
            returnType = inferType(*retExpr.getValue());
          } else {
            returnType = Types::Void();
          }
          break;  // Use first return statement for type inference
        }
      }

      if (returnType) {
        return returnType;
      }

      // No return found. Only a match arm's body, an unsafe block's body,
      // and the compiler's own syntaxless Value blocks evaluate to their
      // last statement. Every other kind is a statement body: its trailing
      // expression is not a value, so a `try` body no longer types its
      // enclosing try-catch.
      if (!block.producesValue()) {
        return Types::Void();
      }
      const auto& lastExpr = *block.getBody().back();
      return inferType(lastExpr);
    }

    case ASTNodeType::FUNCTION: {
      // Function definitions return a function type (for first-class functions)
      const auto& func = static_cast<const sun::ast::FunctionAST&>(expr);
      const auto& proto = func.getProto();

      // Build parameter types
      std::vector<TypePtr> paramTypes;
      for (const auto& [argName, argType] : proto.getArgs()) {
        paramTypes.push_back(typeAnnotationToType(argType));
      }

      // Get return type
      TypePtr returnType;
      if (proto.hasReturnType()) {
        returnType = typeAnnotationToType(*proto.getReturnType());
      } else if (func.hasBody()) {
        // Infer from body (only for non-extern functions)
        returnType = inferType(func.getBody());
      } else {
        // Extern function without return type - default to void
        returnType = Types::Void();
      }

      // Named functions always get FunctionType (direct call)
      return Types::Function(returnType, std::move(paramTypes));
    }

    case ASTNodeType::LAMBDA: {
      // Lambda expressions return a lambda type (fat pointer closure)
      const auto& lambda = static_cast<const sun::ast::LambdaAST&>(expr);
      const auto& proto = lambda.getProto();

      // Build parameter types
      std::vector<TypePtr> paramTypes;
      for (const auto& [argName, argType] : proto.getArgs()) {
        paramTypes.push_back(typeAnnotationToType(argType));
      }

      // Get return type
      TypePtr returnType;
      if (proto.hasReturnType()) {
        returnType = typeAnnotationToType(*proto.getReturnType());
      } else if (lambda.hasBody()) {
        returnType = inferType(lambda.getBody());
      } else {
        returnType = Types::Void();
      }

      // Lambdas always get LambdaType (fat pointer closure)
      bool canThrow = proto.hasReturnType() && proto.getReturnType()->canError;
      auto lambdaType =
          Types::Lambda(returnType, std::move(paramTypes), canThrow);
      // Metadata for spawn/return escape checks (survives variable binding).
      // An owned capture counts too: the closure's environment holds the value
      // and the frame that built it drops it, so the closure is just as bound
      // to that frame as a borrow makes it.
      if (proto.hasRefCaptures() || !proto.getRefCaptureNames().empty() ||
          !proto.getOwnedCaptureNames().empty()) {
        static_cast<LambdaType*>(lambdaType.get())->setHasRefCaptures(true);
      }
      return lambdaType;
    }

    case ASTNodeType::INDEXED_ASSIGNMENT: {
      const auto& assignment =
          static_cast<const sun::ast::IndexedAssignmentAST&>(expr);
      // Type of indexed assignment is the type of the value being assigned
      return inferType(*assignment.getValue());
    }

    case ASTNodeType::RETURN: {
      const auto& returnExpr =
          static_cast<const sun::ast::ReturnExprAST&>(expr);
      if (returnExpr.hasValue()) {
        return inferType(*returnExpr.getValue());
      }
      return Types::Void();
    }

    case ASTNodeType::QUALIFIED_NAME: {
      if (expr.getModuleDeclaration()) return inferModuleReference(expr, ctx_);
      const auto& qualName =
          static_cast<const sun::ast::QualifiedNameAST&>(expr);
      std::string fullName = qualName.getFullName();

      // Look up in namespaced variables
      VariableInfo* varInfo = ctx_.lookupQualifiedVariable(fullName);
      if (varInfo) {
        qualName.setTargetDeclarationId(varInfo->declarationId);
        return varInfo->type;
      }

      // Look up in namespaced functions
      const FunctionInfo* funcInfo = ctx_.lookupQualifiedFunction(fullName);
      if (funcInfo) {
        qualName.setTargetDeclarationId(funcInfo->declarationId);
        return Types::Function(funcInfo->returnType, funcInfo->paramTypes,
                               funcInfo->canThrow);
      }

      // Unknown qualified name - error in strongly typed language
      logAndThrowError(
          "Unknown qualified name: '" + qualName.getFullName() + "'",
          qualName.getLocation());
    }

    case ASTNodeType::MODULE:
    case ASTNodeType::MOON_SCOPE:
    case ASTNodeType::USING:
    case ASTNodeType::DECLARE_TYPE:
      return Types::Void();

    case ASTNodeType::CLASS_DEFINITION: {
      // Class definitions themselves don't return a value
      return Types::Void();
    }

    case ASTNodeType::INTERFACE_DEFINITION: {
      // Interface definitions themselves don't return a value
      return Types::Void();
    }

    case ASTNodeType::THIS: {
      // 'this' returns the current class type
      if (ctx_.getCurrentClass()) {
        return ctx_.getCurrentClass();
      }
      // Error: 'this' used outside of class method
      return Types::Void();
    }

    case ASTNodeType::MEMBER_ACCESS:
      return inferType(static_cast<const MemberAccessAST&>(expr));

    case ASTNodeType::MEMBER_ASSIGNMENT: {
      const auto& memberAssign =
          static_cast<const sun::ast::MemberAssignmentAST&>(expr);
      // Analyze both sides for side effects and type checking
      inferType(*memberAssign.getObject());
      inferType(*memberAssign.getValue());
      // Assignment expression returns void
      return Types::Void();
    }

    case ASTNodeType::COMPOUND_ASSIGNMENT:
      // Compound assignment is a statement
      return Types::Void();

    case ASTNodeType::TRY_CATCH:
      // A try-catch is a statement: a `try` body is not one of the block
      // kinds that produce a value, so there is nothing to bind or return.
      // Code that wants a value out of a try returns from inside it.
      return Types::Void();

    case ASTNodeType::UNSAFE_BLOCK: {
      const auto& unsafeBlock =
          static_cast<const sun::ast::UnsafeBlockAST&>(expr);
      const auto& body = unsafeBlock.getBody();

      // Set unsafe context for type inference of the body
      ctx_.enterUnsafeBlock();

      TypePtr resultType;
      // For unsafe blocks, the type is the type of the last expression
      // This allows patterns like: return unsafe { _load<T>(ptr, idx); };
      if (body.getType() == ASTNodeType::BLOCK) {
        const auto& block = static_cast<const sun::ast::BlockExprAST&>(body);
        if (!block.isEmpty()) {
          // Return type of last statement (expression statement)
          const auto& lastStmt = *block.getBody().back();
          resultType = inferType(lastStmt);
        }
      }
      if (!resultType) {
        resultType = inferType(body);
      }

      ctx_.exitUnsafeBlock();
      return resultType;
    }

    case ASTNodeType::THROW: {
      // Throw expressions don't return a value (they transfer control)
      // We return Void but in practice this is a noreturn
      return Types::Void();
    }

    case ASTNodeType::GENERIC_CALL: {
      const auto& genericCall = static_cast<const GenericCallAST&>(expr);
      TypePtr type = inferGenericCallType(genericCall);
      return type;
    }

    case ASTNodeType::PACK_EXPANSION: {
      // Pack expansion (args...) - represents multiple values at compile time
      // Type checking is deferred to codegen where we know the actual types
      // For now, just return void as a placeholder
      return Types::Void();
    }

    default:
      logAndThrowError("Cannot infer type for expression of kind " +
                           std::to_string(static_cast<int>(expr.getType())),
                       expr.getLocation());
  }
}

// -------------------------------------------------------------------
// Member access type inference (extracted for clarity)
// -------------------------------------------------------------------

TypePtr TypeInferer::inferModuleMemberType(const MemberAccessAST& memberAccess,
                                           const TypePtr& objectType,
                                           const std::string& memberName) {
  // Module member access: mod_x.mod_y or mod_x.varName
  auto* moduleType =
      static_cast<sun::semantic_analysis::ModuleType*>(objectType.get());
  std::string modPath = moduleType->getModulePath();

  std::string nestedModPath = modPath + "." + memberName;
  if (auto* nested = ctx_.lookupModuleScope(nestedModPath)) {
    ctx_.requireModuleAccessible(*nested, memberAccess.getLocation());
    return Types::Module(QualifiedName::joinPath(nested->scopePath));
  }

  // Use unified symbol lookup to find the member in this module
  SymbolMatch match = ctx_.findSymbolInModule(modPath, memberName);
  if (match) {
    // Name the access after the declaration it resolved to — e.g.
    // "$d9b854ae$_std_make_heap_allocator" for std.make_heap_allocator.
    // Each declaration was given its name when it was declared, and
    // codegen emits it under that name; rebuilding one from the module
    // path here would drop a function's overload suffix and name a symbol
    // that was never emitted.
    QualifiedName resolvedName;
    switch (match.kind) {
      case SymbolKind::Function:
        if (match.functionInfo)
          resolvedName = match.functionInfo->qualifiedName;
        break;
      case SymbolKind::Variable:
        if (match.variableInfo)
          resolvedName = match.variableInfo->qualifiedName;
        break;
      case SymbolKind::Class:
        if (match.classType) resolvedName = match.classType->getQualifiedName();
        break;
      case SymbolKind::GenericClass:
        if (match.genericClassInfo)
          resolvedName = match.genericClassInfo->qualifiedName;
        break;
      case SymbolKind::Interface:
        if (match.interfaceType)
          resolvedName = match.interfaceType->getQualifiedName();
        break;
      case SymbolKind::Enum:
        if (match.enumType) resolvedName = match.enumType->getQualifiedName();
        break;
      default:
        break;
    }
    // A kind that carries no name of its own (a nested module, say) still
    // needs one a lookup can use.
    if (resolvedName.empty()) {
      resolvedName = QualifiedName(
          sun::semantic_analysis::splitModulePath(match.modulePath),
          memberName);
    }
    memberAccess.setQualifiedName(resolvedName);

    switch (match.kind) {
      case SymbolKind::Class:
        return match.classType;
      case SymbolKind::GenericClass: {
        // If type arguments are provided, instantiate the generic class
        if (memberAccess.hasTypeArguments() && match.genericClassInfo) {
          auto typeArgs = resolveTypeArguments(memberAccess.getTypeArguments(),
                                               memberAccess.getLocation(),
                                               "generic class instantiation");
          // Store resolved type args on the AST for codegen
          memberAccess.setResolvedTypeArgs(typeArgs);
          // Instantiate the generic class with module-qualified name
          // (modPath.memberName so lookupGenericClass can find it)
          std::string qualifiedName = modPath + "." + memberName;
          auto specializedClass =
              generics_.instantiateGenericClass(qualifiedName, typeArgs);
          if (specializedClass) {
            return specializedClass;
          }
          logAndThrowError(
              "Failed to instantiate generic class '" + memberName + "'",
              memberAccess.getLocation());
        }
        // No type arguments - return void as placeholder
        return Types::Void();
      }
      case SymbolKind::Interface:
        return match.interfaceType;
      case SymbolKind::GenericInterface: {
        // If type arguments are provided, instantiate the generic interface
        if (memberAccess.hasTypeArguments() && match.genericInterfaceInfo) {
          auto typeArgs = resolveTypeArguments(
              memberAccess.getTypeArguments(), memberAccess.getLocation(),
              "generic interface instantiation");
          // Store resolved type args on the AST for codegen
          memberAccess.setResolvedTypeArgs(typeArgs);
          // Instantiate the generic interface with module-qualified name
          std::string qualifiedName = modPath + "." + memberName;
          auto specializedInterface =
              generics_.instantiateGenericInterface(qualifiedName, typeArgs);
          if (specializedInterface) {
            return specializedInterface;
          }
          logAndThrowError(
              "Failed to instantiate generic interface '" + memberName + "'",
              memberAccess.getLocation());
        }
        // No type arguments - return void as placeholder
        return Types::Void();
      }
      case SymbolKind::Enum:
        return match.enumType;
      case SymbolKind::Function:
        memberAccess.setTargetDeclarationId(match.functionInfo->declarationId);
        return Types::Function(match.functionInfo->returnType,
                               match.functionInfo->paramTypes,
                               match.functionInfo->canThrow);
      case SymbolKind::GenericFunction: {
        // m.f<i32>(...): instantiate here, and point the call site at the
        // specialization rather than at the template's name. A call site
        // that inferred its type arguments from the arguments (see
        // SemanticAnalyzer::resolveModuleQualifiedGenericCall) has already
        // recorded them; only a bare reference to the template has none.
        if (!memberAccess.hasTypeArguments() &&
            !memberAccess.hasResolvedTypeArgs()) {
          logAndThrowError(
              "Generic function '" + memberName + "' in module '" +
                  sun::semantic_analysis::displayModulePath(modPath) +
                  "' needs type arguments here; they are only inferred at a "
                  "call, e.g. " +
                  memberName + "<i32>(...)",
              memberAccess.getLocation());
        }
        if (!match.genericFunctionInfo) {
          logAndThrowError(
              "Generic function '" + memberName + "' in module '" +
                  sun::semantic_analysis::displayModulePath(modPath) +
                  "' has no definition",
              memberAccess.getLocation());
        }
        std::vector<TypePtr> typeArgs =
            memberAccess.hasResolvedTypeArgs()
                ? memberAccess.getResolvedTypeArgs()
                : resolveTypeArguments(memberAccess.getTypeArguments(),
                                       memberAccess.getLocation(),
                                       "generic function instantiation");
        memberAccess.setResolvedTypeArgs(typeArgs);
        SpecializedFunctionInfo specialized =
            generics_.requireGenericSpecialization(*match.genericFunctionInfo,
                                                   typeArgs, memberName,
                                                   memberAccess.getLocation());
        memberAccess.setQualifiedName(specialized.qualifiedName);
        memberAccess.setTargetDeclarationId(
            specialized.asFunctionInfo().declarationId);
        return specialized.functionType();
      }
      case SymbolKind::Variable:
        memberAccess.setTargetDeclarationId(match.variableInfo->declarationId);
        return match.variableInfo->type;
      default:
        break;
    }
  }

  logAndThrowError("Unknown member '" + memberName + "' in module '" +
                       sun::semantic_analysis::displayModulePath(modPath) + "'",
                   memberAccess.getLocation());
}

TypePtr TypeInferer::inferClassMemberType(const MemberAccessAST& memberAccess,
                                          const TypePtr& objectType,
                                          const std::string& memberName) {
  const auto* classType = static_cast<const ClassType*>(objectType.get());

  // Check for field
  const sun::semantic_analysis::ClassField* field =
      ctx_.accessibleField(*classType, memberName, memberAccess.getLocation());
  if (field) {
    memberAccess.setTargetDeclarationId(field->declarationId);
    return field->type;
  }

  // Check for method
  const ClassMethod* method =
      ctx_.accessibleMethod(*classType, memberName, memberAccess.getLocation());
  if (method) {
    if (!method->isGeneric())
      memberAccess.setTargetDeclarationId(method->declarationId);
    TypePtr returnType = method->returnType;

    // Generic method calls: the type arguments written at the call,
    // completed by the ones the call site inferred from its arguments
    if (method->isGeneric()) {
      const auto& typeParams = method->typeParameters;

      std::vector<TypePtr> typeArgPtrs;
      for (const auto& typeArg : memberAccess.getTypeArguments()) {
        auto argType = typeAnnotationToType(*typeArg);
        if (argType) {
          typeArgPtrs.push_back(argType);
        }
      }
      if (memberAccess.hasResolvedTypeArgs() &&
          memberAccess.getResolvedTypeArgs().size() > typeArgPtrs.size()) {
        typeArgPtrs = memberAccess.getResolvedTypeArgs();
      }

      // Store resolved type args on the AST for codegen
      memberAccess.setResolvedTypeArgs(typeArgPtrs);

      if (!typeArgPtrs.empty() && typeArgPtrs.size() == typeParams.size()) {
        // Instantiate the generic method - this creates and stores the
        // specialized FunctionAST on the generic method for codegen access
        auto mutableClassType = std::static_pointer_cast<ClassType>(objectType);
        // Retain the selected specialization independently of its symbol.
        if (auto specialized = generics_.instantiateGenericMethod(
                mutableClassType, memberName, typeArgPtrs)) {
          memberAccess.setTargetDeclarationId(specialized->getDeclarationId());
        }

        ctx_.enterTypeParamScope(typeParams, typeArgPtrs);
        returnType = substituteTypeParameters(returnType);
        std::vector<TypePtr> substitutedParams;
        for (const auto& pt : method->paramTypes) {
          substitutedParams.push_back(substituteTypeParameters(pt));
        }
        ctx_.exitScope();
        return Types::Function(returnType, substitutedParams, method->canThrow,
                               method->isUnsafe);
      }
    }

    return Types::Function(method->returnType, method->paramTypes,
                           method->canThrow, method->isUnsafe);
  }

  logAndThrowError("Unknown member '" + memberName + "' on class '" +
                       classType->getDisplayName() + "'",
                   memberAccess.getLocation());
}

TypePtr TypeInferer::inferInterfaceMemberType(
    const MemberAccessAST& memberAccess, const TypePtr& objectType,
    const std::string& memberName) {
  const auto* ifaceType =
      static_cast<const sun::semantic_analysis::InterfaceType*>(
          objectType.get());

  // Check for field
  const InterfaceField* field =
      ctx_.accessibleField(*ifaceType, memberName, memberAccess.getLocation());
  if (field) {
    memberAccess.setTargetDeclarationId(field->declarationId);
    return field->type;
  }

  // Check for method
  const InterfaceMethod* method =
      ctx_.accessibleMethod(*ifaceType, memberName, memberAccess.getLocation());
  if (method) {
    memberAccess.setTargetDeclarationId(method->declarationId);
    return Types::Function(method->returnType, method->paramTypes, false,
                           method->isUnsafe);
  }

  logAndThrowError("Unknown member '" + memberName + "' on interface '" +
                       ifaceType->toDisplayString() + "'",
                   memberAccess.getLocation());
}

TypePtr TypeInferer::inferTypeParameterMemberType(
    const MemberAccessAST& memberAccess, const TypePtr& objectType,
    const std::string& memberName) {
  // For type parameters, check if there's a narrowed type from _is<T>
  // This allows member access validation during semantic analysis
  if (memberAccess.getObject()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*memberAccess.getObject());
    // Substitute type parameters first for proper narrowing validation
    TypePtr substitutedType = substituteTypeParameters(objectType);
    TypePtr narrowedType =
        ctx_.getNarrowedType(varRef.getName(), substitutedType);
    if (narrowedType) {
      // Recursively dispatch with the narrowed type
      if (narrowedType->isClass()) {
        const auto* classType =
            static_cast<const ClassType*>(narrowedType.get());
        const sun::semantic_analysis::ClassField* field = ctx_.accessibleField(
            *classType, memberName, memberAccess.getLocation());
        if (field) {
          memberAccess.setTargetDeclarationId(field->declarationId);
          return field->type;
        }
        const ClassMethod* method = ctx_.accessibleMethod(
            *classType, memberName, memberAccess.getLocation());
        if (method)
          return Types::Function(method->returnType, method->paramTypes, false,
                                 method->isUnsafe);
        logAndThrowError("Unknown member '" + memberName + "' on class '" +
                             classType->getDisplayName() + "'",
                         memberAccess.getLocation());
      }
      if (narrowedType->isInterface()) {
        const auto* ifaceType =
            static_cast<const sun::semantic_analysis::InterfaceType*>(
                narrowedType.get());
        const InterfaceField* field = ctx_.accessibleField(
            *ifaceType, memberName, memberAccess.getLocation());
        if (field) {
          memberAccess.setTargetDeclarationId(field->declarationId);
          return field->type;
        }
        const InterfaceMethod* method = ctx_.accessibleMethod(
            *ifaceType, memberName, memberAccess.getLocation());
        if (method)
          return Types::Function(method->returnType, method->paramTypes, false,
                                 method->isUnsafe);
        logAndThrowError("Unknown member '" + memberName + "' on interface '" +
                             ifaceType->toDisplayString() + "'",
                         memberAccess.getLocation());
      }
    }
  }
  // No narrowing. A `<T: ISomething>` constraint is the other way a type
  // parameter can have known members: whatever T turns out to be, it
  // implements that interface, so the interface's members are reachable
  // here and every specialization will have them.
  const auto* param =
      static_cast<const sun::semantic_analysis::TypeParameterType*>(
          objectType.get());
  if (param->hasConstraint()) {
    const auto& constraint = param->getConstraint();
    auto ifaceType = sun::semantic_analysis::isTypeTrait(constraint.name)
                         ? nullptr
                         : resolveConstraintInterface(constraint);
    if (ifaceType) {
      const InterfaceField* field = ctx_.accessibleField(
          *ifaceType, memberName, memberAccess.getLocation());
      if (field) {
        memberAccess.setTargetDeclarationId(field->declarationId);
        return field->type;
      }
      const InterfaceMethod* method = ctx_.accessibleMethod(
          *ifaceType, memberName, memberAccess.getLocation());
      if (method)
        return Types::Function(method->returnType, method->paramTypes, false,
                               method->isUnsafe);
      logAndThrowError("Unknown member '" + memberName +
                           "' on type parameter '" + param->getName() +
                           "', which is constrained to interface '" +
                           constraint.toString() + "'",
                       memberAccess.getLocation());
    }
    // A trait such as `_Numeric`, or `lambda`: it says which types are
    // allowed, not which members they carry.
    logAndThrowError("Cannot access member '" + memberName +
                         "' on type parameter '" + param->getName() +
                         "': its constraint '" + constraint.toString() +
                         "' is a type trait, which promises no members. "
                         "Constrain it to an interface to call methods on "
                         "it.",
                     memberAccess.getLocation());
  }

  logAndThrowError("Cannot access member '" + memberName +
                       "' on unconstrained type parameter '" +
                       objectType->toDisplayString() + "'",
                   memberAccess.getLocation());
}

TypePtr TypeInferer::inferType(const MemberAccessAST& memberAccess) {
  if (memberAccess.getModuleDeclaration())
    return inferModuleReference(memberAccess, ctx_);
  const std::string& memberName = memberAccess.getMemberName();

  // Get the fully-resolved object type:
  // 1. Use resolved type if available, otherwise infer
  // 2. Unwrap references
  // 3. Check for type narrowing from _is<T> guards
  // 4. Unwrap raw_ptr<Class> to Class for member access
  TypePtr objectType = memberAccess.getObject()->getResolvedType();
  if (!objectType &&
      memberAccess.getObject()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    // Generic enum object (Option.None): only valid once analysis resolved
    // the specialization from context
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*memberAccess.getObject());
    if (!ctx_.currentScope().lookupVariable(varRef.getName()) &&
        ctx_.scope()->lookupGenericEnum(varRef.getName())) {
      if (auto resolved = memberAccess.getResolvedType()) {
        return resolved;
      }
      logAndThrowError("Cannot infer type arguments for '" + varRef.getName() +
                           "." + memberName +
                           "'; add a type annotation to the target",
                       memberAccess.getLocation());
    }
  }
  if (!objectType) {
    objectType = inferType(*memberAccess.getObject());
  }
  objectType = unwrapRef(objectType);
  // Cached generic fields may carry a parameter from another signature.
  // Use this scope's binding without changing the shared class field.
  if (objectType && objectType->isTypeParameter()) {
    objectType = unwrapRef(substituteTypeParameters(objectType));
  }

  if (!objectType) {
    logAndThrowError(
        "Cannot access member '" + memberName + "' on unknown type",
        memberAccess.getLocation());
  }

  // Unwrap raw_ptr<Class> to Class for member access (requires unsafe)
  if (objectType->isRawPointer()) {
    TypePtr pointeeType =
        static_cast<sun::semantic_analysis::RawPointerType*>(objectType.get())
            ->getPointeeType();
    if (pointeeType && pointeeType->isClass()) {
      if (!ctx_.isInUnsafeBlock()) {
        logAndThrowError(
            "Dereferencing 'raw_ptr' can only be done in an unsafe block",
            memberAccess.getLocation());
      }
      objectType = pointeeType;
    }
  }

  // Unwrap static_ptr<Class> to Class for member access
  if (objectType->isStaticPointer()) {
    TypePtr pointeeType =
        static_cast<StaticPointerType*>(objectType.get())->getPointeeType();
    if (pointeeType && pointeeType->isClass()) {
      objectType = pointeeType;
    }
  }

  // Now dispatch based on the resolved object type
  switch (objectType->getKind()) {
    case sun::semantic_analysis::Type::Kind::Module:
      return inferModuleMemberType(memberAccess, objectType, memberName);
    case sun::semantic_analysis::Type::Kind::Enum: {
      auto* enumType = static_cast<EnumType*>(objectType.get());
      const auto* variant = enumType->getVariant(memberName);
      if (variant) {
        if (variant->hasPayload()) {
          logAndThrowError("Variant '" + memberName + "' of enum '" +
                               enumType->getDisplayName() +
                               "' carries a payload; construct it with '" +
                               enumType->getBaseName() + "." + memberName +
                               "(...)'",
                           memberAccess.getLocation());
        }
        return objectType;  // Enum variant has the enum type
      }
      logAndThrowError("Unknown variant '" + memberName + "' in enum '" +
                           enumType->getDisplayName() + "'",
                       memberAccess.getLocation());
    }

    case sun::semantic_analysis::Type::Kind::Array: {
      // The accessors are methods; the call form is typed by
      // inferArrayMethodType before the callee is inferred, so reaching
      // here means the property form was written.
      if (isArrayMethod(memberName)) {
        logAndThrowError("Array has no property '" + memberName +
                             "'; call it: '" + memberName + "(...)'",
                         memberAccess.getLocation());
      }
      logAndThrowError("Array has no member '" + memberName +
                           "'; available: ndims(), dim(i)",
                       memberAccess.getLocation());
    }

    case sun::semantic_analysis::Type::Kind::StaticPointer: {
      // The accessors are methods; the call form is typed by
      // inferStaticPtrMethodType before the callee is inferred, so reaching
      // here means the property form was written.
      if (isStaticPtrMethod(memberName)) {
        logAndThrowError("static_ptr has no property '" + memberName +
                             "'; call '" + memberName + "()'",
                         memberAccess.getLocation());
      }
      logAndThrowError("static_ptr has no member '" + memberName +
                           "'; available: length(), raw()",
                       memberAccess.getLocation());
    }

    case sun::semantic_analysis::Type::Kind::RawPointer: {
      // raw_ptr<T> where T is not a class (class case handled above): a
      // bare pointer has no members; read through it with _load<T> or
      // _to_ref<T>
      logAndThrowError("raw_ptr has no member '" + memberName +
                           "'; read through it with _load<T>(p, i) or "
                           "_to_ref<T>(p)",
                       memberAccess.getLocation());
    }

    case sun::semantic_analysis::Type::Kind::Class:
      return inferClassMemberType(memberAccess, objectType, memberName);
    case sun::semantic_analysis::Type::Kind::Interface:
      return inferInterfaceMemberType(memberAccess, objectType, memberName);
    case sun::semantic_analysis::Type::Kind::TypeParameter:
      return inferTypeParameterMemberType(memberAccess, objectType, memberName);
    default:
      logAndThrowError("Cannot access member '" + memberName + "' on type '" +
                           objectType->toDisplayString() + "'",
                       memberAccess.getLocation());
  }
}

TypePtr TypeInferer::inferGenericCallType(const GenericCallAST& genericCall) {
  if (genericCall.hasResolvedType()) {
    return genericCall.getResolvedType();
  }
  const std::string& funcName = genericCall.getFunctionName();
  QualifiedName resolved = ctx_.resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  // Dispatch based on call type: intrinsic, generic function, or generic class
  bool isIntrinsicCall = sun::codegen::intrinsics::isIntrinsic(funcName);
  auto* genericClassInfo = ctx_.lookupGenericClass(lookupName);
  auto* genFuncInfo = ctx_.lookupGenericFunction(lookupName);

  if (isIntrinsicCall) {
    return inferIntrinsicCallType(genericCall);
  } else if (genFuncInfo) {
    return inferGenericFunctionCallType(genericCall);
  } else if (genericClassInfo) {
    return inferGenericClassConstructionType(genericCall);
  }

  logAndThrowError("Unknown generic function or class: '" + funcName + "'",
                   genericCall.getLocation());
}

// -------------------------------------------------------------------
// Intrinsic call type inference
// -------------------------------------------------------------------

TypePtr TypeInferer::inferIntrinsicCallType(const GenericCallAST& genericCall) {
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  const std::string& funcName = genericCall.getFunctionName();

  if (funcName == "_sizeof") {
    return Types::Int64();
  }
  if (funcName == "_load") {
    return typeArgs.empty() ? nullptr : typeArgs[0];
  }
  if (funcName == "_store" || funcName == "_init") {
    return Types::Void();
  }
  if (funcName == "_ptr_as_raw" || funcName == "_address_of") {
    return typeArgs.empty() ? nullptr : Types::RawPointer(typeArgs[0]);
  }
  if (funcName == "_to_ref") {
    return typeArgs.empty() ? nullptr : Types::Reference(typeArgs[0]);
  }
  if (funcName == "_is") {
    return Types::Bool();
  }
  if (funcName == "_deinit") {
    return Types::Void();
  }
  if (funcName == "_enum_from_int") {
    if (typeArgs.size() != 1 ||
        (!typeArgs[0]->isTypeParameter() &&
         (!typeArgs[0]->isEnum() ||
          static_cast<EnumType*>(typeArgs[0].get())->hasPayload()))) {
      logAndThrowError("_enum_from_int<T>: T must be an enum without payloads",
                       genericCall.getLocation());
    }
    const auto& args = genericCall.getArgs();
    if (args.size() != 1 || !args[0]->getResolvedType() ||
        (!sun::semantic_analysis::unwrapRef(args[0]->getResolvedType())
              ->isIntegral() &&
         !sun::semantic_analysis::unwrapRef(args[0]->getResolvedType())
              ->isTypeParameter())) {
      logAndThrowError(
          "_enum_from_int<T> requires exactly one integer argument",
          genericCall.getLocation());
    }
    auto result = generics_.instantiateGenericEnum("std.Option", {typeArgs[0]});
    if (!result) {
      logAndThrowError("_enum_from_int<T> requires the standard library",
                       genericCall.getLocation());
    }
    return result;
  }
  if (funcName == "_convert" || funcName == "_bitcast") {
    return typeArgs.empty() ? nullptr : typeArgs[0];
  }
  // _spawn hands back the context it allocated. The type is the stdlib's own
  // ThreadContext, which only exists when std.thread has been loaded — and
  // _spawn is only ever written inside std.thread, so it always has.
  if (funcName == "_spawn") {
    auto context = ctx_.scope()->lookupClass("std.thread.ThreadContext");
    if (!context) {
      logAndThrowError(
          "_spawn requires the standard library's std.thread module",
          genericCall.getLocation());
    }
    return Types::RawPointer(context);
  }
  if (funcName == "_thread_join") {
    return typeArgs.empty() ? nullptr : typeArgs[0];
  }
  if (funcName == "_thread_join_drop") {
    return Types::Void();
  }

  // Unknown intrinsic - return void as fallback
  return Types::Void();
}

// -------------------------------------------------------------------
// static_ptr<T> builtin methods
// -------------------------------------------------------------------

StaticPointerType* TypeInferer::asNonClassStaticPtr(const TypePtr& type) {
  if (!type || !type->isStaticPointer()) return nullptr;
  auto* staticPtr = static_cast<StaticPointerType*>(type.get());
  const auto& pointee = staticPtr->getPointeeType();
  if (pointee && pointee->isClass()) return nullptr;
  return staticPtr;
}

bool TypeInferer::isStaticPtrMethod(const std::string& name) {
  return name == "length" || name == "raw";
}

bool TypeInferer::isArrayMethod(const std::string& name) {
  return name == "ndims" || name == "dim";
}

TypePtr TypeInferer::inferArrayMethodType(const std::string& name,
                                          const std::vector<TypePtr>& argTypes,
                                          const sun::support::Position& loc) {
  if (!isArrayMethod(name)) {
    logAndThrowError(
        "Array has no method '" + name + "'; available: ndims(), dim(i)", loc);
  }
  if (name == "ndims") {
    if (!argTypes.empty()) {
      logAndThrowError("array.ndims() takes no arguments", loc);
    }
    return Types::Int64();
  }
  if (argTypes.size() != 1 || !argTypes[0] ||
      !unwrapRef(argTypes[0])->isIntegral()) {
    logAndThrowError("array.dim(i) takes one integer argument", loc);
  }
  return Types::Int64();
}

TypePtr TypeInferer::inferStaticPtrMethodType(
    const StaticPointerType& ptrType, const std::string& name, size_t argCount,
    const sun::support::Position& loc) {
  if (!isStaticPtrMethod(name)) {
    logAndThrowError(
        "static_ptr has no method '" + name + "'; available: length(), raw()",
        loc);
  }
  if (argCount != 0) {
    logAndThrowError("static_ptr." + name + "() takes no arguments", loc);
  }
  if (name == "length") return Types::Int64();
  return Types::RawPointer(ptrType.getPointeeType());
}

// -------------------------------------------------------------------
// Generic function call type inference
// -------------------------------------------------------------------

TypePtr TypeInferer::inferGenericFunctionCallType(
    const GenericCallAST& genericCall) {
  if (auto calleeType = genericCall.getResolvedCalleeType()) {
    return static_cast<const sun::semantic_analysis::FunctionType&>(*calleeType)
        .getReturnType();
  }
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  const std::string& funcName = genericCall.getFunctionName();
  QualifiedName resolved = ctx_.resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  auto* genFuncInfo = ctx_.lookupGenericFunction(lookupName);
  if (!genFuncInfo) {
    logAndThrowError("Unknown generic function: '" + funcName + "'",
                     genericCall.getLocation());
  }

  // No specialization - type args contain type parameters
  // Compute return type from generic function's declared return type
  if (genFuncInfo->returnType.has_value()) {
    SemanticContext::ScopeSwitchGuard definitionScope(
        ctx_, SemanticContext::definitionScopeOf(*genFuncInfo));
    SemanticContext::SourceFileGuard definitionFile(
        ctx_, genFuncInfo->AST->getSourceFileId());
    auto typeParams = sun::ast::typeParameterNames(genFuncInfo->typeParameters);
    ctx_.enterTypeParamScope(typeParams, typeArgs);
    TypePtr returnType = typeAnnotationToType(*genFuncInfo->returnType);
    ctx_.exitScope();
    return returnType;
  }

  // No declared return type - can't infer without instantiation
  logAndThrowError("Generic function '" + funcName +
                       "' called with unresolved type parameters requires a "
                       "declared return type",
                   genericCall.getLocation());
}

// -------------------------------------------------------------------
// Generic class construction type inference
// -------------------------------------------------------------------

TypePtr TypeInferer::inferGenericClassConstructionType(
    const GenericCallAST& genericCall) {
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  const std::string& funcName = genericCall.getFunctionName();
  QualifiedName resolved = ctx_.resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  auto* genericClassInfo = ctx_.lookupGenericClass(lookupName);
  if (!genericClassInfo) {
    logAndThrowError("Unknown generic class: '" + funcName + "'",
                     genericCall.getLocation());
  }

  // Class not yet instantiated - instantiate it now
  auto specializedClass =
      generics_.instantiateGenericClass(lookupName, typeArgs);
  return specializedClass;
}

}  // namespace sun::semantic_analysis
