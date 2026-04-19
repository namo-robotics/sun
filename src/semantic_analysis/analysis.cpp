// analysis.cpp — Main analysis entry points for semantic analyzer

#include <set>

#include "config.h"
#include "error.h"
#include "intrinsics.h"
#include "parser.h"
#include "semantic_analyzer.h"

// Helper: check if an integer literal value fits in a target primitive type
static bool literalFitsInType(int64_t value,
                              const sun::PrimitiveType* primType) {
  switch (primType->getKind()) {
    case sun::Type::Kind::Int8:
      return value >= INT8_MIN && value <= INT8_MAX;
    case sun::Type::Kind::Int16:
      return value >= INT16_MIN && value <= INT16_MAX;
    case sun::Type::Kind::Int32:
      return value >= INT32_MIN && value <= INT32_MAX;
    case sun::Type::Kind::Int64:
      return true;  // int64_t always fits in i64
    case sun::Type::Kind::UInt8:
      return value >= 0 && value <= UINT8_MAX;
    case sun::Type::Kind::UInt16:
      return value >= 0 && value <= UINT16_MAX;
    case sun::Type::Kind::UInt32:
      return value >= 0 && static_cast<uint64_t>(value) <= UINT32_MAX;
    case sun::Type::Kind::UInt64:
      return value >= 0;  // int64_t can't represent full u64 range
    default:
      return false;
  }
}

// Helper: try to coerce an integer literal to a target primitive type.
// Returns true if coercion happened (literal fits in target type).
// If throwOnFail is true, throws an error when the literal doesn't fit.
static bool tryCoerceIntegerLiteral(ExprAST* expr, sun::TypePtr targetType,
                                    bool throwOnFail = false) {
  if (!expr || !targetType || !targetType->isPrimitive()) return false;
  if (expr->getType() != ASTNodeType::NUMBER) return false;

  const auto& numLit = static_cast<const NumberExprAST&>(*expr);
  if (!numLit.isInteger()) return false;

  int64_t val = numLit.getIntVal();
  const auto* primType =
      static_cast<const sun::PrimitiveType*>(targetType.get());

  if (literalFitsInType(val, primType)) {
    expr->setResolvedType(targetType);
    return true;
  }

  if (throwOnFail) {
    logAndThrowError("Integer literal " + std::to_string(val) +
                     " cannot be represented as '" + targetType->toString() +
                     "'");
  }
  return false;
}

// Helper: extract type guard pattern from condition
// If condition is `_is<T>(var)`, returns (varName, narrowedType)
// Works for concrete types, interfaces, and type traits
static std::optional<std::pair<std::string, sun::TypePtr>> extractTypeGuard(
    const ExprAST& cond, SemanticAnalyzer& analyzer) {
  // Must be a GenericCallAST with function name "_is"
  if (cond.getType() != ASTNodeType::GENERIC_CALL) return std::nullopt;

  const auto& genericCall = static_cast<const GenericCallAST&>(cond);
  if (sun::getIntrinsic(genericCall.getFunctionName()) != sun::Intrinsic::Is) {
    return std::nullopt;
  }

  // Must have exactly one argument that is a variable reference
  const auto& args = genericCall.getArgs();
  if (args.size() != 1) return std::nullopt;
  if (args[0]->getType() != ASTNodeType::VARIABLE_REFERENCE)
    return std::nullopt;

  const auto& varRef = static_cast<const VariableReferenceAST&>(*args[0]);
  const std::string& varName = varRef.getName();

  // Get the type argument
  const auto& typeArgs = genericCall.getTypeArguments();
  const std::string& typeName = typeArgs[0]->baseName;

  // Skip type traits (_Integer, _Float, etc.) - they don't narrow to a concrete
  // type
  if (sun::isTypeTrait(typeName)) {
    return std::nullopt;
  }

  // Check if it's an interface
  auto interfaceType = analyzer.lookupInterface(typeName);
  if (interfaceType) {
    return std::make_pair(varName, interfaceType);
  }

  // Check if it's a class
  auto classType = analyzer.lookupClass(typeName);
  if (classType) {
    return std::make_pair(varName, classType);
  }

  // Check if it's a primitive type
  sun::TypePtr primType = sun::Types::fromString(typeName);
  if (primType) {
    return std::make_pair(varName, primType);
  }

  return std::nullopt;
}

// -------------------------------------------------------------------
// Type assignability checking
// -------------------------------------------------------------------

// Check if a type can be assigned to another type.
// This implements the subtyping rules for Sun:
// - Exact type equality
// - Class C can be assigned to interface I if C implements I
// - ref T can be assigned to ref I if T is assignable to I
// - Numeric widening: smaller integers to larger (including signed/unsigned),
// f32 to f64
bool SemanticAnalyzer::isAssignableTo(const sun::TypePtr& from,
                                      const sun::TypePtr& to) {
  if (!from || !to) return false;

  // Exact equality always works
  if (from->equals(*to)) return true;

  // Numeric widening
  if (from->isPrimitive() && to->isPrimitive()) {
    auto fromKind = from->getKind();
    auto toKind = to->getKind();

    auto isInteger = [](sun::Type::Kind k) {
      return k == sun::Type::Kind::Int8 || k == sun::Type::Kind::Int16 ||
             k == sun::Type::Kind::Int32 || k == sun::Type::Kind::Int64 ||
             k == sun::Type::Kind::UInt8 || k == sun::Type::Kind::UInt16 ||
             k == sun::Type::Kind::UInt32 || k == sun::Type::Kind::UInt64;
    };

    auto intBitWidth = [](sun::Type::Kind k) -> int {
      switch (k) {
        case sun::Type::Kind::Int8:
        case sun::Type::Kind::UInt8:
          return 8;
        case sun::Type::Kind::Int16:
        case sun::Type::Kind::UInt16:
          return 16;
        case sun::Type::Kind::Int32:
        case sun::Type::Kind::UInt32:
          return 32;
        case sun::Type::Kind::Int64:
        case sun::Type::Kind::UInt64:
          return 64;
        default:
          return 0;
      }
    };

    // Allow integer widening (destination must be at least as wide)
    // This includes u8 -> i64, i32 -> i64, etc.
    if (isInteger(fromKind) && isInteger(toKind)) {
      return intBitWidth(fromKind) <= intBitWidth(toKind);
    }

    // Allow f32 <-> f64 conversions (both widening and narrowing)
    // This matches the existing permissive behavior for floating point
    if ((fromKind == sun::Type::Kind::Float32 ||
         fromKind == sun::Type::Kind::Float64) &&
        (toKind == sun::Type::Kind::Float32 ||
         toKind == sun::Type::Kind::Float64)) {
      return true;
    }
  }

  // Unwrap reference types and check inner compatibility
  if (to->isReference() && from->isReference()) {
    auto* toRef = static_cast<sun::ReferenceType*>(to.get());
    auto* fromRef = static_cast<sun::ReferenceType*>(from.get());
    return isAssignableTo(fromRef->getReferencedType(),
                          toRef->getReferencedType());
  }

  // Class-to-interface assignability:
  // Class C can be assigned to interface I if C implements I
  if (to->isInterface() && from->isClass()) {
    auto* ifaceType = static_cast<sun::InterfaceType*>(to.get());
    auto* classType = static_cast<sun::ClassType*>(from.get());
    return classType->implementsInterface(ifaceType->getName());
  }

  // ref Class -> Interface (unwrap ref, check class implements interface)
  if (to->isInterface() && from->isReference()) {
    auto* fromRef = static_cast<sun::ReferenceType*>(from.get());
    sun::TypePtr innerFrom = fromRef->getReferencedType();
    if (innerFrom && innerFrom->isClass()) {
      auto* ifaceType = static_cast<sun::InterfaceType*>(to.get());
      auto* classType = static_cast<sun::ClassType*>(innerFrom.get());
      return classType->implementsInterface(ifaceType->getName());
    }
  }

  return false;
}

// -------------------------------------------------------------------
// Main analysis entry point
// -------------------------------------------------------------------

void SemanticAnalyzer::analyze(ExprAST& expr) { analyzeExpr(expr); }

// -------------------------------------------------------------------
// Expression analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeExpr(ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::NUMBER: {
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::STRING_LITERAL: {
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::BOOL_LITERAL: {
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::NULL_LITERAL: {
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::ARRAY_LITERAL: {
      auto& arrLit = static_cast<ArrayLiteralAST&>(expr);
      // Analyze each element
      for (const auto& elem : arrLit.getElements()) {
        analyzeExpr(const_cast<ExprAST&>(*elem));
      }
      // Always use inferType - it will use any expected type hint (from
      // function parameter) to widen element types if needed, while computing
      // proper dimensions
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::INDEX: {
      auto& arrIdx = static_cast<IndexAST&>(expr);
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
      // Set resolved type (element type of the array)
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::SLICE: {
      // SliceExprAST can appear standalone in some contexts
      auto& sliceExpr = static_cast<SliceExprAST&>(expr);
      if (sliceExpr.hasStart()) {
        analyzeExpr(const_cast<ExprAST&>(*sliceExpr.getStart()));
      }
      if (sliceExpr.hasEnd()) {
        analyzeExpr(const_cast<ExprAST&>(*sliceExpr.getEnd()));
      }
      expr.setResolvedType(sun::Types::Slice());
      break;
    }

    case ASTNodeType::VARIABLE_REFERENCE: {
      auto& varRef = static_cast<VariableReferenceAST&>(expr);
      // Use inferType which checks type narrowing from _is<T> guards.
      // Each AST node gets its type set based on the scope context at analysis
      // time.
      expr.setResolvedType(inferType(expr));

      // If this reference resolves to a qualified name (via using imports),
      // store it so codegen doesn't need to do name resolution
      sun::QualifiedName resolved = resolveNameWithUsings(varRef.getName());
      if (resolved.mangled() != varRef.getName()) {
        varRef.setQualifiedName(resolved);
      }
      break;
    }

    case ASTNodeType::VARIABLE_CREATION: {
      auto& varCreate = static_cast<VariableCreationAST&>(expr);
      // Determine type first (before analyzing value, for array literals)
      sun::TypePtr declaredType;
      if (varCreate.hasTypeAnnotation()) {
        declaredType = typeAnnotationToType(*varCreate.getTypeAnnotation());
        // For array literals with explicit type annotation, set the type before
        // analysis
        if (varCreate.getValue()->getType() == ASTNodeType::ARRAY_LITERAL) {
          const_cast<ExprAST&>(*varCreate.getValue())
              .setResolvedType(declaredType);
        }
      }

      // Named functions cannot be assigned to variables - only lambdas
      if (varCreate.getValue()->isFunction()) {
        logAndThrowError("Cannot assign a named function to variable '" +
                             varCreate.getName() + "'. Use a lambda instead.",
                         varCreate.getLocation());
      }

      // Analyze the value expression
      analyzeExpr(const_cast<ExprAST&>(*varCreate.getValue()));
      sun::TypePtr rhsType = varCreate.getValue()->getResolvedType();

      // Determine the final variable type
      sun::TypePtr type;
      if (declaredType) {
        // Check type compatibility: RHS must be assignable to declared type
        // This enables interface polymorphism: var s: IShape = Circle(...)
        if (rhsType && !isAssignableTo(rhsType, declaredType)) {
          // Allow integer literal coercion as a fallback
          if (!tryCoerceIntegerLiteral(
                  const_cast<ExprAST*>(varCreate.getValue()), declaredType,
                  false)) {
            logAndThrowError("Cannot assign value of type '" +
                                 rhsType->toString() + "' to variable '" +
                                 varCreate.getName() + "' of type '" +
                                 declaredType->toString() + "'",
                             varCreate.getLocation());
          }
        }
        type = declaredType;
      } else {
        type = rhsType;
      }

      validateTypeParameter(type, varCreate);

      // Note: Move semantics tracking is handled by the borrow checker
      declareVariable(varCreate.getName(), type);
      // Set the resolved type on the variable creation node itself
      expr.setResolvedType(type);
      break;
    }

    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      auto& varAssign = static_cast<VariableAssignmentAST&>(expr);

      // Named functions cannot be assigned to variables - only lambdas
      if (varAssign.getValue()->isFunction()) {
        logAndThrowError("Cannot assign a named function to variable '" +
                             varAssign.getName() + "'. Use a lambda instead.",
                         varAssign.getLocation());
      }

      analyzeExpr(const_cast<ExprAST&>(*varAssign.getValue()));
      sun::TypePtr rhsType = varAssign.getValue()->getResolvedType();

      // Look up the variable's type (important for references)
      VariableInfo* varInfo = lookupVariable(varAssign.getName());
      if (varInfo) {
        // For reference types, assignment goes through the reference to the
        // underlying value. So check assignability to the referenced type.
        sun::TypePtr targetType = varInfo->type;
        if (targetType && targetType->isReference()) {
          auto* refType = static_cast<sun::ReferenceType*>(targetType.get());
          targetType = refType->getReferencedType();
        }

        // Check type compatibility for interface polymorphism
        if (rhsType && targetType && !isAssignableTo(rhsType, targetType)) {
          // Allow integer literal coercion as a fallback
          if (!tryCoerceIntegerLiteral(
                  const_cast<ExprAST*>(varAssign.getValue()), targetType,
                  false)) {
            logAndThrowError("Cannot assign value of type '" +
                                 rhsType->toString() + "' to variable '" +
                                 varAssign.getName() + "' of type '" +
                                 varInfo->type->toString() + "'",
                             varAssign.getLocation());
          }
        }
        expr.setResolvedType(varInfo->type);
      } else {
        expr.setResolvedType(inferType(expr));
      }
      break;
    }

    case ASTNodeType::REFERENCE_CREATION: {
      auto& refCreate = static_cast<ReferenceCreationAST&>(expr);
      // Analyze the target expression
      analyzeExpr(const_cast<ExprAST&>(*refCreate.getTarget()));
      // The target must be an lvalue (variable reference or similar)
      // For now, we just check that it's a variable reference
      if (refCreate.getTarget()->getType() != ASTNodeType::VARIABLE_REFERENCE &&
          refCreate.getTarget()->getType() != ASTNodeType::MEMBER_ACCESS) {
        llvm::errs()
            << "Error: Reference target must be a variable or member\n";
      }
      // Determine the type of the referenced expression
      sun::TypePtr targetType = inferType(*refCreate.getTarget());
      // Create reference type: ref(T)
      sun::TypePtr refType = sun::Types::Reference(targetType);
      // Declare the reference variable
      declareVariable(refCreate.getName(), refType);
      // Set the resolved type
      expr.setResolvedType(refType);
      break;
    }

    case ASTNodeType::FUNCTION: {
      auto& func = static_cast<FunctionAST&>(expr);
      PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

      // Get function signature info (sets captures, converts param types)
      FunctionInfo funcInfo = getFunctionInfo(func);

      // Register function BEFORE analyzing body to support recursive calls
      // (only if named function with explicit return type)
      if (funcInfo.returnType) {
        registerFunction(proto.getName(), funcInfo);
      }

      // Analyze the function body
      analyzeFunction(func);

      // If return type was inferred (not explicit), register now
      if (!funcInfo.returnType) {
        sun::TypePtr inferredReturn =
            proto.hasReturnType() ? typeAnnotationToType(*proto.getReturnType())
                                  : sun::Types::Void();
        proto.setResolvedReturnType(inferredReturn);
        registerFunction(proto.getName(), {inferredReturn, funcInfo.paramTypes,
                                           funcInfo.captures});
      }

      // Set the function type on the function node
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::LAMBDA: {
      auto& lambda = static_cast<LambdaAST&>(expr);
      PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

      // Get lambda signature info (sets captures, converts param types)
      FunctionInfo lambdaInfo = getLambdaInfo(lambda);

      // Analyze the lambda body
      analyzeLambda(lambda);

      // If return type was inferred (not explicit), update prototype
      if (!lambdaInfo.returnType) {
        sun::TypePtr inferredReturn =
            proto.hasReturnType() ? typeAnnotationToType(*proto.getReturnType())
                                  : sun::Types::Void();
        proto.setResolvedReturnType(inferredReturn);
      }

      // Set the lambda type on the lambda node
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::BLOCK: {
      auto& block = static_cast<BlockExprAST&>(expr);
      analyzeBlock(block);
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::IF: {
      auto& ifExpr = static_cast<IfExprAST&>(expr);
      analyzeExpr(*ifExpr.getCond());

      // Check for type guard pattern: _is<T>(var)
      auto typeGuard = extractTypeGuard(*ifExpr.getCond(), *this);
      if (typeGuard) {
        // Apply type narrowing in the then-block
        enterScope();
        narrowVariable(typeGuard->first, typeGuard->second);
        analyzeExpr(*ifExpr.getThen());
        exitScope();
      } else {
        analyzeExpr(*ifExpr.getThen());
      }

      if (ifExpr.getElse()) {
        analyzeExpr(*ifExpr.getElse());
      }

      // If expression type: use inferType (it handles with/without else)
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::MATCH: {
      auto& matchExpr = static_cast<MatchExprAST&>(expr);
      // Analyze the discriminant expression
      analyzeExpr(const_cast<ExprAST&>(*matchExpr.getDiscriminant()));
      // Analyze each arm
      for (const auto& arm : matchExpr.getArms()) {
        if (arm.pattern) {
          analyzeExpr(const_cast<ExprAST&>(*arm.pattern));
        }
        analyzeExpr(const_cast<ExprAST&>(*arm.body));
      }
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::FOR_LOOP: {
      auto& forExpr = static_cast<ForExprAST&>(expr);
      // Create scope for loop variables (init may declare variables)
      enterScope();
      if (forExpr.getInit()) {
        analyzeExpr(const_cast<ExprAST&>(*forExpr.getInit()));
      }
      if (forExpr.getCondition()) {
        analyzeExpr(const_cast<ExprAST&>(*forExpr.getCondition()));
      }
      if (forExpr.getIncrement()) {
        analyzeExpr(const_cast<ExprAST&>(*forExpr.getIncrement()));
      }
      analyzeExpr(const_cast<ExprAST&>(*forExpr.getBody()));
      exitScope();
      expr.setResolvedType(sun::Types::Float64());  // for loops return 0.0
      break;
    }

    case ASTNodeType::FOR_IN_LOOP: {
      auto& forInExpr = static_cast<ForInExprAST&>(expr);
      // Analyze the iterable expression
      analyzeExpr(const_cast<ExprAST&>(*forInExpr.getIterable()));

      // Get the type of the iterable
      auto iterableType = forInExpr.getIterable()->getResolvedType();

      // Verify the iterable type implements IIterator<T> or IIterable<T>
      if (auto classType =
              std::dynamic_pointer_cast<sun::ClassType>(iterableType)) {
        bool implementsIterator = false;
        bool implementsIterable = false;

        // Check implemented interfaces for IIterator<*> or IIterable<*>
        // Interface names may be module-qualified: sun_IIterator_i32,
        // sun_IIterable_String, etc. Or unqualified: IIterator_i32,
        // IIterable_String, etc.
        for (const auto& ifaceName : classType->getImplementedInterfaces()) {
          // Check for IIterator (with or without module prefix)
          if (ifaceName.find("IIterator_") != std::string::npos ||
              ifaceName.find("IIterator<") != std::string::npos ||
              ifaceName == "IIterator") {
            implementsIterator = true;
            break;
          }
          // Check for IIterable (with or without module prefix)
          if (ifaceName.find("IIterable_") != std::string::npos ||
              ifaceName.find("IIterable<") != std::string::npos ||
              ifaceName == "IIterable") {
            implementsIterable = true;
            break;
          }
        }

        if (!implementsIterator && !implementsIterable) {
          logAndThrowError(
              "for-in loop requires type that implements IIterator<T> or "
              "IIterable<T>, but '" +
              classType->getDisplayName() + "' does not implement either");
        }
      } else {
        logAndThrowError(
            "for-in loop requires a class type that implements IIterator<T> "
            "or IIterable<T>");
      }

      // Convert loop variable type annotation to type
      auto loopVarType = typeAnnotationToType(forInExpr.getLoopVarType());
      forInExpr.setResolvedLoopVarType(loopVarType);

      // Create scope for loop body with loop variable
      enterScope();
      declareVariable(forInExpr.getLoopVar(), loopVarType);
      analyzeExpr(const_cast<ExprAST&>(*forInExpr.getBody()));
      exitScope();

      expr.setResolvedType(sun::Types::Float64());  // for-in loops return 0.0
      break;
    }

    case ASTNodeType::WHILE_LOOP: {
      auto& whileExpr = static_cast<WhileExprAST&>(expr);
      analyzeExpr(const_cast<ExprAST&>(*whileExpr.getCondition()));
      analyzeExpr(const_cast<ExprAST&>(*whileExpr.getBody()));
      expr.setResolvedType(sun::Types::Float64());  // while loops return 0.0
      break;
    }

    case ASTNodeType::BINARY: {
      auto& binExpr = static_cast<BinaryExprAST&>(expr);
      analyzeExpr(const_cast<ExprAST&>(*binExpr.getLHS()));
      analyzeExpr(const_cast<ExprAST&>(*binExpr.getRHS()));
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::UNARY: {
      auto& unaryExpr = static_cast<UnaryExprAST&>(expr);
      analyzeExpr(const_cast<ExprAST&>(*unaryExpr.getOperand()));
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::CALL: {
      auto& callExpr = static_cast<CallExprAST&>(expr);
      analyzeCall(callExpr);
      break;
    }

    case ASTNodeType::INDEXED_ASSIGNMENT: {
      auto& assignment = static_cast<IndexedAssignmentAST&>(expr);
      analyzeExpr(const_cast<ExprAST&>(*assignment.getTarget()));
      analyzeExpr(const_cast<ExprAST&>(*assignment.getValue()));

      // Get the element type from the target (what we're assigning to)
      sun::TypePtr elementType = assignment.getTarget()->getResolvedType();
      ExprAST* valueExpr = const_cast<ExprAST*>(assignment.getValue());

      // Try to coerce integer literal to target type (throws if doesn't fit)
      tryCoerceIntegerLiteral(valueExpr, elementType, /*throwOnFail=*/true);

      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::RETURN: {
      auto& returnExpr = static_cast<ReturnExprAST&>(expr);
      if (returnExpr.hasValue()) {
        analyzeExpr(const_cast<ExprAST&>(*returnExpr.getValue()));
        expr.setResolvedType(inferType(*returnExpr.getValue()));
      } else {
        expr.setResolvedType(sun::Types::Void());
      }
      break;
    }

    case ASTNodeType::IMPORT: {
      // Import statements are handled by the parser before semantic analysis.
      // Nothing to do here - the imported symbols are already registered.
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::NAMESPACE: {
      auto& nsDecl = static_cast<NamespaceAST&>(expr);
      // Enter the namespace scope
      enterModuleScope(nsDecl.getName());

      // Register this module name for qualified name resolution
      // Removes trailing underscore from prefix (e.g., "mod_x_" -> "mod_x")
      std::string modulePrefix = getCurrentModulePrefix();
      if (!modulePrefix.empty() && modulePrefix.back() == '_') {
        modulePrefix.pop_back();
      }
      registerModule(modulePrefix);

      // Analyze the body of the namespace
      for (const auto& bodyExpr : nsDecl.getBody().getBody()) {
        // For functions declared in the namespace, register them with
        // qualified names
        if (bodyExpr->isFunction()) {
          auto& func = static_cast<FunctionAST&>(*bodyExpr);
          const std::string& funcName = func.getProto().getName();
          if (!funcName.empty()) {
            // Analyze the function first
            analyzeExpr(*bodyExpr);

            // Register with qualified name - build FunctionInfo from analyzed
            // function
            sun::QualifiedName qualifiedName = makeQualifiedName(funcName);
            // Set qualified name on prototype for codegen
            const_cast<PrototypeAST&>(func.getProto())
                .setQualifiedName(qualifiedName);
            std::vector<sun::TypePtr> funcParamTypes;
            for (const auto& [argName, argType] : func.getProto().getArgs()) {
              funcParamTypes.push_back(typeAnnotationToType(argType));
            }
            sun::TypePtr funcReturnType =
                func.getProto().hasReturnType()
                    ? typeAnnotationToType(*func.getProto().getReturnType())
                    : sun::Types::Void();
            FunctionInfo funcInfo = {funcReturnType, funcParamTypes,
                                     func.getProto().getCaptures()};
            // Register in functionTable so the qualified name can be found
            // via using imports and qualified access (resolveNameWithUsings
            // and lookupQualifiedFunction both search functionTable)
            registerFunction(qualifiedName.mangled(), funcInfo);
          }
        } else if (bodyExpr->getType() == ASTNodeType::VARIABLE_CREATION) {
          // Analyze the variable creation
          analyzeExpr(*bodyExpr);

          auto& varCreate = static_cast<VariableCreationAST&>(*bodyExpr);
          std::string qualifiedName =
              qualifyNameInCurrentModule(varCreate.getName());
          sun::TypePtr type = varCreate.getResolvedType();
          if (type) {
            registerNamespacedVariable(qualifiedName, type);
          }
        } else {
          analyzeExpr(*bodyExpr);
        }
      }

      // Exit the namespace scope
      exitScope();
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::USING: {
      auto& usingDecl = static_cast<UsingAST&>(expr);
      // Check if this is "using A.B;" where A_B is actually a module name
      // In that case, treat it as "import all from A_B" (wildcard)
      std::string namespacePath = usingDecl.getNamespacePathString();
      std::string target = usingDecl.getTarget();

      if (!usingDecl.isWildcardImport() &&
          !usingDecl.isPrefixWildcardImport()) {
        // Build the full path in mangled form: "A.B" -> "A_B"
        std::string mangledPath =
            namespacePath.empty() ? target : namespacePath + "_" + target;
        // Convert dots to underscores for module lookup
        for (char& c : mangledPath) {
          if (c == '.') c = '_';
        }
        if (isModuleName(mangledPath)) {
          // Target is a module, convert to wildcard import from that module
          // Store dot-separated path for display: "A.B"
          std::string displayPath =
              namespacePath.empty() ? target : namespacePath + "." + target;
          UsingImport import(displayPath, "*");
          addUsingImport(import);
          expr.setResolvedType(sun::Types::Void());
          break;
        }
      }

      // Normal case: import symbol or wildcard from namespace
      UsingImport import(namespacePath, target);
      addUsingImport(import);
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::QUALIFIED_NAME: {
      auto& qualName = static_cast<QualifiedNameAST&>(expr);
      std::string fullName = qualName.getFullName();

      // Look up in namespaced variables first
      VariableInfo* varInfo = lookupQualifiedVariable(fullName);
      if (varInfo) {
        expr.setResolvedType(varInfo->type);
        break;
      }

      // Look up in namespaced functions
      const FunctionInfo* funcInfo = lookupQualifiedFunction(fullName);
      if (funcInfo) {
        expr.setResolvedType(
            sun::Types::Function(funcInfo->returnType, funcInfo->paramTypes));
        break;
      }

      // Unknown qualified name - default to f64
      expr.setResolvedType(sun::Types::Float64());
      break;
    }

    case ASTNodeType::CLASS_DEFINITION: {
      auto& classDef = static_cast<ClassDefinitionAST&>(expr);

      // Qualify class name with module prefix if inside a module
      sun::QualifiedName qualifiedClass = makeQualifiedName(classDef.getName());
      std::string className = qualifiedClass.mangled();
      // Set qualified name on AST for codegen (source name stays for errors)
      classDef.setQualifiedName(qualifiedClass);

      // Validate class name
      if (isReservedIdentifier(classDef.getName())) {
        logAndThrowError(
            "Class name '" + classDef.getName() +
            "' is invalid: names starting with '_' are reserved for builtins");
      }

      // Check for redefinition of builtin types
      if (typeRegistry->isBuiltinTypeName(classDef.getName())) {
        logAndThrowError("Cannot redefine builtin type '" + classDef.getName() +
                         "'");
      }

      // Validate field names
      for (const auto& field : classDef.getFields()) {
        if (isReservedIdentifier(field.name)) {
          logAndThrowError("Field name '" + field.name +
                               "' is invalid: names starting with '_' are "
                               "reserved for builtins",
                           field.location);
        }
      }

      // Validate method names
      for (const auto& methodDecl : classDef.getMethods()) {
        const std::string& methodName =
            methodDecl.function->getProto().getName();
        if (isReservedIdentifier(methodName)) {
          logAndThrowError("Method name '" + methodName +
                           "' is invalid: names starting with '_' are reserved "
                           "for builtins");
        }
      }

      // Check if this is a generic class definition
      if (classDef.isGeneric()) {
        // Register as a generic class template (not instantiated yet)
        GenericClassInfo genericInfo;
        genericInfo.AST = &classDef;
        genericInfo.typeParameters = classDef.getTypeParameters();
        registerGenericClass(className, genericInfo);
        expr.setResolvedType(sun::Types::Void());
        return;
      }

      // Check if this non-generic class has any generic methods
      // If so, register it in genericClassTable so instantiateGenericMethod can
      // find its definition
      bool hasGenericMethods = false;
      for (const auto& methodDecl : classDef.getMethods()) {
        if (methodDecl.function->getProto().isGeneric()) {
          hasGenericMethods = true;
          break;
        }
      }
      if (hasGenericMethods) {
        GenericClassInfo genericInfo;
        genericInfo.AST = &classDef;
        genericInfo.typeParameters = {};  // Non-generic class, no type params
        registerGenericClass(className, genericInfo);
      }

      // Create the class type with the qualified name
      auto classType = typeRegistry->getClass(className);
      // Store the user-written base name for error messages
      if (className != classDef.getName()) {
        classType->setBaseName(classDef.getName());
      }

      // Add fields to the class type
      for (const auto& field : classDef.getFields()) {
        if (classType->hasField(field.name)) {
          logAndThrowError("Field '" + field.name +
                               "' already exists in class '" +
                               classDef.getName() + "'",
                           field.location);
        }
        sun::TypePtr fieldType = typeAnnotationToType(field.type);

        // Check for ref types in fields
        if constexpr (sun::Config::FORBID_REF_FIELDS_IN_CLASSES) {
          if (fieldType && fieldType->isReference()) {
            logAndThrowError("Field '" + field.name + "' in class '" +
                                 classDef.getName() + "' has reference type '" +
                                 fieldType->toString() +
                                 "'. References cannot be stored in class "
                                 "fields. Use a pointer type or store a copy.",
                             field.location);
          }
        }

        classType->addField(field.name, fieldType);
      }

      // Register the class so methods can reference it (use qualified name)
      registerClass(className, classType);

      // Inherit interface fields BEFORE analyzing methods
      // This adds interface fields to the class, which methods may access
      inheritInterfaceFields(classDef, classType);

      // Save old class context and set new one
      auto savedClass = currentClass;
      setCurrentClass(classType);

      // PASS 1: Register all methods first (so methods can call each other)
      // Store FunctionInfo for each method for use in pass 2
      std::vector<FunctionInfo> methodInfos;
      for (const auto& methodDecl : classDef.getMethods()) {
        // Enter a scope for getFunctionInfo (to access 'this' for captures)
        enterScope();
        declareVariable("this", classType, /*isParam=*/true);

        // Get method signature info (sets captures, converts param types)
        FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);
        methodInfos.push_back(methodInfo);

        const PrototypeAST& proto = methodDecl.function->getProto();

        // For methods with explicit return type, we can register now
        // For methods needing inference, use Void as placeholder (will update
        // after analysis)
        sun::TypePtr returnType =
            methodInfo.returnType ? methodInfo.returnType : sun::Types::Void();

        // Add method to class type (include generic type parameters)
        classType->addMethod(proto.getName(), returnType, methodInfo.paramTypes,
                             methodDecl.isConstructor,
                             proto.getTypeParameters());

        // Register the method as a function with mangled name
        std::string mangledName =
            classType->getMangledMethodName(proto.getName());

        // For methods, add 'this' as first parameter type
        std::vector<sun::TypePtr> methodParamTypes;
        methodParamTypes.push_back(classType);  // this parameter
        for (const auto& pt : methodInfo.paramTypes) {
          methodParamTypes.push_back(pt);
        }
        registerFunction(mangledName, {returnType, methodParamTypes, {}});

        exitScope();
      }

      // PASS 2: Analyze all method bodies
      for (size_t i = 0; i < classDef.getMethods().size(); ++i) {
        const auto& methodDecl = classDef.getMethods()[i];

        // Enter a scope for the method
        enterScope(ScopeType::Function);

        // Declare 'this' as a parameter with the class type
        declareVariable("this", classType, /*isParam=*/true);

        // Analyze the method body
        analyzeFunction(*methodDecl.function);

        exitScope();
      }

      // Validate interface implementations
      validateInterfaceImplementation(classDef, classType);

      // Restore old class context
      setCurrentClass(savedClass);

      // Set resolved type to the class type so codegen can get the qualified
      // name
      expr.setResolvedType(classType);
      break;
    }

    case ASTNodeType::INTERFACE_DEFINITION: {
      auto& interfaceDef = static_cast<InterfaceDefinitionAST&>(expr);

      // Qualify interface name with module prefix if inside a module
      sun::QualifiedName qualifiedInterface =
          makeQualifiedName(interfaceDef.getName());
      std::string interfaceName = qualifiedInterface.mangled();
      // Set qualified name on AST for codegen (source name stays for errors)
      interfaceDef.setQualifiedName(qualifiedInterface);

      // Validate interface name
      if (isReservedIdentifier(interfaceDef.getName())) {
        logAndThrowError(
            "Interface name '" + interfaceDef.getName() +
            "' is invalid: names starting with '_' are reserved for builtins");
      }

      // Check for redefinition of builtin types
      if (typeRegistry->isBuiltinTypeName(interfaceDef.getName())) {
        logAndThrowError("Cannot redefine builtin interface '" +
                         interfaceDef.getName() + "'");
      }

      // Validate field names
      for (const auto& field : interfaceDef.getFields()) {
        if (isReservedIdentifier(field.name)) {
          logAndThrowError("Interface field name '" + field.name +
                           "' is invalid: names starting with '_' are reserved "
                           "for builtins");
        }
      }

      // Validate method names
      for (const auto& methodDecl : interfaceDef.getMethods()) {
        const std::string& methodName =
            methodDecl.function->getProto().getName();
        if (isReservedIdentifier(methodName)) {
          logAndThrowError("Interface method name '" + methodName +
                           "' is invalid: names starting with '_' are reserved "
                           "for builtins");
        }
      }

      // Handle generic interfaces differently
      if (interfaceDef.isGeneric()) {
        // Register as generic interface template for later instantiation
        GenericInterfaceInfo info;
        info.AST = &interfaceDef;
        info.typeParameters = interfaceDef.getTypeParameters();
        registerGenericInterface(interfaceDef.getName(), info);

        // Create a generic interface type (for type checking generic
        // references)
        auto interfaceType = typeRegistry->getGenericInterface(
            interfaceDef.getName(), interfaceDef.getTypeParameters());
        registerInterface(interfaceDef.getName(), interfaceType);

        expr.setResolvedType(sun::Types::Void());
        break;
      }

      // Non-generic interface: create the interface type directly
      auto interfaceType = typeRegistry->getInterface(interfaceName);
      // Store the user-written base name for error messages
      if (interfaceName != interfaceDef.getName()) {
        interfaceType->setBaseName(interfaceDef.getName());
      }

      // Create a pseudo-class type for 'this' during interface method analysis
      // This allows default implementations to access interface fields
      auto pseudoClass =
          typeRegistry->getClass("__interface_" + interfaceDef.getName());

      // Add fields to the interface type and pseudo-class
      for (const auto& field : interfaceDef.getFields()) {
        sun::TypePtr fieldType = typeAnnotationToType(field.type);
        interfaceType->addField(field.name, fieldType);
        pseudoClass->addField(field.name, fieldType);
      }

      // Add methods to the interface type
      for (const auto& methodDecl : interfaceDef.getMethods()) {
        // Get method signature info (sets captures, converts param types)
        FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);
        const PrototypeAST& proto = methodDecl.function->getProto();

        // Get return type
        sun::TypePtr returnType =
            methodInfo.returnType ? methodInfo.returnType : sun::Types::Void();

        // Add method to interface type (include generic type parameters)
        interfaceType->addMethod(
            proto.getName(), returnType, methodInfo.paramTypes,
            methodDecl.hasDefaultImpl, proto.getTypeParameters());

        // Analyze default method body if present
        if (methodDecl.hasDefaultImpl) {
          // Set pseudo-class as currentClass so 'this' works
          auto savedClass = currentClass;
          currentClass = pseudoClass;

          // Analyze the method body
          analyzeFunction(*methodDecl.function);

          // Restore original currentClass
          currentClass = savedClass;
        }
      }

      // Register the interface
      registerInterface(interfaceDef.getName(), interfaceType);

      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::ENUM_DEFINITION: {
      auto& enumDef = static_cast<EnumDefinitionAST&>(expr);

      // Validate enum name
      if (isReservedIdentifier(enumDef.getName())) {
        logAndThrowError(
            "Enum name '" + enumDef.getName() +
            "' is invalid: names starting with '_' are reserved for builtins");
      }

      // Validate variant names and check for duplicates
      std::set<std::string> seenVariants;
      for (const auto& variant : enumDef.getVariants()) {
        if (isReservedIdentifier(variant.name)) {
          logAndThrowError("Enum variant name '" + variant.name +
                               "' is invalid: names starting with '_' are "
                               "reserved for builtins",
                           variant.location);
        }
        if (seenVariants.count(variant.name)) {
          logAndThrowError("Duplicate enum variant '" + variant.name +
                               "' in enum '" + enumDef.getName() + "'",
                           variant.location);
        }
        seenVariants.insert(variant.name);
      }

      // Create the enum type
      auto enumType = typeRegistry->getEnum(enumDef.getName());

      // Add variants to the enum type
      for (const auto& variant : enumDef.getVariants()) {
        enumType->addVariant(variant.name, variant.value);
      }

      // Register the enum in the namespace
      registerEnum(enumDef.getName(), enumType);

      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::THIS: {
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::MEMBER_ACCESS: {
      auto& memberAccess = static_cast<MemberAccessAST&>(expr);

      // Check for enum variant access: EnumName.VariantName
      // Don't try to analyze the "object" if it's an enum type name
      bool isEnumAccess = false;
      if (memberAccess.getObject()->getType() ==
          ASTNodeType::VARIABLE_REFERENCE) {
        const auto& varRef =
            static_cast<const VariableReferenceAST&>(*memberAccess.getObject());
        if (lookupEnum(varRef.getName())) {
          isEnumAccess = true;
        }
      }

      if (!isEnumAccess) {
        // Analyze the object expression (only if not enum access)
        analyzeExpr(const_cast<ExprAST&>(*memberAccess.getObject()));
      }
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::MEMBER_ASSIGNMENT: {
      auto& memberAssign = static_cast<MemberAssignmentAST&>(expr);
      // Analyze the object and value expressions
      analyzeExpr(const_cast<ExprAST&>(*memberAssign.getObject()));
      analyzeExpr(const_cast<ExprAST&>(*memberAssign.getValue()));
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::TRY_CATCH: {
      auto& tryCatchExpr = static_cast<TryCatchExprAST&>(expr);
      // Analyze the try block
      analyzeBlock(const_cast<BlockExprAST&>(tryCatchExpr.getTryBlock()));

      // Analyze the catch clause
      const auto& catchClause = tryCatchExpr.getCatchClause();

      // Create scope for the catch body
      enterScope();

      // If there's a binding, declare it in scope
      if (!catchClause.bindingName.empty()) {
        if (!catchClause.bindingType.has_value()) {
          logAndThrowError("catch binding '" + catchClause.bindingName +
                           "' requires a type annotation, e.g., catch (" +
                           catchClause.bindingName + ": IError) { ... }");
        }
        sun::TypePtr bindingType =
            typeAnnotationToType(*catchClause.bindingType);
        declareVariable(catchClause.bindingName, bindingType);
      }

      // Analyze the catch body
      analyzeBlock(const_cast<BlockExprAST&>(*catchClause.body));

      exitScope();

      // The result type is the type of the try block
      sun::TypePtr resultType = inferType(tryCatchExpr.getTryBlock());
      expr.setResolvedType(resultType ? resultType : sun::Types::Void());
      break;
    }

    case ASTNodeType::THROW: {
      auto& throwExpr = static_cast<ThrowExprAST&>(expr);
      // Analyze the error expression being thrown
      analyzeExpr(const_cast<ExprAST&>(throwExpr.getErrorExpr()));
      // Throw doesn't return a value
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::GENERIC_CALL: {
      auto& genericCall = static_cast<GenericCallAST&>(expr);
      const std::string& funcName = genericCall.getFunctionName();
      const auto& args = genericCall.getArgs();

      // Resolve the function/class name through using imports (MatrixView ->
      // sun_MatrixView)
      std::string resolvedName = resolveNameWithUsings(funcName).mangled();

      // Resolve type arguments to sun::TypePtr
      std::vector<sun::TypePtr> typeArgs;
      for (const auto& ta : genericCall.getTypeArguments()) {
        typeArgs.push_back(typeAnnotationToType(*ta));
      }

      // Store resolved type arguments on the AST for codegen
      // This ensures codegen doesn't need to resolve type parameters
      genericCall.setResolvedTypeArgs(typeArgs);

      // validate type args
      for (auto& typeArg : typeArgs) {
        auto kind = typeArg->getKind();
        validateTypeParameter(typeArg, genericCall);
      }

      // Get type parameters from the generic function or class
      // Skip for intrinsics - they're handled at codegen time
      std::vector<std::string> typeParams;
      auto* genericClassInfo = lookupGenericClass(resolvedName);
      bool isIntrinsicCall = sun::isIntrinsic(funcName);
      if (genericClassInfo) {
        typeParams = genericClassInfo->typeParameters;
      } else if (genericFunctionTable.contains(resolvedName)) {
        auto it = genericFunctionTable.find(resolvedName);
        typeParams = it->second.typeParameters;
        // Store the generic function AST on the call node for codegen
        genericCall.setGenericFunctionAST(it->second.AST);
      } else if (!isIntrinsicCall) {
        logAndThrowError("Unknown generic function or class '" + funcName +
                         "'");
      }

      // Enter scope and bind type parameters
      enterScope();
      if (typeParams.size() == typeArgs.size()) {
        addTypeParameterBindings(typeParams, typeArgs);
      }

      // Try to get expected parameter types for array literal type propagation
      std::vector<sun::TypePtr> expectedParamTypes;
      if (genericClassInfo) {
        // Instantiate the generic class to get init method parameters
        auto specializedClass = instantiateGenericClass(resolvedName, typeArgs);
        if (specializedClass) {
          if (auto* classType =
                  dynamic_cast<sun::ClassType*>(specializedClass.get())) {
            if (auto* initMethod = classType->getMethod("init")) {
              expectedParamTypes = initMethod->paramTypes;
            }
          }
        }
      } else if (genericFunctionTable.contains(resolvedName)) {
        // Only instantiate if all type arguments are concrete (not type
        // parameters) If we're inside a generic function and T is still a type
        // parameter, we can't create a real specialization yet - it will be
        // created when the outer generic function is instantiated with concrete
        // types.
        bool allConcrete = std::all_of(
            typeArgs.begin(), typeArgs.end(),
            [](const sun::TypePtr& t) { return !t->isTypeParameter(); });
        if (allConcrete) {
          auto it = genericFunctionTable.find(resolvedName);
          auto genericFuncAST = it->second.AST;
          auto specializedFunc =
              instantiateGenericFunction(genericFuncAST, typeArgs);
          if (specializedFunc) {
            expectedParamTypes = specializedFunc->paramTypes;
          }
        }
      }

      // Propagate expected types to array literal arguments before analysis
      for (size_t i = 0; i < args.size() && i < expectedParamTypes.size();
           ++i) {
        if (args[i]->getType() == ASTNodeType::ARRAY_LITERAL) {
          sun::TypePtr paramType = expectedParamTypes[i];
          // Handle ref array<T> -> array<T>
          if (paramType && paramType->isReference()) {
            auto* refType =
                static_cast<const sun::ReferenceType*>(paramType.get());
            paramType = refType->getReferencedType();
          }
          if (paramType && paramType->isArray()) {
            const_cast<ExprAST&>(*args[i]).setResolvedType(paramType);
          }
        }
      }

      // Analyze all arguments
      for (const auto& arg : genericCall.getArgs()) {
        analyzeExpr(const_cast<ExprAST&>(*arg));
      }
      expr.setResolvedType(inferType(expr));
      exitScope();
      break;
    }

    case ASTNodeType::PACK_EXPANSION: {
      // Pack expansion is handled at codegen time
      // Just set the resolved type for now
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::DECLARE_TYPE: {
      auto& declareExpr = static_cast<DeclareTypeAST&>(expr);
      // Trigger generic instantiation by resolving the type annotation
      sun::TypePtr resolvedType =
          typeAnnotationToType(declareExpr.getTypeAnnotation());
      declareExpr.setResolvedDeclaredType(resolvedType);

      // If there's an alias, register it
      if (declareExpr.hasAlias()) {
        const std::string& aliasName = declareExpr.getAliasName();
        // Check current scope only for redefinition (shadowing is allowed)
        if (scopeStack.back().typeAliases.find(aliasName) !=
            scopeStack.back().typeAliases.end()) {
          logAndThrowError("Type alias '" + aliasName +
                           "' is already defined in this scope");
        }
        if (resolvedType) {
          scopeStack.back().typeAliases[aliasName] = resolvedType;
        }
      }

      expr.setResolvedType(sun::Types::Void());
      break;
    }

    default:
      break;
  }
}

// -------------------------------------------------------------------
// Block analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeBlock(BlockExprAST& block) {
  for (const auto& expr : block.getBody()) {
    analyzeExpr(*expr);
  }
}

// -------------------------------------------------------------------
// Helper: resolve parameter types from prototype
// -------------------------------------------------------------------

std::vector<sun::TypePtr> SemanticAnalyzer::validateAndResolveParamTypes(
    PrototypeAST& proto) {
  // Validate parameter names
  for (const auto& argName : proto.getArgNames()) {
    if (isReservedIdentifier(argName)) {
      logAndThrowError(
          "Parameter name '" + argName +
          "' is invalid: names starting with '_' are reserved for builtins");
    }
  }

  // Resolve parameter types
  std::vector<sun::TypePtr> paramTypes;
  for (auto& [argName, argType] : proto.getMutableArgs()) {
    sun::TypePtr paramType = typeAnnotationToType(argType);

    // Check for compound types being passed by value
    if constexpr (sun::Config::REQUIRE_REF_FOR_COMPOUND_PARAMS) {
      if (paramType && paramType->isCompound()) {
        // Error: compound types must be passed by reference
        logAndThrowError("Parameter '" + argName + "' has compound type '" +
                         paramType->toString() +
                         "' which cannot be passed by value. Use 'ref " +
                         paramType->toString() + "' instead.");
      }
    }
    // When REQUIRE_REF_FOR_COMPOUND_PARAMS is false, compound types are
    // passed by value with move semantics - no ref wrapping needed.

    paramTypes.push_back(paramType);
  }

  proto.setResolvedParamTypes(paramTypes);
  return paramTypes;
}

// -------------------------------------------------------------------
// Function info extraction
// -------------------------------------------------------------------

FunctionInfo SemanticAnalyzer::getFunctionInfo(FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Validate function name (if named function, not lambda)
  if (!proto.getName().empty() && isReservedIdentifier(proto.getName())) {
    logAndThrowError(
        "Function name '" + proto.getName() +
        "' is invalid: names starting with '_' are reserved for builtins");
  }

  // Build captures using current scope information
  std::vector<Capture> captures = buildCaptures(func);
  proto.setCaptures(captures);

  // For generic FREE functions (not methods), register in genericFunctionTable
  // so they can be looked up when called with type arguments.
  // We do NOT skip body analysis — codegen needs resolved types on all nodes.
  if (proto.isGeneric() && !currentClass) {
    GenericFunctionInfo genInfo;
    genInfo.AST = &func;
    genInfo.typeParameters = proto.getTypeParameters();
    if (proto.hasReturnType()) {
      genInfo.returnType = *proto.getReturnType();
    }
    genInfo.params = proto.getArgs();
    genericFunctionTable[proto.getName()] = std::move(genInfo);
  }

  // Validate and resolve parameter types using shared helper
  std::vector<sun::TypePtr> paramTypes = validateAndResolveParamTypes(proto);

  // Get return type if explicitly specified (Void if not)
  sun::TypePtr returnType;
  if (proto.hasReturnType()) {
    returnType = typeAnnotationToType(*proto.getReturnType());
  } else {
    returnType = sun::Types::Void();
  }
  proto.setResolvedReturnType(returnType);

  return {returnType, paramTypes, captures};
}

// -------------------------------------------------------------------
// Function body analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeFunction(FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // For extern functions (no body), just validate and return
  if (func.isExtern()) {
    if (!proto.hasReturnType()) {
      logAndThrowError("Extern function '" + proto.getName() +
                       "' must have an explicit return type");
    }
    return;
  }

  // Enter function scope
  enterScope(ScopeType::Function);

  // If this is a generic function/method, bind its type parameters
  if (proto.isGeneric()) {
    const auto& typeParams = proto.getTypeParameters();
    std::vector<sun::TypePtr> typeParamTypes;
    for (const auto& tp : typeParams) {
      typeParamTypes.push_back(typeAnnotationToType(TypeAnnotation(tp)));
    }
    addTypeParameterBindings(typeParams, typeParamTypes);
  }

  // Declare parameters
  for (const auto& [argName, argType] : proto.getArgs()) {
    sun::TypePtr paramType = typeAnnotationToType(argType);
    auto kind = paramType->getKind();
    declareVariable(argName, paramType, /*isParam=*/true);
  }

  // Add captured variables to scope (so nested functions can see them)
  for (const auto& cap : proto.getCaptures()) {
    declareVariable(cap.name, cap.type);
  }

  // Analyze the function body
  analyzeBlock(const_cast<BlockExprAST&>(func.getBody()));

  // Infer return type if not specified
  if (!proto.hasReturnType()) {
    sun::TypePtr returnType = inferType(func.getBody());
    // Populate the inferred return type on the prototype
    if (returnType) {
      proto.setReturnType(TypeAnnotation(returnType->toString()));
    }
  }

  exitScope();
}

// -------------------------------------------------------------------
// Lambda signature extraction
// -------------------------------------------------------------------

FunctionInfo SemanticAnalyzer::getLambdaInfo(LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  // Build captures using current scope information
  std::vector<Capture> captures = buildCaptures(lambda);
  proto.setCaptures(captures);

  // Validate and resolve parameter types using shared helper
  std::vector<sun::TypePtr> paramTypes = validateAndResolveParamTypes(proto);

  // Get return type if explicitly specified (Void if not)
  sun::TypePtr returnType;
  if (proto.hasReturnType()) {
    returnType = typeAnnotationToType(*proto.getReturnType());
  } else {
    returnType = sun::Types::Void();
  }
  proto.setResolvedReturnType(returnType);

  return {returnType, paramTypes, captures};
}

// -------------------------------------------------------------------
// Lambda body analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeLambda(LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  // Enter function scope
  enterScope(ScopeType::Function);

  // Lambdas don't have type parameters (no generic lambdas)

  // Declare parameters
  for (const auto& [argName, argType] : proto.getArgs()) {
    sun::TypePtr paramType = typeAnnotationToType(argType);
    declareVariable(argName, paramType, /*isParam=*/true);
  }

  // Add captured variables to scope (so nested functions can see them)
  for (const auto& cap : proto.getCaptures()) {
    declareVariable(cap.name, cap.type);
  }

  // Analyze the lambda body
  analyzeBlock(const_cast<BlockExprAST&>(lambda.getBody()));

  // Infer return type if not specified
  if (!proto.hasReturnType()) {
    sun::TypePtr returnType = inferType(lambda.getBody());
    if (returnType) {
      proto.setReturnType(TypeAnnotation(returnType->toString()));
    }
  }

  exitScope();
}

// -------------------------------------------------------------------
// Type parameter validation
// -------------------------------------------------------------------

void SemanticAnalyzer::validateTypeParameter(const sun::TypePtr& type,
                                             const ExprAST& node) {
  if (!type || !type->isTypeParameter()) return;

  auto* typeParam = static_cast<const sun::TypeParameterType*>(type.get());

  // Type traits (_Integer, _Float, etc.) are not scope-bound type parameters
  if (sun::isTypeTrait(typeParam->getName())) return;

  sun::TypePtr found = findTypeParameter(typeParam->getName());
  if (!found) {
    const Position& loc = node.getLocation();
    std::string msg = "Unknown type parameter '" + typeParam->getName() +
                      "' at " + std::to_string(loc.line) + ":" +
                      std::to_string(loc.column) + " in '" + node.toString() +
                      "'. This is a bug in the compiler - please report it.";
    logAndThrowError(msg, loc);
  }
}

// -------------------------------------------------------------------
// Clear resolved types (for re-analysis of shared generic ASTs)
// -------------------------------------------------------------------

void SemanticAnalyzer::clearResolvedTypes(ExprAST& expr) {
  expr.clearResolvedType();

  // Recursively clear based on expression type
  switch (expr.getType()) {
    case ASTNodeType::BLOCK: {
      auto& block = static_cast<BlockExprAST&>(expr);
      for (const auto& stmt : block.getBody()) {
        clearResolvedTypes(const_cast<ExprAST&>(*stmt));
      }
      break;
    }
    case ASTNodeType::BINARY: {
      auto& bin = static_cast<BinaryExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*bin.getLHS()));
      clearResolvedTypes(const_cast<ExprAST&>(*bin.getRHS()));
      break;
    }
    case ASTNodeType::UNARY: {
      auto& unary = static_cast<UnaryExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*unary.getOperand()));
      break;
    }
    case ASTNodeType::CALL: {
      auto& call = static_cast<CallExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*call.getCallee()));
      for (const auto& arg : call.getArgs()) {
        clearResolvedTypes(const_cast<ExprAST&>(*arg));
      }
      break;
    }
    case ASTNodeType::MEMBER_ACCESS: {
      auto& ma = static_cast<MemberAccessAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ma.getObject()));
      break;
    }
    case ASTNodeType::INDEX: {
      auto& idx = static_cast<IndexAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*idx.getTarget()));
      for (const auto& slice : idx.getIndices()) {
        if (slice->hasStart())
          clearResolvedTypes(const_cast<ExprAST&>(*slice->getStart()));
        if (slice->hasEnd())
          clearResolvedTypes(const_cast<ExprAST&>(*slice->getEnd()));
      }
      break;
    }
    case ASTNodeType::VARIABLE_CREATION: {
      auto& vc = static_cast<VariableCreationAST&>(expr);
      if (vc.getValue()) {
        clearResolvedTypes(const_cast<ExprAST&>(*vc.getValue()));
      }
      break;
    }
    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      auto& va = static_cast<VariableAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*va.getValue()));
      break;
    }
    case ASTNodeType::MEMBER_ASSIGNMENT: {
      auto& ma = static_cast<MemberAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ma.getObject()));
      clearResolvedTypes(const_cast<ExprAST&>(*ma.getValue()));
      break;
    }
    case ASTNodeType::INDEXED_ASSIGNMENT: {
      auto& ia = static_cast<IndexedAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ia.getTarget()));
      clearResolvedTypes(const_cast<ExprAST&>(*ia.getValue()));
      break;
    }
    case ASTNodeType::IF: {
      auto& ifExpr = static_cast<IfExprAST&>(expr);
      clearResolvedTypes(*ifExpr.getCond());
      clearResolvedTypes(*ifExpr.getThen());
      if (ifExpr.getElse()) {
        clearResolvedTypes(*ifExpr.getElse());
      }
      break;
    }
    case ASTNodeType::FOR_LOOP: {
      auto& loop = static_cast<ForExprAST&>(expr);
      if (loop.getInit())
        clearResolvedTypes(const_cast<ExprAST&>(*loop.getInit()));
      if (loop.getCondition())
        clearResolvedTypes(const_cast<ExprAST&>(*loop.getCondition()));
      if (loop.getIncrement())
        clearResolvedTypes(const_cast<ExprAST&>(*loop.getIncrement()));
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getBody()));
      break;
    }
    case ASTNodeType::FOR_IN_LOOP: {
      auto& loop = static_cast<ForInExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getIterable()));
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getBody()));
      break;
    }
    case ASTNodeType::WHILE_LOOP: {
      auto& loop = static_cast<WhileExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getCondition()));
      clearResolvedTypes(const_cast<ExprAST&>(*loop.getBody()));
      break;
    }
    case ASTNodeType::RETURN: {
      auto& ret = static_cast<ReturnExprAST&>(expr);
      if (ret.hasValue()) {
        clearResolvedTypes(const_cast<ExprAST&>(*ret.getValue()));
      }
      break;
    }
    case ASTNodeType::REFERENCE_CREATION: {
      auto& ref = static_cast<ReferenceCreationAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ref.getTarget()));
      break;
    }
    case ASTNodeType::GENERIC_CALL: {
      auto& gc = static_cast<GenericCallAST&>(expr);
      for (const auto& arg : gc.getArgs()) {
        clearResolvedTypes(const_cast<ExprAST&>(*arg));
      }
      break;
    }
    case ASTNodeType::TRY_CATCH: {
      auto& tc = static_cast<TryCatchExprAST&>(expr);
      clearResolvedTypes(const_cast<BlockExprAST&>(tc.getTryBlock()));
      const auto& clause = tc.getCatchClause();
      clearResolvedTypes(*clause.body);
      break;
    }
    case ASTNodeType::THROW: {
      auto& th = static_cast<ThrowExprAST&>(expr);
      if (th.hasErrorExpr()) {
        clearResolvedTypes(const_cast<ExprAST&>(th.getErrorExpr()));
      }
      break;
    }
    case ASTNodeType::ARRAY_LITERAL: {
      auto& arr = static_cast<ArrayLiteralAST&>(expr);
      for (const auto& elem : arr.getElements()) {
        clearResolvedTypes(const_cast<ExprAST&>(*elem));
      }
      break;
    }
    case ASTNodeType::MATCH: {
      auto& match = static_cast<MatchExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*match.getDiscriminant()));
      for (const auto& arm : match.getArms()) {
        clearResolvedTypes(const_cast<ExprAST&>(*arm.body));
      }
      break;
    }
    // Terminal nodes (no children to recurse into)
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
    case ASTNodeType::BOOL_LITERAL:
    case ASTNodeType::NULL_LITERAL:
    case ASTNodeType::VARIABLE_REFERENCE:
    case ASTNodeType::THIS:
    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
      break;
    default:
      // For any other node types, just clear this node (may miss children)
      break;
  }
}

// -------------------------------------------------------------------
// Lazy method parsing and analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::lazyParseAndAnalyzeMethod(
    FunctionAST& methodFunc, std::shared_ptr<sun::ClassType> classType,
    const std::vector<std::string>& typeParams,
    const std::vector<sun::TypePtr>& typeArgs) {
  // Step 1: Lazy parse if body is empty but has source text
  if (methodFunc.hasSourceText() && methodFunc.getBody().getBody().empty()) {
    auto parsedFunc =
        Parser::lazyParseFunctionSource(methodFunc.getSourceText());
    if (!parsedFunc) {
      logAndThrowError("Failed to parse lazy method body for: " +
                       methodFunc.getProto().getName());
    }
    // Transfer the parsed body to the existing FunctionAST
    methodFunc.setBody(std::make_unique<BlockExprAST>(
        std::move(const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
            parsedFunc->getBody().getBody()))));
  }

  // Step 1.5: Extract module prefix from class name (e.g., "sun_Vec" -> "sun")
  // This ensures types used in method bodies resolve correctly
  std::string modulePrefix;
  bool inModuleScope = false;
  if (classType) {
    const std::string& className = classType->getName();
    size_t underscorePos = className.find('_');
    if (underscorePos != std::string::npos) {
      modulePrefix = className.substr(0, underscorePos);
    }
    // Enter module scope if class is from a namespace
    if (!modulePrefix.empty()) {
      enterModuleScope(modulePrefix);
      // Add implicit using import for the module so unqualified names resolve
      addUsingImport(UsingImport(modulePrefix, "*"));
      inModuleScope = true;
    }
  }

  // Step 2: Set up scope with type parameter bindings
  enterScope();
  if (typeParams.size() == typeArgs.size()) {
    addTypeParameterBindings(typeParams, typeArgs);
  }

  // Step 3: Set class context for 'this' member access resolution
  auto savedClass = currentClass;
  if (classType) {
    setCurrentClass(classType);
  }

  // Step 4: Enter method scope and declare 'this' parameter
  enterScope(ScopeType::Function);
  if (classType) {
    declareVariable("this", classType, /*isParam=*/true);
  }

  // Step 5: Declare method parameters with substituted types
  const auto& proto = methodFunc.getProto();
  for (const auto& [argName, argType] : proto.getArgs()) {
    sun::TypePtr paramType = typeAnnotationToType(argType);
    paramType = substituteTypeParameters(paramType);
    declareVariable(argName, paramType, /*isParam=*/true);
  }

  // Step 5.5: Clear old resolved types before re-analysis
  // This is critical for shared generic ASTs that may have resolvedType set
  // from a previous specialization (e.g., Map<i64,i64> types on Map<i64,i32>)
  clearResolvedTypes(const_cast<BlockExprAST&>(methodFunc.getBody()));

  // Step 6: Analyze the method body
  analyzeBlock(const_cast<BlockExprAST&>(methodFunc.getBody()));

  // Step 7: Pop scopes and restore context
  exitScope();  // method scope
  exitScope();  // type param scope
  if (inModuleScope) {
    exitScope();  // module scope
  }
  setCurrentClass(savedClass);
}

// -------------------------------------------------------------------
// Call expression analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeCall(CallExprAST& callExpr) {
  // Get parameter types early for array literal type propagation
  std::vector<sun::TypePtr> expectedParamTypes;
  auto calleeASTType = callExpr.getCallee()->getType();
  if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*callExpr.getCallee());
    // Resolve the name through using imports (e.g., make_heap_allocator ->
    // sun_make_heap_allocator)
    std::string resolvedName =
        resolveNameWithUsings(varRef.getName()).mangled();
    // Try to look up function parameters
    auto allFuncs = getAllFunctions(resolvedName);
    if (!allFuncs.empty()) {
      // Use first overload's param types for type propagation
      expectedParamTypes = allFuncs[0].paramTypes;
    } else {
      // Check if this is a class constructor
      auto classType = lookupClass(resolvedName);
      if (classType) {
        // Get init method parameters
        if (auto* initMethod = classType->getMethod("init")) {
          expectedParamTypes = initMethod->paramTypes;
        }
      }
    }
  }

  // Propagate expected types to array literal arguments before analysis
  // This allows array literals to generate with the correct element type
  const auto& args = callExpr.getArgs();
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    if (args[i]->getType() == ASTNodeType::ARRAY_LITERAL) {
      sun::TypePtr paramType = expectedParamTypes[i];
      // Handle ref array<T> -> array<T>
      if (paramType && paramType->isReference()) {
        auto* refType = static_cast<const sun::ReferenceType*>(paramType.get());
        paramType = refType->getReferencedType();
      }
      if (paramType && paramType->isArray()) {
        // Set the expected type on the array literal before analysis
        const_cast<ExprAST&>(*args[i]).setResolvedType(paramType);
      }
    }
  }

  // Analyze arguments FIRST (before callee) to get types for overload
  // resolution
  for (const auto& arg : callExpr.getArgs()) {
    analyzeExpr(const_cast<ExprAST&>(*arg));
  }

  // Collect argument types for overload resolution
  std::vector<sun::TypePtr> argTypes;
  for (const auto& arg : callExpr.getArgs()) {
    argTypes.push_back(arg->getResolvedType());
  }

  // For function calls by name, do overload resolution before analyzing
  // callee This avoids errors for overloaded functions referenced by name
  std::optional<FunctionInfo> resolvedFunc;
  std::shared_ptr<sun::ClassType> classType = nullptr;
  if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
    auto& varRef = static_cast<VariableReferenceAST&>(
        const_cast<ExprAST&>(*callExpr.getCallee()));
    // Resolve the name through using imports (e.g., Vec -> sun_Vec)
    sun::QualifiedName resolved = resolveNameWithUsings(varRef.getName());
    std::string resolvedName = resolved.mangled();

    // Store the qualified name so codegen doesn't need to do name resolution
    if (resolvedName != varRef.getName()) {
      varRef.setQualifiedName(resolved);
    }

    resolvedFunc = lookupFunction(resolvedName, argTypes);
    if (resolvedFunc) {
      // Set resolved type on the callee directly
      varRef.setResolvedType(sun::Types::Function(resolvedFunc->returnType,
                                                  resolvedFunc->paramTypes));
    } else {
      // Check if this is a class constructor call: ClassName(args...)
      // This creates a stack-allocated class instance
      classType = lookupClass(resolvedName);
      if (classType) {
        // Set resolved type on the callee to indicate this is a class
        // constructor call (stack-allocated)
        varRef.setResolvedType(classType);
      } else {
        // Not a function or class - analyze normally (will check variables,
        // etc.)
        analyzeExpr(varRef);
      }
    }
  } else if (calleeASTType == ASTNodeType::MEMBER_ACCESS) {
    // Handle method calls: object.method(args...)
    const auto& memberAccess =
        static_cast<const MemberAccessAST&>(*callExpr.getCallee());
    const std::string& methodName = memberAccess.getMemberName();
    // Analyze the object expression to get its type
    analyzeExpr(const_cast<ExprAST&>(*callExpr.getCallee()));
  } else {
    // Not a simple variable reference or method call - analyze the callee
    // expression
    analyzeExpr(const_cast<ExprAST&>(*callExpr.getCallee()));
  }

  // Type check: verify argument types match parameter types
  sun::TypePtr calleeSunType = callExpr.getCallee()->getResolvedType();
  if (!calleeSunType) {
    calleeSunType = inferType(*callExpr.getCallee());
  }
  std::vector<sun::TypePtr> paramTypes;

  if (resolvedFunc) {
    paramTypes = resolvedFunc->paramTypes;
  } else if (calleeSunType && calleeSunType->isFunction()) {
    paramTypes = static_cast<const sun::FunctionType*>(calleeSunType.get())
                     ->getParamTypes();
  } else if (calleeSunType && calleeSunType->isLambda()) {
    paramTypes = static_cast<const sun::LambdaType*>(calleeSunType.get())
                     ->getParamTypes();
  } else if (classType && classType->isClass()) {
    // Class constructor call: look up init method's param types
    auto* ct = static_cast<const sun::ClassType*>(classType.get());
    const auto* initMethod = ct->getConstructor();
    if (initMethod) {
      paramTypes = initMethod->paramTypes;
    }
  }

  // Check argument count
  if (!paramTypes.empty() && args.size() != paramTypes.size()) {
    std::string funcName = "<unknown>";
    if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
      funcName = static_cast<const VariableReferenceAST&>(*callExpr.getCallee())
                     .getName();
    }
    logAndThrowError("Function '" + funcName + "' expects " +
                     std::to_string(paramTypes.size()) + " arguments, got " +
                     std::to_string(args.size()));
  }

  // If we found a function via overload resolution, types are already
  // compatible Otherwise, check each argument type manually
  if (!resolvedFunc) {
    for (size_t i = 0; i < args.size() && i < paramTypes.size(); ++i) {
      sun::TypePtr argType = args[i]->getResolvedType();
      sun::TypePtr paramType = paramTypes[i];

      // Try to coerce integer literal to parameter type
      if (tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                                  paramType)) {
        argType = paramType;  // Update for subsequent checks
      }

      if (argType && paramType && !paramType->equals(*argType)) {
        // Allow implicit conversions for compatible types
        bool compatible = false;

        // Get function name for error messages
        std::string funcName = "<unknown>";
        if (callExpr.getCallee()->getType() ==
            ASTNodeType::VARIABLE_REFERENCE) {
          funcName =
              static_cast<const VariableReferenceAST&>(*callExpr.getCallee())
                  .getName();
        }

        // Reference parameter accepts the referenced type directly
        if (paramType->isReference()) {
          auto* refType =
              static_cast<const sun::ReferenceType*>(paramType.get());
          if (refType->getReferencedType()->equals(*argType)) {
            compatible = true;
          }
          // ref array<T> (unsized) accepts any array<T, dims...>
          if (refType->getReferencedType()->isArray() && argType->isArray()) {
            auto* paramArray = static_cast<const sun::ArrayType*>(
                refType->getReferencedType().get());
            auto* argArray = static_cast<const sun::ArrayType*>(argType.get());
            if (paramArray->isUnsized() && paramArray->getElementType()->equals(
                                               *argArray->getElementType())) {
              compatible = true;
            }
          }
          // Auto-deref: raw_ptr<T> is compatible with ref T
          if (argType->isRawPointer()) {
            auto* ptrType =
                static_cast<const sun::RawPointerType*>(argType.get());
            if (ptrType->getPointeeType()->equals(
                    *refType->getReferencedType())) {
              compatible = true;
            }
          }
        }

        // Auto-deref: raw_ptr<T> can be passed where T or ref T is expected
        if (argType->isRawPointer() && !paramType->isRawPointer()) {
          auto* ptrType =
              static_cast<const sun::RawPointerType*>(argType.get());
          sun::TypePtr pointeeType = ptrType->getPointeeType();
          // For primitives, auto-deref to value is allowed
          if (pointeeType->equals(*paramType) && paramType->isPrimitive() &&
              !paramType->isReference()) {
            compatible = true;
          }
          // For any type, auto-deref to ref is allowed
          if (paramType->isReference()) {
            auto* refType =
                static_cast<const sun::ReferenceType*>(paramType.get());
            if (pointeeType->equals(*refType->getReferencedType())) {
              compatible = true;
            }
          }
        }

        // Null is compatible with any pointer type
        if (argType->isNullPointer() && paramType->isAnyPointer()) {
          compatible = true;
        }

        // Integer widening: smaller int types can be passed to larger int
        // params i8 -> i16 -> i32 -> i64, u8 -> u16 -> u32 -> u64
        if (!compatible && argType->isPrimitive() && paramType->isPrimitive()) {
          if ((argType->isInt8() || argType->isInt16() || argType->isInt32()) &&
              paramType->isInt64()) {
            compatible = true;
          } else if ((argType->isInt8() || argType->isInt16()) &&
                     paramType->isInt32()) {
            compatible = true;
          } else if (argType->isInt8() && paramType->isInt16()) {
            compatible = true;
          }
          // Unsigned widening
          else if ((argType->isUInt8() || argType->isUInt16() ||
                    argType->isUInt32()) &&
                   paramType->isUInt64()) {
            compatible = true;
          } else if ((argType->isUInt8() || argType->isUInt16()) &&
                     paramType->isUInt32()) {
            compatible = true;
          } else if (argType->isUInt8() && paramType->isUInt16()) {
            compatible = true;
          }
          // Float widening: f32 -> f64
          else if (argType->isFloat32() && paramType->isFloat64()) {
            compatible = true;
          }
        }

        // static_ptr<T> is compatible with raw_ptr<T>
        if (argType->isStaticPointer() && paramType->isRawPointer()) {
          auto* staticPtr =
              static_cast<const sun::StaticPointerType*>(argType.get());
          auto* rawPtr =
              static_cast<const sun::RawPointerType*>(paramType.get());
          if (staticPtr->getPointeeType()->equals(*rawPtr->getPointeeType())) {
            compatible = true;
          }
        }

        // raw_ptr<T> is compatible with raw_ptr<i8> (like C's void*)
        if (argType->isRawPointer() && paramType->isRawPointer()) {
          auto* paramRawPtr =
              static_cast<const sun::RawPointerType*>(paramType.get());
          if (paramRawPtr->getPointeeType()->isInt8()) {
            compatible = true;
          }
        }

        // Class-to-interface compatibility:
        // A class C can be passed where interface I is expected if C implements
        // I
        if (!compatible && isAssignableTo(argType, paramType)) {
          compatible = true;
        }

        if (!compatible) {
          logAndThrowError("Type mismatch in argument " +
                           std::to_string(i + 1) + " of call to '" + funcName +
                           "': expected " + paramType->toString() + ", got " +
                           argType->toString());
        }
      }
    }
  }

  // Note: Move semantics for ptr<T> arguments are tracked by the borrow checker

  callExpr.setResolvedType(inferType(callExpr));
}
