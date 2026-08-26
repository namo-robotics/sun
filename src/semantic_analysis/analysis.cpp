// analysis.cpp — Main analysis entry points for semantic analyzer

#include <set>
#include <unordered_set>

#include "codegen/intrinsics/intrinsics.h"
#include "semantic_analysis/generic_type_arguments.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/config.h"
#include "support/error.h"

using sun::unwrapRef;

namespace {

// "i32, ref Vec<i32>" — for call diagnostics.
std::string formatTypeList(const std::vector<sun::TypePtr>& types) {
  std::string out;
  for (size_t i = 0; i < types.size(); ++i) {
    if (i > 0) out += ", ";
    out += types[i] ? types[i]->toDisplayString() : "unknown";
  }
  return out;
}

// "\n  - trim()\n  - trim(ref HeapAllocator)" — the candidate list shown
// after "No matching overload".
std::string formatCandidates(
    const std::string& name,
    const std::vector<std::vector<sun::TypePtr>>& candidates) {
  std::string out;
  for (const auto& params : candidates) {
    out += "\n  - " + name + "(" + formatTypeList(params) + ")";
  }
  return out;
}

}  // namespace

void SemanticAnalyzer::reportNoMethodForArgCount(
    const sun::ClassType& cls, const std::string& name,
    const std::vector<sun::TypePtr>& argTypes, const Position& loc) const {
  std::vector<std::vector<sun::TypePtr>> candidates;
  for (const auto& method : cls.getMethods()) {
    if (method.name != name) continue;
    // A generic method's recorded parameters are the uninstantiated ones, so
    // their count is not something to hold the call to.
    if (method.isGeneric()) return;
    if (method.paramTypes.size() == argTypes.size()) return;
    candidates.push_back(method.paramTypes);
  }
  if (candidates.empty()) return;

  logAndThrowError(
      "No matching overload of '" + name + "' for argument types (" +
          formatTypeList(argTypes) +
          "). Available overloads:" + formatCandidates(name, candidates),
      loc);
}

// -------------------------------------------------------------------
// Main analysis entry point
// -------------------------------------------------------------------

void SemanticAnalyzer::analyze(ExprAST& expr) { analyzeExpr(expr); }

// -------------------------------------------------------------------
// Borrow targets
// -------------------------------------------------------------------

bool SemanticAnalyzer::isBorrowableLvalue(const ExprAST& target) {
  ASTNodeType kind = target.getType();
  // A conditional picks one of two slots at runtime; it borrows if both
  // branches do.
  if (kind == ASTNodeType::TERNARY) {
    const auto& ternary = static_cast<const TernaryExprAST&>(target);
    return isBorrowableLvalue(*ternary.getThen()) &&
           isBorrowableLvalue(*ternary.getElse());
  }
  return kind == ASTNodeType::VARIABLE_REFERENCE ||
         kind == ASTNodeType::MEMBER_ACCESS || kind == ASTNodeType::INDEX;
}

void SemanticAnalyzer::validateBorrowTarget(const ExprAST& target,
                                            const Position& loc) {
  if (!isBorrowableLvalue(target)) {
    logAndThrowError(
        "Reference target must be a variable, field, or array element", loc);
  }
  if (target.getType() == ASTNodeType::TERNARY) {
    const auto& ternary = static_cast<const TernaryExprAST&>(target);
    validateBorrowTarget(*ternary.getThen(), loc);
    validateBorrowTarget(*ternary.getElse(), loc);
    return;
  }
  if (target.getType() == ASTNodeType::INDEX) {
    const auto& indexExpr = static_cast<const IndexAST&>(target);
    auto baseType = sun::unwrapRef(indexExpr.getTarget()->getResolvedType());
    if (baseType && baseType->isClass()) {
      logAndThrowError(
          "Cannot create a reference to a class __index__ element - it "
          "has no storage address",
          loc);
    }
    if (indexExpr.hasSlices()) {
      logAndThrowError("Cannot create a reference to a slice", loc);
    }
  }
  checkPackedFieldNotBorrowed(target, loc);
}

// -------------------------------------------------------------------
// Expression analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeExpr(ExprAST& expr, sun::TypePtr expectedType) {
  LocationGuard locationGuard(*this, expr.getLocation());
  switch (expr.getType()) {
    case ASTNodeType::NUMBER: {
      // If we have an expected type, try to use it for integer literals
      if (expectedType && expectedType->isPrimitive()) {
        if (tryCoerceIntegerLiteral(&expr, expectedType, false)) {
          break;
        }
      }
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::CHAR_LITERAL: {
      // 'a' is always a char and b'a' is always a u8; neither takes its type
      // from context the way an integer literal does.
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

    case ASTNodeType::STRUCT_LITERAL: {
      analyzeStructLiteral(static_cast<StructLiteralAST&>(expr), expectedType);
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
      // `c[i]` on a class calls __index__ / __slice__, a method like any
      // other: it needs a mutable receiver unless declared const, and a
      // `ref T` result seen through a constant receiver is `const ref T`
      bool receiverImmutable = false;
      sun::TypePtr targetType =
          unwrapRef(arrIdx.getTarget()->getResolvedType());
      if (targetType && targetType->isClass()) {
        const auto* classType =
            static_cast<const sun::ClassType*>(targetType.get());
        const char* opName = arrIdx.hasSlices() ? "__slice__" : "__index__";
        if (const auto* method = classType->getMethod(opName)) {
          receiverImmutable = checkMethodReceiver(
              *arrIdx.getTarget(), opName, method->isConst,
              /*isConstructor=*/false, arrIdx.getLocation());
        }
      }
      // Set resolved type (element type of the array)
      sun::TypePtr resultType = inferType(expr);
      if (receiverImmutable) resultType = createConstView(resultType);
      expr.setResolvedType(resultType);
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
      expr.setResolvedType(inferType(expr));
      sun::QualifiedName resolved = resolveNameWithUsings(varRef.getName());
      varRef.setQualifiedName(resolved);
      break;
    }

    case ASTNodeType::VARIABLE_CREATION: {
      auto& varCreate = static_cast<VariableCreationAST&>(expr);
      auto varName = varCreate.getName();
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

      // Analyze the value expression, passing declared type as expected type
      analyzeExpr(const_cast<ExprAST&>(*varCreate.getValue()), declaredType);
      sun::TypePtr rhsType = varCreate.getValue()->getResolvedType();

      // Determine the final variable type
      // `var r: ref T = <lvalue>` borrows the lvalue's storage - the same
      // implicit borrow a `ref T` parameter takes at a call site. An RHS that
      // is already a reference (a call returning `ref T`) goes through
      // isAssignableTo instead.
      bool bindsBorrow = false;
      if (declaredType && declaredType->isReference() && rhsType &&
          !rhsType->isReference()) {
        auto referenced = static_cast<sun::ReferenceType*>(declaredType.get())
                              ->getReferencedType();
        if (isAssignableTo(rhsType, referenced)) {
          if (!isBorrowableLvalue(*varCreate.getValue())) {
            logAndThrowError("Cannot bind reference '" + varCreate.getName() +
                                 "' to a temporary - a reference must bind a "
                                 "variable, field, or array element",
                             varCreate.getLocation());
          }
          bindsBorrow = true;
          validateBorrowTarget(*varCreate.getValue(), varCreate.getLocation());
          if (sun::isMutableRef(declaredType)) {
            requireMutablePlace(*varCreate.getValue(),
                                "take a mutable reference to",
                                varCreate.getLocation());
          }
        }
      }
      // Taking the value moves it: a field cannot leave an immutable object
      if (!bindsBorrow) {
        checkMoveSource(*varCreate.getValue(), varCreate.getLocation());
      }

      sun::TypePtr type;
      if (declaredType) {
        // Check type compatibility: RHS must be assignable to declared type
        // This enables interface polymorphism: var s: IShape = Circle(...)
        if (!bindsBorrow && rhsType && !isAssignableTo(rhsType, declaredType)) {
          // Allow integer literal coercion as a fallback
          if (!tryCoerceIntegerLiteral(
                  const_cast<ExprAST*>(varCreate.getValue()), declaredType,
                  false)) {
            logAndThrowError(
                "Cannot assign value of type '" + rhsType->toDisplayString() +
                    "' to variable '" + varCreate.getName() + "' of type '" +
                    declaredType->toDisplayString() + "'",
                varCreate.getLocation());
          }
        }
        type = declaredType;
      } else {
        type = rhsType;
      }

      // Nothing can be stored in a variable of type void, and an inferred
      // `var` has nothing to infer from a call that returns nothing.
      if (type && sun::unwrapRef(type)->isVoid()) {
        logAndThrowError(
            declaredType ? "Variable '" + varName + "' cannot have type 'void'"
                         : "Cannot infer a type for variable '" + varName +
                               "': the value assigned to it produces no result",
            varCreate.getLocation());
      }

      validateTypeParameter(type, varCreate);

      // Note: Move semantics tracking is handled by the borrow checker
      declareVariable(varCreate.getName(), type, /*isParam=*/false,
                      varCreate.isConst());
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

      // Look up the variable's type first for expected type propagation
      VariableInfo* varInfo = lookupVariable(varAssign.getName());
      // A module-level global is emitted under its mangled name; record it so
      // codegen can find the symbol (locals keep the name as written).
      if (varInfo && varInfo->isGlobal) {
        varAssign.setQualifiedName(resolveNameWithUsings(varAssign.getName()));
      }
      if (varInfo && varInfo->isConst) {
        logAndThrowError("Cannot assign to constant '" + varAssign.getName() +
                             "'; declare it with 'var' if it must change",
                         varAssign.getLocation());
      }
      if (varInfo && sun::isConstRef(varInfo->type)) {
        logAndThrowError("Cannot assign through const reference '" +
                             varAssign.getName() + "'",
                         varAssign.getLocation());
      }
      if (varInfo && varInfo->captureKind == CaptureKind::ByValue) {
        logAndThrowError("Cannot mutate by-value captured variable '" +
                             varAssign.getName() +
                             "': capture it by reference with 'lambda [ref " +
                             varAssign.getName() + "]'",
                         varAssign.getLocation());
      }
      sun::TypePtr expectedTargetType = nullptr;
      if (varInfo) {
        expectedTargetType = varInfo->type;
        // For reference types, the target is the referenced type
        if (expectedTargetType && expectedTargetType->isReference()) {
          auto* refType =
              static_cast<sun::ReferenceType*>(expectedTargetType.get());
          expectedTargetType = refType->getReferencedType();
        }
      }

      // Analyze the value expression with expected type
      analyzeExpr(const_cast<ExprAST&>(*varAssign.getValue()),
                  expectedTargetType);
      sun::TypePtr rhsType = varAssign.getValue()->getResolvedType();
      checkMoveSource(*varAssign.getValue(), varAssign.getLocation());

      if (varInfo) {
        // Check type compatibility for interface polymorphism
        if (rhsType && expectedTargetType &&
            !isAssignableTo(rhsType, expectedTargetType)) {
          // Allow integer literal coercion as a fallback
          if (!tryCoerceIntegerLiteral(
                  const_cast<ExprAST*>(varAssign.getValue()),
                  expectedTargetType, false)) {
            logAndThrowError(
                "Cannot assign value of type '" + rhsType->toDisplayString() +
                    "' to variable '" + varAssign.getName() + "' of type '" +
                    varInfo->type->toDisplayString() + "'",
                varAssign.getLocation());
          }
        }
        expr.setResolvedType(varInfo->type);
      } else {
        expr.setResolvedType(inferType(expr));
      }
      break;
    }

    case ASTNodeType::COMPOUND_ASSIGNMENT: {
      auto& compound = static_cast<CompoundAssignmentAST&>(expr);

      // By-value captures are immutable (mirror VARIABLE_ASSIGNMENT)
      if (compound.getTarget()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
        const auto& varRef =
            static_cast<const VariableReferenceAST&>(*compound.getTarget());
        VariableInfo* varInfo = lookupVariable(varRef.getName());
        if (varInfo && varInfo->captureKind == CaptureKind::ByValue) {
          logAndThrowError("Cannot mutate by-value captured variable '" +
                               varRef.getName() +
                               "': capture it by reference with 'lambda [ref " +
                               varRef.getName() + "]'",
                           compound.getLocation());
        }
      }

      // Analyze the target as a read: gives the whole target subtree
      // resolved types (codegen signedness depends on them)
      analyzeExpr(const_cast<ExprAST&>(*compound.getTarget()));
      requireMutablePlace(*compound.getTarget(), "assign to",
                          compound.getLocation());
      sun::TypePtr targetType =
          sun::unwrapRef(compound.getTarget()->getResolvedType());

      // Analyze the value with the target's type as expected
      analyzeExpr(const_cast<ExprAST&>(*compound.getValue()), targetType);
      sun::TypePtr rhsType = compound.getValue()->getResolvedType();

      if (rhsType && targetType && !isAssignableTo(rhsType, targetType)) {
        // Allow integer literal coercion as a fallback
        if (!tryCoerceIntegerLiteral(const_cast<ExprAST*>(compound.getValue()),
                                     targetType, false)) {
          logAndThrowError(
              "Cannot apply '" + compound.getOp().text +
                  "' with value of type '" + rhsType->toDisplayString() +
                  "' to target of type '" + targetType->toDisplayString() + "'",
              compound.getLocation());
        }
      }

      // Compound assignment is a statement; codegen returns the stored value
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::REFERENCE_CREATION: {
      auto& refCreate = static_cast<ReferenceCreationAST&>(expr);
      // Analyze the target expression
      analyzeExpr(const_cast<ExprAST&>(*refCreate.getTarget()));

      validateBorrowTarget(*refCreate.getTarget(), expr.getLocation());
      // A mutable borrow needs a place that may be changed
      if (refCreate.isMutable()) {
        requireMutablePlace(*refCreate.getTarget(),
                            "take a mutable reference to", expr.getLocation());
      }
      // Determine the type of the referenced expression. Rebinding through
      // another reference borrows the same referent, not the reference.
      sun::TypePtr targetType = unwrapRef(inferType(*refCreate.getTarget()));
      // Create reference type: ref(T) or const ref(T)
      sun::TypePtr refType =
          sun::Types::Reference(targetType, refCreate.isMutable());
      // Declare the reference variable
      declareVariable(refCreate.getName(), refType);
      // Set the resolved type
      expr.setResolvedType(refType);
      break;
    }

    case ASTNodeType::FUNCTION: {
      auto& func = static_cast<FunctionAST&>(expr);
      PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

      // Get function signature info (includes qualified name with function
      // context)
      FunctionInfo funcInfo = getFunctionInfo(func);

      // Apply computed info to prototype
      applyFunctionInfoToProto(proto, funcInfo);
      proto.setQualifiedName(funcInfo.qualifiedName);

      // Register generic functions for later instantiation
      if (proto.isGeneric() && !currentClass) {
        registerGenericFunctionInCurrentScope(func);
      }

      // Only register non-generic functions in the normal function table.
      // Generic functions are looked up via genericFunctions table instead.
      if (!proto.isGeneric()) {
        registerFunctionInCurrentScope(funcInfo.qualifiedName.baseName,
                                       funcInfo);
      }

      // Analyze the function body
      analyzeFunction(func);

      // Set the function type on the function node
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::LAMBDA: {
      auto& lambda = static_cast<LambdaAST&>(expr);
      PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

      // Get lambda signature info (pure computation)
      FunctionInfo lambdaInfo = getLambdaInfo(lambda);

      // Apply computed info to prototype
      applyFunctionInfoToProto(proto, lambdaInfo);

      // Analyze the lambda body
      analyzeLambda(lambda);

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
      auto typeGuard = extractTypeGuard(*ifExpr.getCond());
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

      // Enum discriminants get variant patterns, payload bindings, and
      // exhaustiveness checking
      sun::TypePtr discType =
          unwrapRef(matchExpr.getDiscriminant()->getResolvedType());
      if (discType && discType->isEnum()) {
        analyzeEnumMatch(matchExpr,
                         std::static_pointer_cast<sun::EnumType>(discType),
                         expectedType);
        expr.setResolvedType(inferType(expr));
        break;
      }

      // Analyze each arm, propagating expectedType to arm bodies
      for (const auto& arm : matchExpr.getArms()) {
        if (arm.hasPayloadParens) {
          logAndThrowError(
              "Destructuring patterns require an enum discriminant",
              arm.pattern ? arm.pattern->getLocation() : expr.getLocation());
        }
        if (arm.pattern) {
          analyzeExpr(const_cast<ExprAST&>(*arm.pattern));
        }
        analyzeExpr(const_cast<ExprAST&>(*arm.body), expectedType);
      }
      // If we have an expected type and all arms resolved to it, use it
      if (expectedType) {
        bool allArmsMatch = true;
        for (const auto& arm : matchExpr.getArms()) {
          if (arm.body->getResolvedType() != expectedType) {
            allArmsMatch = false;
            break;
          }
        }
        if (allArmsMatch) {
          expr.setResolvedType(expectedType);
          break;
        }
      }
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::TERNARY: {
      auto& ternary = static_cast<TernaryExprAST&>(expr);
      // Condition is not required to be bool (matches if/while laxness);
      // codegen coerces numeric conditions to i1.
      analyzeExpr(*ternary.getCond());
      analyzeExpr(*ternary.getThen(), expectedType);
      analyzeExpr(*ternary.getElse(), expectedType);

      sun::TypePtr thenType =
          sun::unwrapRef(ternary.getThen()->getResolvedType());
      sun::TypePtr elseType =
          sun::unwrapRef(ternary.getElse()->getResolvedType());

      // Integer literals adopt the other branch's type: c ? x : 0
      if (thenType && elseType && !thenType->equals(*elseType)) {
        if (tryCoerceIntegerLiteral(ternary.getThen(), elseType, false)) {
          thenType = elseType;
        } else if (tryCoerceIntegerLiteral(ternary.getElse(), thenType,
                                           false)) {
          elseType = thenType;
        }
      }

      expr.setResolvedType(
          unifyTernaryTypes(thenType, elseType, expr.getLocation()));
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

      // Convert loop variable type annotation to type
      auto loopVarType = typeAnnotationToType(forInExpr.getLoopVarType());
      forInExpr.setResolvedLoopVarType(loopVarType);

      // Verify the iterable type implements IIterator<T> or IIterable<T>
      auto classType = std::dynamic_pointer_cast<sun::ClassType>(iterableType);
      if (!classType) {
        logAndThrowError(
            "for-in loop requires a class type that implements IIterator<T> "
            "or IIterable<T>",
            forInExpr.getLocation());
      }
      bool implementsIterator = false;
      bool implementsIterable = false;
      // Interface names are mangled and may be module-qualified
      // (sun_IIterator_i32_Range); match on the base name
      for (const auto& ifaceName : classType->getImplementedInterfaces()) {
        if (ifaceName.find("IIterator_") != std::string::npos ||
            ifaceName.find("IIterator<") != std::string::npos ||
            ifaceName == "IIterator") {
          implementsIterator = true;
          break;
        }
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
                classType->getDisplayName() + "' does not implement either",
            forInExpr.getLocation());
      }

      // Resolve the iterator class: the iterable itself, or what iter()
      // returns. Codegen relies on these shapes, so they are all errors here.
      std::shared_ptr<sun::ClassType> iteratorType = classType;
      if (!implementsIterator) {
        const auto* iterMethod = classType->getMethod("iter");
        iteratorType = iterMethod ? std::dynamic_pointer_cast<sun::ClassType>(
                                        sun::unwrapRef(iterMethod->returnType))
                                  : nullptr;
        if (!iteratorType) {
          logAndThrowError("for-in loop: '" + classType->getDisplayName() +
                               "' must define iter() returning an iterator "
                               "class",
                           forInExpr.getLocation());
        }
      }
      const sun::ClassMethod* nextMethod = iteratorType->getMethod("next");
      if (!nextMethod || !nextMethod->returnType) {
        logAndThrowError("for-in loop: iterator '" +
                             iteratorType->getDisplayName() +
                             "' must define next(container: ref " +
                             classType->getDisplayName() + ") Option<T>",
                         forInExpr.getLocation());
      }

      // next() takes exactly the iterable by ref: codegen passes the
      // iterable's address, so any other parameter type would reinterpret it
      bool containerOk = nextMethod->paramTypes.size() == 1 &&
                         nextMethod->paramTypes[0] &&
                         nextMethod->paramTypes[0]->isReference();
      if (containerOk) {
        sun::TypePtr paramType = sun::unwrapRef(nextMethod->paramTypes[0]);
        containerOk = paramType && (paramType->isTypeParameter() ||
                                    paramType->equals(*classType));
      }
      if (!containerOk) {
        logAndThrowError("for-in loop: iterator '" +
                             iteratorType->getDisplayName() +
                             "' must take the iterable by reference: "
                             "next(container: ref " +
                             classType->getDisplayName() + ")",
                         forInExpr.getLocation());
      }

      // The element type is the payload of next()'s Option<T>; the loop
      // variable annotation must agree with it
      sun::TypePtr elementType;
      if (auto* opt = dynamic_cast<sun::EnumType*>(
              sun::unwrapRef(nextMethod->returnType).get())) {
        const sun::EnumVariant* some = opt->getVariant("Some");
        if (some && some->payloadTypes.size() == 1 && opt->hasVariant("None")) {
          elementType = some->payloadTypes[0];
        }
      }
      if (!elementType) {
        logAndThrowError("for-in loop: iterator '" +
                             iteratorType->getDisplayName() +
                             "' must return Option<T> from next(), got '" +
                             nextMethod->returnType->toDisplayString() + "'",
                         forInExpr.getLocation());
      }
      // An iterator that yields Option<ref X> borrows: `for (var x: X in c)`
      // binds x to the element in place rather than copying it out, which is
      // what lets a container be iterated without duplicating elements it
      // still owns. Writing `ref X` in the annotation says the same thing.
      if (elementType->isReference() && loopVarType &&
          !loopVarType->isReference() &&
          sun::unwrapRef(elementType)->equals(*loopVarType)) {
        loopVarType = elementType;
        forInExpr.setResolvedLoopVarType(loopVarType);
      }
      if (loopVarType && !elementType->isTypeParameter() &&
          !loopVarType->isTypeParameter() &&
          !elementType->equals(*loopVarType)) {
        logAndThrowError("for-in loop variable '" + forInExpr.getLoopVar() +
                             "' has type '" + loopVarType->toDisplayString() +
                             "' but the iterator yields '" +
                             elementType->toDisplayString() + "'",
                         forInExpr.getLocation());
      }

      // Create scope for loop body with loop variable
      enterScope();
      declareVariable(forInExpr.getLoopVar(), loopVarType, /*isParam=*/false,
                      forInExpr.isConst());
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

      // Payload enums have no structural equality; match is the eliminator
      TokenKind binOp = binExpr.getOp().kind;
      if (binOp == TokenKind::EQUAL_EQUAL || binOp == TokenKind::NOT_EQUAL) {
        for (const ExprAST* side : {binExpr.getLHS(), binExpr.getRHS()}) {
          sun::TypePtr sideType = unwrapRef(side->getResolvedType());
          if (sideType && sideType->isEnum() &&
              static_cast<sun::EnumType*>(sideType.get())->hasPayload()) {
            logAndThrowError(
                "Cannot compare enum '" +
                    static_cast<sun::EnumType*>(sideType.get())
                        ->getDisplayName() +
                    "' with '==' ; use match to inspect payload enums",
                expr.getLocation());
          }
        }
      }
      checkCharOperands(binExpr);
      coerceBinaryLiteralOperands(binExpr, expectedType);
      expr.setResolvedType(inferType(expr));
      break;
    }

    case ASTNodeType::UNARY: {
      auto& unaryExpr = static_cast<UnaryExprAST&>(expr);
      analyzeExpr(const_cast<ExprAST&>(*unaryExpr.getOperand()));

      TokenKind op = unaryExpr.getOp().kind;
      auto operandType = sun::unwrapRef(inferType(*unaryExpr.getOperand()));

      // Unresolved generic operands are validated again at instantiation
      if (operandType && !operandType->isTypeParameter()) {
        const std::string name = operandType->toDisplayString();
        switch (op) {
          case TokenKind::NOT:
            if (!operandType->isBool()) {
              logAndThrowError(
                  "'not' requires a bool operand, got '" + name + "'",
                  expr.getLocation());
            }
            break;
          case TokenKind::TILDE:
            if (!operandType->isIntegral()) {
              logAndThrowError(
                  "Bitwise NOT (~) requires an integer operand, got '" + name +
                      "'",
                  expr.getLocation());
            }
            break;
          case TokenKind::MINUS:
            if (!operandType->isNumeric()) {
              logAndThrowError(
                  "Unary minus requires a numeric operand, got '" + name + "'",
                  expr.getLocation());
            }
            if (operandType->isUnsigned()) {
              logAndThrowError(
                  "Cannot negate a value of unsigned type '" + name + "'",
                  expr.getLocation());
            }
            break;
          default:
            break;
        }
      }

      // Matches inferType's UNARY rule without re-walking the operand subtree
      expr.setResolvedType(op == TokenKind::NOT ? sun::Types::Bool()
                                                : operandType);
      break;
    }

    case ASTNodeType::CALL: {
      auto& callExpr = static_cast<CallExprAST&>(expr);
      analyzeCall(callExpr, expectedType);
      break;
    }

    case ASTNodeType::INDEXED_ASSIGNMENT: {
      auto& assignment = static_cast<IndexedAssignmentAST&>(expr);
      analyzeExpr(const_cast<ExprAST&>(*assignment.getTarget()));
      requireMutablePlace(*assignment.getTarget(), "assign to an element of",
                          assignment.getLocation());
      analyzeExpr(const_cast<ExprAST&>(*assignment.getValue()));
      checkMoveSource(*assignment.getValue(), assignment.getLocation());

      // `obj[i] = v` on a class dispatches to __setindex__ (resolved in
      // codegen); it must be accessible from here like any other member
      if (assignment.getTarget()->getType() == ASTNodeType::INDEX) {
        const auto& idx = static_cast<const IndexAST&>(*assignment.getTarget());
        sun::TypePtr objType = unwrapRef(idx.getTarget()->getResolvedType());
        if (objType && objType->isClass()) {
          accessibleMethod(static_cast<const sun::ClassType&>(*objType),
                           "__setindex__", assignment.getLocation());
        }
      }

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
        // Propagate the function's return type for return-position inference
        // (e.g. `return Option.None;`)
        sun::TypePtr declaredReturn = currentFunctionReturnType();
        analyzeExpr(const_cast<ExprAST&>(*returnExpr.getValue()),
                    declaredReturn);
        sun::TypePtr valueType = inferType(*returnExpr.getValue());
        // Returning by value out of a borrow would hand the caller a second
        // value backed by the borrowed storage.
        if (valueType && valueType->isReference() && declaredReturn &&
            !declaredReturn->isReference() &&
            !sun::typeCopiesByRead(declaredReturn)) {
          logAndThrowError(
              "Cannot return a borrowed '" + declaredReturn->toDisplayString() +
                  "' by value: reading it out of the borrow would copy it. "
                  "Return 'ref " +
                  declaredReturn->toDisplayString() +
                  "' to keep borrowing, copy it explicitly with clone(), or "
                  "move the value out first (take()/pop()/remove() on a "
                  "container).",
              returnExpr.getLocation());
        }
        if (!declaredReturn || !declaredReturn->isReference()) {
          checkMoveSource(*returnExpr.getValue(), returnExpr.getLocation());
        }
        expr.setResolvedType(valueType);
      } else {
        expr.setResolvedType(sun::Types::Void());
      }
      break;
    }

    case ASTNodeType::MODULE: {
      auto& nsDecl = static_cast<ModuleAST&>(expr);
      // Enter the namespace scope
      declareModule(nsDecl);

      // Analyze the body of the namespace
      // Functions handle their own qualified name registration in FUNCTION case
      for (const auto& bodyExpr : nsDecl.getBody().getBody()) {
        if (bodyExpr->getType() == ASTNodeType::VARIABLE_CREATION) {
          // Variables need special handling to register in namespacedVariables
          auto& varCreate = static_cast<VariableCreationAST&>(*bodyExpr);
          if (bodyExpr->isPrecompiled()) {
            // A global imported from a .moon: the storage and its initial
            // value live in the bundle, so there is nothing to analyze — only
            // the type to resolve and the name to register.
            registerPrecompiledModuleVariable(varCreate);
            continue;
          }
          analyzeExpr(*bodyExpr);
          sun::QualifiedName qualifiedName =
              makeQualifiedName(varCreate.getName());
          varCreate.setQualifiedName(qualifiedName);
          if (auto type = varCreate.getResolvedType()) {
            registerModuleVariable(varCreate.getName(), qualifiedName.mangled(),
                                   type, varCreate.getVisibility(),
                                   varCreate.isConst());
          }
        } else if (bodyExpr->getType() == ASTNodeType::REFERENCE_CREATION) {
          analyzeExpr(*bodyExpr);
          auto& refCreate = static_cast<ReferenceCreationAST&>(*bodyExpr);
          sun::QualifiedName qualifiedName =
              makeQualifiedName(refCreate.getName());
          refCreate.setQualifiedName(qualifiedName);
        } else {
          analyzeExpr(*bodyExpr);
        }
      }

      // Exit the namespace scope
      exitScope();
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::MOON_SCOPE: {
      // MoonScopeAST wraps module stubs from a moon import
      // Enter a scope with the content hash prefix for symbol isolation
      auto& moonScope = static_cast<MoonScopeAST&>(expr);
      const std::string& contentHash = moonScope.getContentHash();
      if (!contentHash.empty()) {
        enterModuleScope(contentHash);
      }
      // Analyze contained ModuleAST nodes
      for (const auto& bodyExpr : moonScope.getBody().getBody()) {
        analyzeExpr(*bodyExpr);
      }
      if (!contentHash.empty()) {
        exitScope();
      }
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::USING: {
      registerUsing(static_cast<UsingAST&>(expr));
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::QUALIFIED_NAME: {
      auto& qualName = static_cast<QualifiedNameAST&>(expr);
      std::string fullName = qualName.getFullName();

      // Look up in namespaced variables first
      VariableInfo* varInfo = lookupQualifiedVariable(fullName);
      if (varInfo) {
        // Set resolved mangled name from the variable's qualified name
        if (!varInfo->qualifiedName.empty()) {
          qualName.setResolvedMangledName(varInfo->qualifiedName.mangled());
        }
        expr.setResolvedType(varInfo->type);
        break;
      }

      // Look up in namespaced functions - this searches all matching module
      // scopes (including same-named modules in different import scopes)
      const FunctionInfo* funcInfo = lookupQualifiedFunction(fullName);
      if (funcInfo) {
        // Set resolved mangled name from the function's actual qualified name
        // This handles same-named modules in different import scopes correctly
        if (!funcInfo->qualifiedName.empty()) {
          qualName.setResolvedMangledName(funcInfo->qualifiedName.mangled());
        }
        expr.setResolvedType(sun::Types::Function(
            funcInfo->returnType, funcInfo->paramTypes, funcInfo->canThrow));
        break;
      }

      // Unknown qualified name - default to f64
      expr.setResolvedType(sun::Types::Float64());
      break;
    }

    case ASTNodeType::CLASS_DEFINITION: {
      auto& classDef = static_cast<ClassDefinitionAST&>(expr);
      const std::string& baseName = classDef.getName();

      // Partial classes: add methods to the primary class.
      if (classDef.isPartial()) {
        analyzePartialClass(classDef, expr);
        return;
      }

      // Qualify class name with module prefix if inside a module
      // For precompiled classes (from .moon), use the qualified name from
      // metadata (includes content hash prefix for symbol isolation)
      sun::QualifiedName qualifiedClass;
      if (classDef.hasQualifiedName()) {
        qualifiedClass = classDef.getQualifiedName();
      } else {
        qualifiedClass = makeQualifiedName(baseName);
        classDef.setQualifiedName(qualifiedClass);
      }
      std::string mangledClassName = qualifiedClass.mangled();

      // Forbid redefinition of class in same module
      if (definedSymbols_.count(mangledClassName)) {
        logAndThrowError("Redefinition of class '" + baseName + "'",
                         classDef.getLocation());
      }

      // Validate class name
      validateNotReserved(classDef.getName(), "Class name",
                          classDef.getLocation());

      // Check for redefinition of builtin types
      if (typeRegistry->isBuiltinTypeName(classDef.getName())) {
        logAndThrowError(
            "Cannot redefine builtin type '" + classDef.getName() + "'",
            classDef.getLocation());
      }

      // Validate field names
      for (const auto& field : classDef.getFields()) {
        validateNotReserved(field.name, "Field name", field.location);
      }

      // Validate method names
      for (const auto& methodDecl : classDef.getMethods()) {
        const std::string& methodName =
            methodDecl.function->getProto().getName();
        validateNotReserved(methodName, "Method name",
                            methodDecl.function->getLocation());
      }

      // Register in generic class table if this is a generic class or has
      // generic methods (needed for instantiateGenericMethod to find the def)
      if (classDef.isGeneric() || classDef.hasGenericMethods()) {
        GenericClassInfo genericInfo;
        genericInfo.AST = &classDef;
        genericInfo.typeParameters = classDef.getTypeParameters();
        genericInfo.definitionScope = currentScope->shared_from_this();
        genericInfo.qualifiedName = qualifiedClass;
        registerGenericClass(baseName, genericInfo);

        // Generic class templates are not analyzed further until instantiated
        if (classDef.isGeneric()) {
          expr.setResolvedType(sun::Types::Void());
          return;
        }
      }

      // Create the class type with the qualified name
      auto classType = typeRegistry->getClass(qualifiedClass);

      // Layout must be decided before any getStructType() call memoizes it
      classType->setPacked(classDef.isPacked());
      classType->visibility = classDef.getVisibility();

      // Register the class BEFORE processing fields to allow self-referential
      // types (e.g., var next: raw_ptr<Node> inside class Node)
      registerClass(baseName, classType);

      // Fields and method signatures are normally registered by the
      // declaration pre-pass (registerClassShape); classes analyzed outside a
      // pre-passed block register them here.
      bool shapeRegistered =
          preRegisteredClassShapes_.count(mangledClassName) > 0;
      if (!shapeRegistered) {
        registerClassShape(classDef, qualifiedClass, classType);
      }

      // Inherit interface fields BEFORE analyzing methods
      // This adds interface fields to the class, which methods may access
      inheritInterfaceFields(classDef, classType);

      // Merge methods from any pending class extensions
      // Extensions are collected during import processing and merged here
      // so all methods (primary + extensions) can call each other
      auto extIt = pendingExtensions_.find(baseName);
      if (extIt != pendingExtensions_.end()) {
        for (ClassDefinitionAST* extDef : extIt->second) {
          // Validate: check for duplicate methods
          for (const auto& extMethod : extDef->getMethods()) {
            const std::string& methodName =
                extMethod.function->getProto().getName();
            // Check against primary's methods
            for (const auto& primaryMethod : classDef.getMethods()) {
              if (primaryMethod.function->getProto().getName() == methodName) {
                logAndThrowError("Method '" + methodName +
                                     "' already defined in class '" + baseName +
                                     "'",
                                 extMethod.function->getLocation());
              }
            }
            // Check against other extension methods already merged
            // (The getMutableMethods approach handles this via sequential
            // merge)
          }
          // Merge extension methods into the primary class definition
          for (auto& extMethod : extDef->getMutableMethods()) {
            classDef.getMutableMethods().push_back(std::move(extMethod));
          }
        }
        // Clear the pending extensions for this class (they're now merged)
        pendingExtensions_.erase(extIt);
      }

      // Save old class context and set new one
      auto savedClass = currentClass;
      setCurrentClass(classType);

      // Enter a Class scope to contain all method scopes in the tree
      enterClassScope(qualifiedClass);

      // PASS 1: Make the (already registered) method signatures resolvable
      // by mangled name inside the class scope
      for (const auto& methodDecl : classDef.getMethods()) {
        const PrototypeAST& proto = methodDecl.function->getProto();
        std::string mangledName =
            classType->getMangledMethodName(proto.getName());
        std::vector<sun::TypePtr> methodParamTypes;
        methodParamTypes.push_back(classType);  // this parameter
        for (const auto& pt : proto.getResolvedParamTypes()) {
          methodParamTypes.push_back(pt);
        }
        sun::TypePtr returnType = proto.hasResolvedReturnType()
                                      ? proto.getResolvedReturnType()
                                      : sun::Types::Void();
        registerFunctionInCurrentScope(mangledName,
                                       {returnType, methodParamTypes, {}});
      }

      // PASS 2: Analyze all method bodies
      for (size_t i = 0; i < classDef.getMethods().size(); ++i) {
        const auto& methodDecl = classDef.getMethods()[i];
        // Set qualified name: scopePath includes module and class context
        PrototypeAST& proto =
            const_cast<PrototypeAST&>(methodDecl.function->getProto());
        std::vector<std::string> methodScopePath = qualifiedClass.scopePath;
        methodScopePath.push_back(mangledClassName);
        proto.setQualifiedName(
            sun::QualifiedName(methodScopePath, proto.getName()));
        analyzeFunction(*methodDecl.function);
      }

      // Validate interface implementations
      validateInterfaceImplementation(classDef, classType);

      exitScope();  // Class scope

      // Restore old class context
      setCurrentClass(savedClass);

      // Track symbol for redefinition detection
      definedSymbols_.insert(mangledClassName);

      // Store primary AST for partial class merging (if a partial appears
      // later)
      currentScope->classDefinitions[baseName] = &classDef;

      // Set resolved type to the class type so codegen can get the qualified
      // name
      expr.setResolvedType(classType);
      break;
    }

    case ASTNodeType::INTERFACE_DEFINITION: {
      auto& interfaceDef = static_cast<InterfaceDefinitionAST&>(expr);

      // Qualify interface name with module prefix if inside a module
      // For precompiled interfaces (from .moon), use the qualified name from
      // metadata
      sun::QualifiedName qualifiedInterface;
      if (interfaceDef.hasQualifiedName()) {
        qualifiedInterface = interfaceDef.getQualifiedName();
      } else {
        qualifiedInterface = makeQualifiedName(interfaceDef.getName());
        interfaceDef.setQualifiedName(qualifiedInterface);
      }
      std::string interfaceName = qualifiedInterface.mangled();

      // Forbid redefinition of interface in same module
      if (definedSymbols_.count(interfaceName)) {
        logAndThrowError(
            "Redefinition of interface '" + interfaceDef.getName() + "'",
            interfaceDef.getLocation());
      }

      // Validate interface name
      validateNotReserved(interfaceDef.getName(), "Interface name",
                          interfaceDef.getLocation());

      // Check for redefinition of builtin types
      if (typeRegistry->isBuiltinTypeName(interfaceDef.getName())) {
        logAndThrowError("Cannot redefine builtin interface '" +
                             interfaceDef.getName() + "'",
                         interfaceDef.getLocation());
      }

      // Validate field names
      for (const auto& field : interfaceDef.getFields()) {
        validateNotReserved(field.name, "Interface field name", field.location);
      }

      // Validate method names
      for (const auto& methodDecl : interfaceDef.getMethods()) {
        const std::string& methodName =
            methodDecl.function->getProto().getName();
        validateNotReserved(methodName, "Interface method name",
                            methodDecl.function->getLocation());
      }

      // Handle generic interfaces differently
      if (interfaceDef.isGeneric()) {
        // Register as generic interface template for later instantiation
        GenericInterfaceInfo info;
        info.AST = &interfaceDef;
        info.typeParameters = interfaceDef.getTypeParameters();
        info.qualifiedName = qualifiedInterface;
        registerGenericInterface(interfaceDef.getName(), info);

        // Create a generic interface type (for type checking generic
        // references)
        auto interfaceType = typeRegistry->getGenericInterface(
            interfaceDef.getName(), interfaceDef.getTypeParameterNames());
        interfaceType->visibility = interfaceDef.getVisibility();
        interfaceType->setQualifiedName(qualifiedInterface);
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
      interfaceType->visibility = interfaceDef.getVisibility();
      interfaceType->setQualifiedName(qualifiedInterface);

      // Create a pseudo-class type for 'this' during interface method analysis
      // This allows default implementations to access interface fields
      auto pseudoClass =
          typeRegistry->getClass("__interface_" + interfaceDef.getName());

      // Add fields to the interface type and pseudo-class
      for (const auto& field : interfaceDef.getFields()) {
        sun::TypePtr fieldType = typeAnnotationToType(field.type);
        interfaceType->addField(field.name, fieldType).visibility =
            field.visibility;
        pseudoClass->addField(field.name, fieldType).visibility =
            field.visibility;
      }

      // Add methods to the interface type
      for (const auto& methodDecl : interfaceDef.getMethods()) {
        // Get method signature info (pure computation)
        FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);
        PrototypeAST& proto =
            const_cast<PrototypeAST&>(methodDecl.function->getProto());

        // Apply computed info to prototype
        applyFunctionInfoToProto(proto, methodInfo);

        // Add method to interface type (include generic type parameters)
        auto& method = interfaceType->addMethod(
            proto.getName(), methodInfo.returnType, methodInfo.paramTypes,
            methodDecl.hasDefaultImpl, proto.getTypeParameterNames());
        method.visibility = methodVisibility(*methodDecl.function);
        method.isConst = methodDecl.isConst;
      }

      // Enter Interface scope to contain method scopes
      enterInterfaceScope(qualifiedInterface);

      // Analyze default method bodies
      for (const auto& methodDecl : interfaceDef.getMethods()) {
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

      exitScope();  // Interface scope

      // Register the interface
      registerInterface(interfaceDef.getName(), interfaceType);

      // Track symbol for redefinition detection
      definedSymbols_.insert(interfaceName);

      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::ENUM_DEFINITION: {
      analyzeEnumDefinition(static_cast<EnumDefinitionAST&>(expr));
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
        } else if (tryAnalyzeGenericEnumUnitVariant(memberAccess,
                                                    expectedType)) {
          // Generic enum unit variant (Option.None): resolved from expected
          // type in enums.cpp
          break;
        }
      }

      if (!isEnumAccess) {
        // Analyze the object expression (only if not enum access)
        analyzeExpr(const_cast<ExprAST&>(*memberAccess.getObject()));
      }
      expr.setResolvedType(inferType(expr));
      // A method in value position becomes a bound method reference with
      // lambda type (call-position callees don't route through this case).
      maybeResolveBoundMethodRef(memberAccess, expectedType);
      break;
    }

    case ASTNodeType::MEMBER_ASSIGNMENT: {
      auto& memberAssign = static_cast<MemberAssignmentAST&>(expr);
      // Analyze the object first so the field type can flow into the value
      // as the expected type (e.g. `this.value = Option.None;`)
      analyzeExpr(const_cast<ExprAST&>(*memberAssign.getObject()));
      requireMutablePlace(
          *memberAssign.getObject(),
          "assign to field '" + memberAssign.getMemberName() + "' of",
          memberAssign.getLocation());

      sun::TypePtr objectType = memberAssign.getObject()->getResolvedType();
      objectType = unwrapRef(objectType);

      // mod.global = value: a write to a module-level variable, not a field
      if (objectType && objectType->isModule()) {
        analyzeModuleGlobalAssignment(memberAssign, *objectType);
        expr.setResolvedType(sun::Types::Void());
        break;
      }

      sun::TypePtr expectedFieldType;
      if (objectType && objectType->isClass()) {
        auto* classType = static_cast<sun::ClassType*>(objectType.get());
        if (const sun::ClassField* field =
                accessibleField(*classType, memberAssign.getMemberName(),
                                memberAssign.getLocation())) {
          expectedFieldType = field->type;
        }
      }
      analyzeExpr(const_cast<ExprAST&>(*memberAssign.getValue()),
                  expectedFieldType);
      checkMoveSource(*memberAssign.getValue(), memberAssign.getLocation());

      if (objectType && objectType->isClass()) {
        auto* classType = static_cast<sun::ClassType*>(objectType.get());
        const sun::ClassField* field =
            classType->getField(memberAssign.getMemberName());
        if (field) {
          sun::TypePtr rhsType = memberAssign.getValue()->getResolvedType();
          sun::TypePtr fieldType = field->type;

          if (rhsType && !isAssignableTo(rhsType, fieldType)) {
            // Allow integer literal coercion as a fallback
            if (!tryCoerceIntegerLiteral(
                    const_cast<ExprAST*>(memberAssign.getValue()), fieldType,
                    false)) {
              logAndThrowError(
                  "Cannot assign value of type '" + rhsType->toDisplayString() +
                      "' to field '" + memberAssign.getMemberName() +
                      "' of type '" + fieldType->toDisplayString() + "'",
                  memberAssign.getLocation());
            }
          }
        }
      }

      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::TRY_CATCH: {
      auto& tryCatchExpr = static_cast<TryCatchExprAST&>(expr);

      // Track that we're inside a try block for error propagation checking
      enterTryBlock();

      // Analyze the try block
      analyzeBlock(const_cast<BlockExprAST&>(tryCatchExpr.getTryBlock()));

      // Exit try block tracking
      exitTryBlock();

      // Analyze each catch clause, tested in source order.
      auto builtinIError = typeRegistry->getInterface("IError");
      bool sawCatchAll = false;
      auto& clauses = tryCatchExpr.getCatchClausesMutable();
      for (auto& catchClause : clauses) {
        if (catchClause.bindingName.empty() ||
            !catchClause.bindingType.has_value()) {
          logAndThrowError(
              "catch clause requires a typed binding, e.g. catch (e: IError) "
              "{ ... }",
              tryCatchExpr.getLocation());
        }

        sun::TypePtr bindingType =
            typeAnnotationToType(*catchClause.bindingType);

        // The catch type must be IError or a class implementing IError.
        bool isCatchAll = false;
        bool valid = false;
        if (bindingType && bindingType->isInterface()) {
          if (bindingType.get() == builtinIError.get()) {
            valid = true;
            isCatchAll = true;  // catch (e: IError) matches any error
          }
        } else if (bindingType && bindingType->isClass()) {
          valid = static_cast<sun::ClassType*>(bindingType.get())
                      ->implementsInterface("IError");
        }
        if (!valid) {
          logAndThrowError(
              "catch type must be 'IError' or a class implementing IError, "
              "got '" +
                  (bindingType ? bindingType->toDisplayString()
                               : std::string("?")) +
                  "'",
              tryCatchExpr.getLocation());
        }

        // A catch-all (IError) makes any following clause unreachable.
        if (sawCatchAll) {
          logAndThrowError(
              "unreachable catch clause: a 'catch (e: IError)' catch-all must "
              "be the last handler",
              tryCatchExpr.getLocation());
        }
        if (isCatchAll) sawCatchAll = true;

        // Record resolution for codegen's typed matching.
        catchClause.isCatchAll = isCatchAll;
        catchClause.resolvedMangledName =
            isCatchAll ? std::string()
                       : static_cast<sun::ClassType*>(bindingType.get())
                             ->getMangledName();

        enterScope();
        declareVariable(catchClause.bindingName, bindingType);
        analyzeBlock(const_cast<BlockExprAST&>(*catchClause.body));
        exitScope();
      }

      // The result type is the type of the try block
      sun::TypePtr resultType = inferType(tryCatchExpr.getTryBlock());
      expr.setResolvedType(resultType ? resultType : sun::Types::Void());
      break;
    }

    case ASTNodeType::UNSAFE_BLOCK: {
      auto& unsafeBlock = static_cast<UnsafeBlockAST&>(expr);

      // Track that we're inside an unsafe block
      enterUnsafeBlock();

      // Analyze the body
      analyzeBlock(unsafeBlock.getBody());

      // Exit unsafe block tracking
      exitUnsafeBlock();

      // Infer the result type (inferType handles unsafe context internally)
      sun::TypePtr resultType = inferType(expr);
      expr.setResolvedType(resultType ? resultType : sun::Types::Void());
      break;
    }

    case ASTNodeType::THROW: {
      auto& throwExpr = static_cast<ThrowExprAST&>(expr);

      // Validate that throw is used inside a function declared with ", IError"
      if (!isInThrowingFunction()) {
        logAndThrowError(
            "throw can only be used in functions declared with ', IError'",
            throwExpr.getLocation());
      }

      // Analyze the error expression being thrown
      analyzeExpr(const_cast<ExprAST&>(throwExpr.getErrorExpr()));

      // Validate that the thrown expression implements IError
      sun::TypePtr errorType = inferType(throwExpr.getErrorExpr());
      if (errorType) {
        bool implementsIError = false;

        // Get the builtin IError interface for comparison
        auto builtinIError = typeRegistry->getInterface("IError");

        // Check if it's the IError interface itself (e.g., re-throwing caught
        // error)
        if (errorType->isInterface()) {
          // IError itself is throwable
          if (errorType.get() == builtinIError.get()) {
            implementsIError = true;
          }
        }
        // Check if it's a class that implements IError
        else if (errorType->isClass()) {
          auto* classType = static_cast<sun::ClassType*>(errorType.get());
          implementsIError = classType->implementsInterface("IError");
        }
        // Check if it's a reference to a class that implements IError
        else if (errorType->isReference()) {
          auto* refType = static_cast<sun::ReferenceType*>(errorType.get());
          sun::TypePtr innerType = refType->getReferencedType();
          if (innerType && innerType->isClass()) {
            auto* classType = static_cast<sun::ClassType*>(innerType.get());
            implementsIError = classType->implementsInterface("IError");
          }
          // Also allow reference to IError interface
          else if (innerType && innerType->isInterface()) {
            if (innerType.get() == builtinIError.get()) {
              implementsIError = true;
            }
          }
        }

        if (!implementsIError) {
          logAndThrowError(
              "throw expression must be a type implementing IError, got '" +
                  errorType->toDisplayString() + "'",
              throwExpr.getLocation());
        }
      }

      // Throw doesn't return a value
      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::SPAWN: {
      auto& spawnExpr = static_cast<SpawnExprAST&>(expr);

      // Analyze the lambda expression being spawned
      analyzeExpr(const_cast<ExprAST&>(spawnExpr.getLambda()));

      // Validate that the argument is a lambda
      sun::TypePtr lambdaType = inferType(spawnExpr.getLambda());
      if (!lambdaType || !lambdaType->isLambda()) {
        logAndThrowError(
            "spawn requires a lambda expression, got '" +
                (lambdaType ? lambdaType->toDisplayString() : "unknown") + "'",
            spawnExpr.getLocation());
      }

      // The lambda should take no arguments (for now)
      auto* lambda = static_cast<sun::LambdaType*>(lambdaType.get());
      if (!lambda->getParamTypes().empty()) {
        logAndThrowError("spawn lambda must take no arguments, got " +
                             std::to_string(lambda->getParamTypes().size()) +
                             " parameter(s)",
                         spawnExpr.getLocation());
      }

      // Set the spawn expression type to Thread<T> where T is the lambda's
      // return type
      sun::TypePtr returnType = lambda->getReturnType();
      expr.setResolvedType(std::make_shared<sun::ThreadType>(returnType));
      break;
    }

    case ASTNodeType::GENERIC_CALL: {
      auto& genericCall = static_cast<GenericCallAST&>(expr);
      const std::string& funcName = genericCall.getFunctionName();

      // Resolve the function/class name through using imports
      sun::QualifiedName resolved = resolveNameWithUsings(funcName);
      const std::string& lookupName = resolved.baseName;

      // Resolve type arguments to sun::TypePtr
      std::vector<sun::TypePtr> typeArgs;
      for (const auto& ta : genericCall.getTypeArguments()) {
        typeArgs.push_back(typeAnnotationToType(*ta));
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
      auto* genericClassInfo = lookupGenericClass(lookupName);
      auto* genFuncInfo = lookupGenericFunction(lookupName);

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
        if (currentScope->typeAliases.find(aliasName) !=
            currentScope->typeAliases.end()) {
          logAndThrowError(
              "Type alias '" + aliasName + "' is already defined in this scope",
              declareExpr.getLocation());
        }
        if (resolvedType) {
          currentScope->typeAliases[aliasName] = resolvedType;
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
  // Declaration pre-pass: register all top-level declarations so that
  // ordering doesn't matter at module level.
  collectDeclarations(block);

  // Sequential analysis of all statements (bodies, expressions, etc.)
  for (const auto& expr : block.getBody()) {
    analyzeExpr(*expr);
  }
}

// -------------------------------------------------------------------
// Identifier validation
// -------------------------------------------------------------------

void SemanticAnalyzer::validateNotReserved(const std::string& name,
                                           const std::string& kind,
                                           std::optional<Position> location) {
  if (isReservedIdentifier(name)) {
    logAndThrowError(kind + " '" + name +
                         "' is invalid: names starting with '_' are "
                         "reserved for builtins",
                     location);
  }
}

// -------------------------------------------------------------------
// using declarations (idempotent: run by the pre-pass and the main pass)
// -------------------------------------------------------------------

void SemanticAnalyzer::registerUsing(UsingAST& usingDecl) {
  // "using A.B;" where A.B is a module name means "import all from A.B"
  std::string namespacePath = usingDecl.getNamespacePathString();
  std::string target = usingDecl.getTarget();

  if (!usingDecl.isModuleImport()) {
    std::string displayPath =
        namespacePath.empty() ? target : namespacePath + "." + target;
    if (auto* modScope = lookupModuleScope(displayPath)) {
      requireModuleAccessible(*modScope, usingDecl.getLocation());
      UsingImport import(displayPath, "*");
      addUsingImport(import);
      addImportBinding(ImportBinding::wildcard(modScope));
      return;
    }
  }

  // Normal case: import symbol or wildcard from namespace
  UsingImport import(namespacePath, target);
  addUsingImport(import);
  if (auto* modScope = lookupModuleScope(namespacePath)) {
    requireModuleAccessible(*modScope, usingDecl.getLocation());
    if (import.isWildcard) {
      addImportBinding(ImportBinding::wildcard(modScope));
    } else {
      addImportBinding(ImportBinding(target, modScope, target));
    }
  }
}

// -------------------------------------------------------------------
// Class shape registration (fields + method signatures)
// -------------------------------------------------------------------

void SemanticAnalyzer::registerClassShape(
    ClassDefinitionAST& classDef, const sun::QualifiedName& qualifiedClass,
    std::shared_ptr<sun::ClassType> classType) {
  std::string mangledClassName = qualifiedClass.mangled();
  if (!preRegisteredClassShapes_.insert(mangledClassName).second) return;

  // Fields
  for (const auto& field : classDef.getFields()) {
    if (classType->hasField(field.name)) {
      logAndThrowError("Field '" + field.name + "' already exists in class '" +
                           classDef.getName() + "'",
                       field.location);
    }
    sun::TypePtr fieldType = typeAnnotationToType(field.type);

    if constexpr (sun::Config::FORBID_REF_FIELDS_IN_CLASSES) {
      if (fieldType && fieldType->isReference()) {
        logAndThrowError("Field '" + field.name + "' in class '" +
                             classDef.getName() + "' has reference type '" +
                             fieldType->toDisplayString() +
                             "'. References cannot be stored in class "
                             "fields. Use a pointer type or store a copy.",
                         field.location);
      }
    }

    checkPackedFieldType(classDef, field, fieldType);
    classType->addField(field.name, fieldType).visibility = field.visibility;
  }

  // Implemented interfaces (fields inherited, implementation recorded)
  inheritInterfaceFields(classDef, classType);

  // Method signatures ('this' resolves against the class being shaped)
  auto savedClass = currentClass;
  setCurrentClass(classType);
  for (const auto& methodDecl : classDef.getMethods()) {
    FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);
    PrototypeAST& proto =
        const_cast<PrototypeAST&>(methodDecl.function->getProto());
    applyFunctionInfoToProto(proto, methodInfo);
    auto& method = classType->addMethod(
        proto.getName(), methodInfo.returnType, methodInfo.paramTypes,
        methodDecl.isConstructor, proto.getTypeParameterNames(),
        proto.canThrow());
    method.visibility = methodVisibility(*methodDecl.function);
    method.isConst = methodDecl.isConst;
  }
  setCurrentClass(savedClass);

  // The builtin IError predates all source, so it is registered with
  // message() returning static_ptr<u8> — the only string type that exists at
  // that point. The stdlib upgrades the contract: once sun.String is known,
  // IError.message() returns an owned String clone, and every implementation
  // compiled after this line must match that signature.
  if (typeRegistry && qualifiedClass.baseName == "String" &&
      !qualifiedClass.owner().empty() &&
      qualifiedClass.owner().back() == "sun") {
    if (auto ierror = typeRegistry->getInterface("IError")) {
      ierror->setMethodReturnType("message", classType);
    }
  }
}

// -------------------------------------------------------------------
// Helper: resolve parameter types from prototype
// -------------------------------------------------------------------

std::vector<sun::TypePtr> SemanticAnalyzer::validateAndResolveParamTypes(
    PrototypeAST& proto, std::optional<Position> loc,
    bool allowByValueObjects) {
  // Validate parameter names
  for (const auto& argName : proto.getArgNames()) {
    validateNotReserved(argName, "Parameter name", loc);
  }

  // Resolve parameter types
  std::vector<sun::TypePtr> paramTypes;
  for (auto& [argName, argType] : proto.getMutableArgs()) {
    sun::TypePtr paramType = typeAnnotationToType(argType);

    // Check for compound types being passed by value
    if constexpr (sun::Config::REQUIRE_REF_FOR_COMPOUND_PARAMS) {
      // C externs are exempt: passing a struct by value is what the C ABI
      // specifies, so it is the callee's signature rather than a Sun choice.
      if (!allowByValueObjects && paramType && paramType->isCompound()) {
        // Error: compound types must be passed by reference
        logAndThrowError("Parameter '" + argName + "' has compound type '" +
                             paramType->toDisplayString() +
                             "' which cannot be passed by value. Use 'ref " +
                             paramType->toDisplayString() + "' instead.",
                         loc);
      }
    }
    // When REQUIRE_REF_FOR_COMPOUND_PARAMS is false, compound types are
    // passed by value with move semantics - no ref wrapping needed.

    paramTypes.push_back(paramType);
  }

  return paramTypes;
}

// -------------------------------------------------------------------
// Function info extraction (pure computation, no side effects)
// -------------------------------------------------------------------

FunctionInfo SemanticAnalyzer::getFunctionInfo(FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // Validate function name (if named function, not lambda)
  if (!proto.getName().empty()) {
    validateNotReserved(proto.getName(), "Function name", func.getLocation());
  }

  // Build captures using current scope information
  std::vector<Capture> captures = buildCaptures(func);

  // Validate and resolve parameter types. Only C externs may take objects by
  // value; see validateAndResolveParamTypes.
  std::vector<sun::TypePtr> paramTypes = validateAndResolveParamTypes(
      proto, func.getLocation(), /*allowByValueObjects=*/func.isCExtern());

  // Resolve return type if specified; Void for constructors (no return type)
  sun::TypePtr returnType = sun::Types::Void();
  if (proto.hasReturnType()) {
    returnType = typeAnnotationToType(*proto.getReturnType());
    if (!returnType) {
      logAndThrowError("Failed to resolve return type for function '" +
                           proto.getName() + "'",
                       func.getLocation());
    }
  }

  // Compute qualified name (includes module path and function context for
  // nested functions). Precompiled stubs have pre-set qualified names with
  // content hash for symbol isolation.
  sun::QualifiedName qualifiedName;
  if (func.isCExtern()) {
    // The Sun-side name is scoped to its module like any other item, so
    // `public` and privacy mean what they say. Only the emitted symbol is
    // fixed by C — codegen takes that from the link name, never from here.
    // No overload suffix: C has no overloading.
    qualifiedName = makeQualifiedName(proto.getName());
  } else if (proto.hasQualifiedName()) {
    qualifiedName = proto.getQualifiedName();
  } else {
    qualifiedName = makeQualifiedName(proto.getName());
  }

  // Add param type suffix for overload disambiguation (unified with methods)
  // Skip for 'main' — it's an entry point with a fixed ABI name — and for
  // externs, whose ABI name is fixed by C.
  if (qualifiedName.paramSuffix.empty() && proto.getName() != "main" &&
      !func.isCExtern()) {
    qualifiedName.setParamSuffix(paramTypes);
  }

  FunctionInfo info;
  info.returnType = returnType;
  info.paramTypes = std::move(paramTypes);
  info.captures = std::move(captures);
  info.qualifiedName = qualifiedName;
  info.canThrow = proto.canThrow();
  info.isCVariadic = proto.isCVariadic();
  info.isCExtern = func.isCExtern();
  info.visibility = func.getVisibility();
  return info;
}

// -------------------------------------------------------------------
// Apply FunctionInfo to prototype
// -------------------------------------------------------------------

void SemanticAnalyzer::applyFunctionInfoToProto(PrototypeAST& proto,
                                                const FunctionInfo& info) {
  proto.setCaptures(info.captures);
  proto.setResolvedParamTypes(info.paramTypes);
  proto.setResolvedReturnType(info.returnType);
}

// -------------------------------------------------------------------
// Partial class analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzePartialClass(ClassDefinitionAST& classDef,
                                           ExprAST& expr) {
  const std::string& baseName = classDef.getName();

  auto existingClass = lookupClass(baseName);
  if (existingClass) {
    // Primary already analyzed — validate and merge methods now
    for (const auto& extMethod : classDef.getMethods()) {
      const std::string& methodName = extMethod.function->getProto().getName();
      if (existingClass->getMethod(methodName)) {
        logAndThrowError("Method '" + methodName +
                             "' already defined in class '" + baseName + "'",
                         extMethod.function->getLocation());
      }
    }

    // Register and analyze extension methods on the existing class
    auto savedClass = currentClass;
    setCurrentClass(existingClass);

    // Enter a Class scope to contain extension method scopes
    enterClassScope(existingClass->getQualifiedName());

    // Register all extension methods first
    for (const auto& methodDecl : classDef.getMethods()) {
      FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);
      PrototypeAST& proto =
          const_cast<PrototypeAST&>(methodDecl.function->getProto());

      // Apply computed info to prototype
      applyFunctionInfoToProto(proto, methodInfo);

      auto& method = existingClass->addMethod(
          proto.getName(), methodInfo.returnType, methodInfo.paramTypes,
          methodDecl.isConstructor, proto.getTypeParameterNames(),
          proto.canThrow());
      method.visibility = methodVisibility(*methodDecl.function);
      method.isConst = methodDecl.isConst;
      std::string mangledName =
          existingClass->getMangledMethodName(proto.getName());
      std::vector<sun::TypePtr> methodParamTypes;
      methodParamTypes.push_back(existingClass);
      for (const auto& pt : methodInfo.paramTypes) {
        methodParamTypes.push_back(pt);
      }
      registerFunctionInCurrentScope(
          mangledName, {methodInfo.returnType, methodParamTypes, {}});
    }

    // Analyze extension method bodies
    for (const auto& methodDecl : classDef.getMethods()) {
      analyzeFunction(*methodDecl.function);
    }

    exitScope();  // Class scope

    // Merge methods into primary AST so codegen generates them
    for (auto* s = currentScope; s != nullptr; s = s->parent) {
      auto it = s->classDefinitions.find(baseName);
      if (it != s->classDefinitions.end()) {
        for (auto& extMethod : classDef.getMutableMethods()) {
          it->second->getMutableMethods().push_back(std::move(extMethod));
        }
        break;
      }
    }

    setCurrentClass(savedClass);
  } else {
    // Primary not yet seen — stash for merging when primary is analyzed
    pendingExtensions_[baseName].push_back(&classDef);
  }
  expr.setResolvedType(sun::Types::Void());
}

// -------------------------------------------------------------------
// Function body analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeStructLiteral(StructLiteralAST& literal,
                                            const sun::TypePtr& expectedType) {
  if (!expectedType || !expectedType->isClass()) {
    logAndThrowError(
        "A '{ field: value }' literal needs a known class type. Annotate the "
        "target, as in `var x: MyClass = { ... };`.",
        literal.getLocation());
    return;
  }

  auto* classType = static_cast<sun::ClassType*>(expectedType.get());

  // A class with its own init is constructed through it; allowing both would
  // give two ways to build one object with different invariants.
  if (classType->getMethod("init")) {
    logAndThrowError("Class '" + classType->getDisplayName() +
                         "' declares an 'init', so construct it with "
                         "'" +
                         classType->getDisplayName() +
                         "(...)' rather than a '{ field: value }' literal.",
                     literal.getLocation());
    return;
  }

  std::set<std::string> seen;
  for (auto& field : literal.getMutableFields()) {
    const sun::ClassField* classField =
        accessibleField(*classType, field.name, field.location);
    if (!classField) {
      logAndThrowError("Class '" + classType->getDisplayName() +
                           "' has no field '" + field.name + "'",
                       field.location);
      continue;
    }
    if (!seen.insert(field.name).second) {
      logAndThrowError(
          "Field '" + field.name + "' is initialized more than once",
          field.location);
      continue;
    }

    analyzeExpr(*field.value, classField->type);
    sun::TypePtr valueType = field.value->getResolvedType();
    checkMoveSource(*field.value, field.location);
    if (valueType && classField->type &&
        !isAssignableTo(valueType, classField->type)) {
      if (!tryCoerceIntegerLiteral(field.value.get(), classField->type,
                                   false)) {
        logAndThrowError(
            "Cannot initialize field '" + field.name + "' of type '" +
                classField->type->toDisplayString() +
                "' with a value of type '" + valueType->toDisplayString() + "'",
            field.location);
      }
    }
  }

  // Every field must be named. A field left out would silently be zero, which
  // is exactly the class of bug this syntax exists to prevent.
  std::string missing;
  for (const auto& classField : classType->getFields()) {
    if (seen.count(classField.name)) continue;
    if (!missing.empty()) missing += ", ";
    missing += classField.name;
  }
  if (!missing.empty()) {
    logAndThrowError("Struct literal for '" + classType->getDisplayName() +
                         "' is missing field(s): " + missing,
                     literal.getLocation());
  }

  literal.setResolvedType(expectedType);
}

void SemanticAnalyzer::checkExternCallAllowed(const FunctionInfo& info,
                                              const std::string& displayName,
                                              const Position& loc) const {
  if (!info.isCExtern || isInUnsafeBlock()) return;
  logAndThrowError(
      "Calling extern function '" + displayName +
          "' requires an unsafe block: C code is outside the borrow "
          "checker's guarantees. Wrap the call in `unsafe { ... }`, or "
          "expose it through a safe Sun wrapper.",
      loc);
}

// `mod.name = value`. A module is a namespace rather than an object, so the
// target is the module's own variable: it must exist, be visible, be
// assignable, and take the value's type. Codegen writes the global directly.
void SemanticAnalyzer::analyzeModuleGlobalAssignment(
    MemberAssignmentAST& assign, const sun::Type& objectType) {
  const auto& moduleType = static_cast<const sun::ModuleType&>(objectType);
  const std::string& modPath = moduleType.getModulePath();
  const std::string& memberName = assign.getMemberName();

  SymbolMatch match = findSymbolInModule(modPath, memberName);
  if (!match) {
    logAndThrowError(
        "Unknown member '" + memberName + "' in module '" + modPath + "'",
        assign.getLocation());
  }
  if (match.kind != SymbolKind::Variable || !match.variableInfo) {
    logAndThrowError(
        "Cannot assign to '" + match.display() + "': it is not a variable",
        assign.getLocation());
  }

  const VariableInfo& target = *match.variableInfo;
  // display() names the declaring module without any library-hash scope
  std::string full = target.qualifiedName.display();
  if (target.isConst) {
    logAndThrowError("Cannot assign to constant '" + full +
                         "'; declare it with 'var' if it must change",
                     assign.getLocation());
  }
  if (sun::isConstRef(target.type)) {
    logAndThrowError("Cannot assign through const reference '" + full + "'",
                     assign.getLocation());
  }

  // The declaration's own qualified name is the symbol codegen emitted the
  // global under, so that is what the write is pointed at
  assign.setResolvedQualifiedName(target.qualifiedName.mangled());

  sun::TypePtr expectedType = unwrapRef(target.type);
  analyzeExpr(const_cast<ExprAST&>(*assign.getValue()), expectedType);
  checkMoveSource(*assign.getValue(), assign.getLocation());

  sun::TypePtr rhsType = assign.getValue()->getResolvedType();
  if (rhsType && expectedType && !isAssignableTo(rhsType, expectedType)) {
    if (!tryCoerceIntegerLiteral(const_cast<ExprAST*>(assign.getValue()),
                                 expectedType, false)) {
      logAndThrowError("Cannot assign value of type '" +
                           rhsType->toDisplayString() + "' to '" + full +
                           "' of type '" + expectedType->toDisplayString() +
                           "'",
                       assign.getLocation());
    }
  }
}

const FunctionInfo* SemanticAnalyzer::resolveModuleQualifiedCall(
    const MemberAccessAST& memberAccess, const sun::TypePtr& objectType,
    const std::vector<sun::TypePtr>& argTypes) const {
  if (!objectType || !objectType->isModule()) return nullptr;

  auto* moduleType = static_cast<sun::ModuleType*>(objectType.get());
  SymbolMatch match = findSymbolInModule(moduleType->getModulePath(),
                                         memberAccess.getMemberName(),
                                         SymbolKind::Function, &argTypes);
  if (!match || !match.functionInfo) return nullptr;

  checkExternCallAllowed(*match.functionInfo, memberAccess.getMemberName(),
                         memberAccess.getLocation());
  memberAccess.setResolvedQualifiedName(
      match.functionInfo->qualifiedName.mangled());
  return match.functionInfo;
}

void SemanticAnalyzer::validateExternSignature(FunctionAST& func) {
  const PrototypeAST& proto = func.getProto();

  // C varargs only make sense at a C boundary — a Sun function body has no
  // way to read them (no va_arg), so allowing `...` there would compile to a
  // signature nothing can use.
  if (proto.hasVariadicParam()) {
    logAndThrowError("Extern function '" + proto.getName() +
                         "' cannot use a named variadic pack; use C varargs "
                         "('...') instead",
                     func.getLocation());
  }

  auto describe = [](const sun::TypePtr& t) {
    return t ? t->toDisplayString() : std::string("<unresolved>");
  };

  // What codegen can lower to a C-compatible signature:
  //  - primitives, which map 1:1
  //  - raw_ptr<T>, a bare pointer
  //  - ref T, which also lowers to a bare pointer and so *is* C's `T*`.
  //    Class layout already matches C (declaration order, natural padding),
  //    so `ref SomeClass` is exactly `struct SomeClass*`.
  //  - classes by value, via per-target C ABI classification (see abi/c_abi.h)
  // Still excluded are the types with no C spelling at all: arrays and slices
  // (fat pointers), interfaces (vtable pairs), lambdas (closures), and
  // error unions.
  // Payload enums have a Sun-private tagged-union layout with no C ABI
  // classification yet; only payload-free (i32) enums cross the C boundary.
  auto isCStyleEnum = [](const sun::TypePtr& t) {
    return t->isEnum() &&
           !static_cast<const sun::EnumType*>(t.get())->hasPayload();
  };
  auto isABISafeParam = [&](const sun::TypePtr& t) {
    return t && (t->isPrimitive() || t->isRawPointer() || t->isReference() ||
                 t->isClass() || isCStyleEnum(t));
  };
  // Returns allow the same, minus `ref`: Sun's ref return has auto-deref
  // semantics that do not correspond to anything C returns. Use raw_ptr<T>
  // for a returned pointer.
  auto isABISafeReturn = [&](const sun::TypePtr& t) {
    return t && (t->isPrimitive() || t->isRawPointer() || t->isClass() ||
                 isCStyleEnum(t));
  };

  if (proto.hasResolvedParamTypes()) {
    const auto& params = proto.getResolvedParamTypes();
    for (size_t i = 0; i < params.size(); ++i) {
      if (params[i] && params[i]->isVoid()) {
        logAndThrowError("Parameter '" + proto.getArgs()[i].first +
                             "' of extern function '" + proto.getName() +
                             "' cannot be void",
                         func.getLocation());
      }
      if (!isABISafeParam(params[i])) {
        logAndThrowError(
            "Parameter '" + proto.getArgs()[i].first +
                "' of extern function '" + proto.getName() + "' has type '" +
                describe(params[i]) +
                "', which has no C equivalent. Extern parameters must be a "
                "primitive, an enum, raw_ptr<T>, ref T (which is C's T*), or "
                "a class (passed by value per the C ABI).",
            func.getLocation());
      }
    }
  }

  if (proto.hasResolvedReturnType() &&
      !isABISafeReturn(proto.getResolvedReturnType())) {
    logAndThrowError(
        "Extern function '" + proto.getName() + "' returns '" +
            describe(proto.getResolvedReturnType()) +
            "', which has no C equivalent. Extern return types must be a "
            "primitive, an enum, raw_ptr<T>, or a class. Note that `ref T` "
            "cannot be returned; use raw_ptr<T>.",
        func.getLocation());
  }
}

void SemanticAnalyzer::analyzeFunction(FunctionAST& func) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(func.getProto());

  // For extern functions (no body), just validate and return
  if (func.isExtern()) {
    if (!proto.hasReturnType()) {
      logAndThrowError("Extern function '" + proto.getName() +
                           "' must have an explicit return type",
                       func.getLocation());
    }
    if (func.isCExtern()) validateExternSignature(func);
    return;
  }

  // Sun has no va_arg, so C varargs are only meaningful on an extern
  // declaration where the callee is C code.
  if (proto.isCVariadic()) {
    logAndThrowError(
        "C varargs ('...') are only allowed on 'extern function' "
        "declarations; '" +
            proto.getName() + "' has a body",
        func.getLocation());
  }

  // Compute function signature from qualified name and resolved param types
  // This signature is used to create unique names for nested functions
  std::string funcSig = getFunctionSignature(proto.getMangledName(),
                                             proto.getResolvedParamTypes());

  // Return type for return-position inference. Some paths (class method
  // pass 2) reach here before the proto's resolved return type is applied;
  // resolve the annotation in the current scope (type parameter bindings for
  // specialized classes are active here).
  sun::TypePtr scopeReturnType = proto.getResolvedReturnType();
  if (!scopeReturnType && proto.hasReturnType() && !proto.isGeneric()) {
    scopeReturnType =
        substituteTypeParameters(typeAnnotationToType(*proto.getReturnType()));
  }

  // Enter function scope with signature for nested function qualification
  // Pass canThrow flag so throw expressions can be validated. A const method
  // body sees the const view of its return type: borrows of `this` are
  // `const ref` there, and the declared `ref` result is what callers with a
  // mutable receiver get.
  if (proto.isConstMethod()) scopeReturnType = createConstView(scopeReturnType);
  enterFunctionScope(funcSig, proto.getQualifiedName(), proto.canThrow(),
                     scopeReturnType);

  // Declare 'this' for methods (when we're inside a class context); it is
  // immutable inside a const method
  if (currentClass) {
    declareVariable("this", currentClass, /*isParam=*/true,
                    /*isConst=*/proto.isConstMethod());
  }

  // If this is a generic function/method, bind each type parameter to itself
  // so the body can be analyzed before any specialization exists. The binding
  // carries the parameter's constraint, which is what lets `<T: IShape>` reach
  // IShape's members on a value of type T (see inferMemberAccessType).
  if (proto.isGeneric()) {
    std::vector<std::string> typeParams;
    std::vector<sun::TypePtr> typeParamTypes;
    for (const auto& tp : proto.getTypeParameters()) {
      typeParams.push_back(tp.name);
      typeParamTypes.push_back(sun::Types::TypeParameter(
          tp.name, tp.constraint ? tp.constraint->name : ""));
    }
    addTypeParameterBindings(typeParams, typeParamTypes);
  }

  // Declare parameters
  for (const auto& [argName, argType] : proto.getArgs()) {
    sun::TypePtr paramType = typeAnnotationToType(argType);
    declareVariable(argName, paramType, /*isParam=*/true);
  }

  // Add captured variables to scope (so nested functions can see them),
  // marked as captures so mutation checks and nested capture lists can
  // distinguish them from ordinary locals
  for (const auto& cap : proto.getCaptures()) {
    declareVariable(cap.name, cap.type);
    if (VariableInfo* vi = lookupVariable(cap.name)) {
      vi->captureKind = cap.kind;
      vi->isConst = cap.isConst;
    }
  }

  // Analyze the function body
  analyzeBlock(const_cast<BlockExprAST&>(func.getBody()));

  exitScope();
}

// -------------------------------------------------------------------
// Lambda signature extraction (pure computation, no side effects)
// -------------------------------------------------------------------

FunctionInfo SemanticAnalyzer::getLambdaInfo(LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  // Build captures using current scope information
  std::vector<Capture> captures = buildCaptures(lambda);

  // Validate and resolve parameter types
  std::vector<sun::TypePtr> paramTypes = validateAndResolveParamTypes(proto);

  // Resolve return type (Sun requires return type annotations on lambdas,
  // parser enforces this, but check defensively)
  sun::TypePtr returnType = sun::Types::Void();
  if (proto.hasReturnType()) {
    returnType = typeAnnotationToType(*proto.getReturnType());
    if (!returnType) {
      logAndThrowError("Failed to resolve return type for lambda",
                       lambda.getLocation());
    }
  }

  return {returnType, paramTypes, captures};
}

// -------------------------------------------------------------------
// Lambda body analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeLambda(LambdaAST& lambda) {
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambda.getProto());

  // Enter function scope (empty signature - lambdas are anonymous)
  // Nested functions in lambdas will still get outer function prefixes
  // Pass canThrow flag from the lambda's prototype
  enterFunctionScope("", sun::QualifiedName(), proto.canThrow(),
                     proto.getResolvedReturnType());

  // Lambdas don't have type parameters (no generic lambdas)

  // Declare parameters
  for (const auto& [argName, argType] : proto.getArgs()) {
    sun::TypePtr paramType = typeAnnotationToType(argType);
    declareVariable(argName, paramType, /*isParam=*/true);
  }

  // Add captured variables to scope (so nested functions can see them),
  // marked as captures so mutation checks and nested capture lists can
  // distinguish them from ordinary locals
  for (const auto& cap : proto.getCaptures()) {
    declareVariable(cap.name, cap.type);
    if (VariableInfo* vi = lookupVariable(cap.name)) {
      vi->captureKind = cap.kind;
      vi->isConst = cap.isConst;
    }
  }

  // Analyze the lambda body
  analyzeBlock(const_cast<BlockExprAST&>(lambda.getBody()));

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
    case ASTNodeType::COMPOUND_ASSIGNMENT: {
      auto& ca = static_cast<CompoundAssignmentAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(*ca.getTarget()));
      clearResolvedTypes(const_cast<ExprAST&>(*ca.getValue()));
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
    case ASTNodeType::TERNARY: {
      auto& ternary = static_cast<TernaryExprAST&>(expr);
      clearResolvedTypes(*ternary.getCond());
      clearResolvedTypes(*ternary.getThen());
      clearResolvedTypes(*ternary.getElse());
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
      for (const auto& clause : tc.getCatchClauses()) {
        clearResolvedTypes(*clause.body);
      }
      break;
    }
    case ASTNodeType::UNSAFE_BLOCK: {
      auto& ub = static_cast<UnsafeBlockAST&>(expr);
      clearResolvedTypes(ub.getBody());
      break;
    }
    case ASTNodeType::THROW: {
      auto& th = static_cast<ThrowExprAST&>(expr);
      if (th.hasErrorExpr()) {
        clearResolvedTypes(const_cast<ExprAST&>(th.getErrorExpr()));
      }
      break;
    }
    case ASTNodeType::SPAWN: {
      auto& sp = static_cast<SpawnExprAST&>(expr);
      clearResolvedTypes(const_cast<ExprAST&>(sp.getLambda()));
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
      for (auto& arm : match.getArmsMutable()) {
        if (arm.pattern) {
          clearResolvedTypes(*arm.pattern);
        }
        arm.resolvedVariantTag = -1;
        for (auto& binding : arm.bindings) {
          binding.resolvedType = nullptr;
          binding.resolvedMangledName.clear();
        }
        clearResolvedTypes(*arm.body);
      }
      break;
    }
    // Terminal nodes (no children to recurse into)
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
    case ASTNodeType::CHAR_LITERAL:
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
// Method analysis with type bindings
// -------------------------------------------------------------------

// Analyze one (cloned) method body of a specialized class. The caller has
// entered the specialized class's scope inside the template's definition
// scope, so the body sees exactly the names the template was written against.
void SemanticAnalyzer::analyzeMethodWithBindings(
    FunctionAST& methodFunc, std::shared_ptr<sun::ClassType> classType,
    const std::vector<std::string>& typeParams,
    const std::vector<sun::TypePtr>& typeArgs) {
  // Step 2: Set up scope with type parameter bindings (only if needed)
  // For generic class methods, type bindings are already in the Class scope
  bool needsTypeParamScope =
      !typeParams.empty() && typeParams.size() == typeArgs.size();
  if (needsTypeParamScope) {
    enterTypeParamScope(typeParams, typeArgs);
  }

  // Step 3: Set class context for 'this' member access resolution
  auto savedClass = currentClass;
  if (classType) {
    setCurrentClass(classType);
  }

  // Step 4: Enter method scope and declare 'this' parameter
  // Compute method signature with substituted param types for nested function
  // qualification
  const auto& proto = methodFunc.getProto();
  std::vector<sun::TypePtr> substitutedParamTypes;
  for (const auto& [argName, argType] : proto.getArgs()) {
    sun::TypePtr paramType = typeAnnotationToType(argType);
    paramType = substituteTypeParameters(paramType);
    substitutedParamTypes.push_back(paramType);
  }
  std::string methodSig = getFunctionSignature(
      classType->getMangledMethodName(proto.getName()), substitutedParamTypes);
  std::string mangledMethodName =
      classType->getMangledMethodName(proto.getName());
  // Resolve the return type under the active bindings so return-position
  // inference (e.g. `return Option.None;`) has the expected type
  sun::TypePtr methodReturnType;
  if (proto.hasReturnType()) {
    methodReturnType =
        substituteTypeParameters(typeAnnotationToType(*proto.getReturnType()));
  }
  // A const method body sees the const view of its return type
  if (proto.isConstMethod())
    methodReturnType = createConstView(methodReturnType);
  enterFunctionScope(methodSig,
                     sun::QualifiedName(classType->getQualifiedName().scopePath,
                                        mangledMethodName),
                     proto.canThrow(), methodReturnType);
  if (classType) {
    declareVariable("this", classType, /*isParam=*/true,
                    /*isConst=*/proto.isConstMethod());
  }

  // Step 5: Declare method parameters with substituted types
  for (size_t i = 0; i < proto.getArgs().size(); ++i) {
    const auto& [argName, argType] = proto.getArgs()[i];
    declareVariable(argName, substitutedParamTypes[i], /*isParam=*/true);
  }

  // Step 5.5: Clear old resolved types before re-analysis
  // This is critical for shared generic ASTs that may have resolvedType set
  // from a previous specialization (e.g., Map<i64,i64> types on Map<i64,i32>)
  clearResolvedTypes(const_cast<BlockExprAST&>(methodFunc.getBody()));

  // Step 6: Analyze the method body
  analyzeBlock(const_cast<BlockExprAST&>(methodFunc.getBody()));

  // Step 7: Pop scopes and restore context
  exitScope();  // method scope
  if (needsTypeParamScope) {
    exitScope();  // type param scope
  }
  setCurrentClass(savedClass);
}

// -------------------------------------------------------------------
// Call expression analysis
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// Bound method references: obj.method in value position
// -------------------------------------------------------------------

void SemanticAnalyzer::maybeResolveBoundMethodRef(MemberAccessAST& memberAccess,
                                                  sun::TypePtr expectedType) {
  sun::TypePtr objectType =
      unwrapRef(memberAccess.getObject()->getResolvedType());
  if (!objectType) return;

  // Unwrap raw_ptr<Class> / static_ptr<Class> (mirrors inferType)
  if (objectType->isRawPointer()) {
    sun::TypePtr pointee =
        static_cast<sun::RawPointerType*>(objectType.get())->getPointeeType();
    if (pointee && pointee->isClass()) objectType = pointee;
  } else if (objectType->isStaticPointer()) {
    sun::TypePtr pointee =
        static_cast<sun::StaticPointerType*>(objectType.get())
            ->getPointeeType();
    if (pointee && pointee->isClass()) objectType = pointee;
  }

  const std::string& memberName = memberAccess.getMemberName();

  // Interface methods as values are not supported (would need a vtable
  // load at bind time). Only diagnose when a lambda is expected so
  // interface method calls stay untouched.
  if (objectType->isInterface() && expectedType && expectedType->isLambda()) {
    auto* ifaceType = static_cast<sun::InterfaceType*>(objectType.get());
    if (ifaceType->getMethod(memberName)) {
      logAndThrowError("Referencing interface method '" + memberName +
                           "' as a value is not supported",
                       memberAccess.getLocation());
    }
    return;
  }

  if (!objectType->isClass()) return;
  const auto* classType = static_cast<const sun::ClassType*>(objectType.get());
  if (classType->getField(memberName)) return;

  std::vector<const sun::ClassMethod*> overloads;
  for (const auto& m : classType->getMethods()) {
    if (m.name == memberName) overloads.push_back(&m);
  }
  if (overloads.empty()) return;  // not a method (inferType already errored)

  const sun::ClassMethod* chosen = nullptr;
  if (overloads.size() == 1) {
    chosen = overloads[0];
  } else if (expectedType && expectedType->isLambda()) {
    // Pick the overload matching the expected lambda signature. A
    // non-throwing method may bind where a throwing lambda is expected.
    const auto* expected =
        static_cast<const sun::LambdaType*>(expectedType.get());
    std::vector<const sun::ClassMethod*> matches;
    for (const auto* m : overloads) {
      sun::LambdaType candidate(m->returnType, m->paramTypes, m->canThrow);
      if (candidate.equalsIgnoringThrow(*expected) &&
          (expected->canThrow() || !m->canThrow)) {
        matches.push_back(m);
      }
    }
    if (matches.size() == 1) chosen = matches[0];
  }

  if (!chosen) {
    logAndThrowError("Cannot reference overloaded method '" + memberName +
                         "' as a value; add a type annotation or call it with "
                         "arguments",
                     memberAccess.getLocation());
    return;
  }

  if (chosen->isGeneric()) {
    logAndThrowError(
        "Cannot use generic method '" + memberName + "' as a value",
        memberAccess.getLocation());
    return;
  }

  // The bound method will run on this receiver later, so the receiver must
  // allow it now
  checkMethodReceiver(*memberAccess.getObject(), memberName, chosen->isConst,
                      chosen->isConstructor, memberAccess.getLocation());

  memberAccess.setResolvedType(sun::Types::Lambda(
      chosen->returnType, chosen->paramTypes, chosen->canThrow));
  memberAccess.setIsBoundMethodRef(true);
}

void SemanticAnalyzer::analyzeCall(CallExprAST& callExpr,
                                   sun::TypePtr expectedType) {
  // Set when the callee swallows a variadic pack, whose arguments are not
  // part of the recorded parameter list — the arity check below sits out.
  bool calleeTakesPack = false;
  // Set when a method is called on a constant receiver (see
  // checkMethodReceiver): a `ref T` result becomes `const ref T`
  bool receiverImmutable = false;
  // Check for unsafe intrinsic calls (non-generic)
  // Generic intrinsics (_load<T>, _store<T>, _address_of<T>) are checked in
  // type_inference.cpp
  static const std::unordered_set<std::string> unsafeIntrinsics = {
      "_malloc",
      "_free",
      "_load_i64",
      "_store_i64",
      "_atomic_cmpxchg_i32",
      "_atomic_store_i32",
      "_atomic_load_i32",
      "_atomic_fetch_add_i32",
      "_atomic_fetch_sub_i32",
      "_futex_wait",
      "_futex_wake",
      "__file_open",
      "__file_close",
      "__file_write",
      "__file_read",
      "__lseek",
      "__fstat",
      "__fsync",
      "__ftruncate",
      "__unlink",
      "__rename",
      "__mkdir",
      "__rmdir",
      "__write",
      "__read"};

  auto calleeASTType = callExpr.getCallee()->getType();
  if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*callExpr.getCallee());
    const std::string& funcName = varRef.getName();
    if (unsafeIntrinsics.count(funcName) && !isInUnsafeBlock()) {
      logAndThrowError("'" + funcName + "' can only be used in an unsafe block",
                       callExpr.getLocation());
    }
  }

  // Enum variant construction: EnumName.Variant(args...) for concrete and
  // generic enums; intercepted before generic callee analysis (see enums.cpp)
  if (tryAnalyzeEnumConstruction(callExpr, expectedType)) {
    return;
  }

  // Get parameter types early for array literal type propagation
  std::vector<sun::TypePtr> expectedParamTypes;
  if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*callExpr.getCallee());
    // Resolve the name through using imports
    sun::QualifiedName resolved = resolveNameWithUsings(varRef.getName());
    // Try to look up function parameters
    auto allFuncs = getAllFunctions(resolved.baseName);
    if (!allFuncs.empty()) {
      // Use first overload's param types for type propagation
      expectedParamTypes = allFuncs[0].paramTypes;
    } else {
      // Check if this is a class constructor (use base name for lookup)
      auto classType = lookupClass(resolved.baseName);
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
  // resolution. Member-access args get the expected param type so an
  // overloaded bound method reference can be disambiguated (kept narrow to
  // avoid changing literal coercion or free-function overload resolution).
  for (size_t i = 0; i < callExpr.getArgs().size(); ++i) {
    const auto& arg = callExpr.getArgs()[i];
    sun::TypePtr expected = (arg->getType() == ASTNodeType::MEMBER_ACCESS &&
                             i < expectedParamTypes.size())
                                ? expectedParamTypes[i]
                                : nullptr;
    analyzeExpr(const_cast<ExprAST&>(*arg), expected);
  }

  // Expand any variadic pack (`f(args...)`) into concrete typed args before
  // overload resolution, so argTypes below reflects the real arguments.
  expandPackArguments(callExpr.getArgsMutable());

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

    // Store the qualified name so codegen doesn't need to do name resolution
    if (resolved.mangled() != varRef.getName()) {
      varRef.setQualifiedName(resolved);
    }

    resolvedFunc = lookupFunction(resolved.baseName, argTypes);
    if (resolvedFunc) {
      checkExternCallAllowed(*resolvedFunc, varRef.getName(),
                             callExpr.getLocation());
      // Set resolved type on the callee directly
      varRef.setResolvedType(sun::Types::Function(resolvedFunc->returnType,
                                                  resolvedFunc->paramTypes,
                                                  resolvedFunc->canThrow));
      // Set qualified name from the resolved function (handles import scopes)
      if (!resolvedFunc->qualifiedName.empty()) {
        varRef.setQualifiedName(resolvedFunc->qualifiedName);
      }
    } else {
      // Check if this is a class constructor call: ClassName(args...)
      // This creates a stack-allocated class instance
      classType = lookupClass(resolved.baseName);
      // A generic function called without type arguments — `identity(42)`.
      // The arguments say what T is, so instantiate that specialization and
      // let the call resolve to it like any other named function.
      const GenericFunctionInfo* genericFunc =
          classType ? nullptr : lookupGenericFunction(resolved.baseName);
      if (classType) {
        // Set resolved type on the callee to indicate this is a class
        // constructor call (stack-allocated)
        varRef.setResolvedType(classType);
      } else if (genericFunc) {
        auto typeArgs = sun::generics::inferGenericTypeArguments(
            *genericFunc, argTypes, varRef.getName(), callExpr.getLocation());
        if (std::any_of(typeArgs.begin(), typeArgs.end(),
                        sun::generics::mentionsTypeParameter)) {
          // In a template body the arguments are still type parameters; the
          // specialization is made when the enclosing generic is
          // instantiated. Until then the call has the substituted signature.
          varRef.setResolvedType(
              genericFunctionSignature(*genericFunc, typeArgs));
        } else {
          SpecializedFunctionInfo specialized = requireGenericSpecialization(
              *genericFunc, typeArgs, varRef.getName(), callExpr.getLocation());
          resolvedFunc = specialized.asFunctionInfo();
          varRef.setQualifiedName(specialized.qualifiedName);
          varRef.setResolvedType(specialized.functionType());
        }
      } else {
        // Check if there are overloads for this function name - if so,
        // report a helpful "no matching overload" error
        auto allOverloads = getAllFunctions(resolved.baseName);
        if (!allOverloads.empty()) {
          // Build error message with arg types and available overloads
          std::string argTypesStr;
          for (size_t i = 0; i < argTypes.size(); ++i) {
            if (i > 0) argTypesStr += ", ";
            argTypesStr +=
                argTypes[i] ? argTypes[i]->toDisplayString() : "unknown";
          }
          std::string overloadsStr;
          for (const auto& overload : allOverloads) {
            overloadsStr += "\n  - " + resolved.baseName + "(";
            for (size_t i = 0; i < overload.paramTypes.size(); ++i) {
              if (i > 0) overloadsStr += ", ";
              overloadsStr += overload.paramTypes[i]
                                  ? overload.paramTypes[i]->toDisplayString()
                                  : "unknown";
            }
            if (overload.isCVariadic) {
              overloadsStr += overload.paramTypes.empty() ? "..." : ", ...";
            }
            overloadsStr += ")";
          }
          logAndThrowError("No matching overload of '" + resolved.baseName +
                               "' for argument types (" + argTypesStr +
                               "). Available overloads:" + overloadsStr,
                           callExpr.getLocation());
        }
        // Not a function or class - analyze normally (will check variables,
        // etc.)
        analyzeExpr(varRef);
      }
    }
  } else if (calleeASTType == ASTNodeType::MEMBER_ACCESS) {
    // Handle method calls: object.method(args...)
    auto& memberAccess = static_cast<MemberAccessAST&>(
        const_cast<ExprAST&>(*callExpr.getCallee()));

    // First analyze the object expression to get its type
    analyzeExpr(const_cast<ExprAST&>(*memberAccess.getObject()));

    // Get object type (unwrap references)
    sun::TypePtr objectType = memberAccess.getObject()->getResolvedType();
    if (!objectType) {
      objectType = inferType(*memberAccess.getObject());
    }
    objectType = unwrapRef(objectType);

    // For class types, do method overload resolution with argument types
    if (objectType && objectType->isClass()) {
      const auto* classType =
          static_cast<const sun::ClassType*>(objectType.get());
      const std::string& methodName = memberAccess.getMemberName();

      // Generic method with an _init_args<T> variadic pack (e.g.
      // allocator.create<Point>(...)): specialize HERE, where the actual call
      // argument types are known, so overloaded constructors resolve and the
      // specialization is keyed (mangled) by the variadic arg types. The
      // inferType trigger defers variadic methods to this path.
      FunctionAST* genericMethod = findGenericMethodAST(classType, methodName);
      bool variadicMethod =
          genericMethod && genericMethod->getProto().hasVariadicConstraint();
      if (variadicMethod && memberAccess.hasTypeArguments()) {
        calleeTakesPack = true;
        std::vector<sun::TypePtr> typeArgPtrs;
        for (const auto& ta : memberAccess.getTypeArguments()) {
          typeArgPtrs.push_back(typeAnnotationToType(*ta));
        }
        memberAccess.setResolvedTypeArgs(typeArgPtrs);
        // create<T>(args...) has no fixed params, so all call args are
        // variadic.
        memberAccess.setResolvedVariadicArgTypes(argTypes);

        auto mutableClassType =
            std::static_pointer_cast<sun::ClassType>(objectType);
        instantiateGenericMethod(mutableClassType, methodName, typeArgPtrs,
                                 argTypes);

        const sun::ClassMethod* method = accessibleMethod(
            *classType, methodName, memberAccess.getLocation());
        if (method) {
          memberAccess.setResolvedType(
              sun::Types::Function(method->returnType, method->paramTypes));
          receiverImmutable = checkMethodReceiver(
              *memberAccess.getObject(), methodName, method->isConst,
              method->isConstructor, memberAccess.getLocation());
        }
      } else if (genericMethod && !variadicMethod) {
        // A generic method: whatever type arguments the call leaves out are
        // inferred from its arguments, then inferType() instantiates the
        // specialization from the complete list (and does the same when all
        // of them were written).
        const sun::ClassMethod* method = accessibleMethod(
            *classType, methodName, memberAccess.getLocation());
        std::vector<sun::TypePtr> written = resolveTypeArguments(
            memberAccess.getTypeArguments(), memberAccess.getLocation(),
            "generic method call");
        if (method && written.size() < method->typeParameters.size()) {
          memberAccess.setResolvedTypeArgs(
              sun::generics::inferMethodTypeArguments(
                  *method, argTypes,
                  classType->getDisplayName() + "." + methodName,
                  memberAccess.getLocation(), written));
        }
        memberAccess.setResolvedType(inferType(memberAccess));
        if (method) {
          receiverImmutable = checkMethodReceiver(
              *memberAccess.getObject(), methodName, method->isConst,
              method->isConstructor, memberAccess.getLocation());
        }
      } else {
        // Try to find a method overload matching the argument types
        const sun::ClassMethod* method = accessibleMethodForArgs(
            *classType, methodName, argTypes, memberAccess.getLocation());
        if (method) {
          // Set the resolved type on the member access for later use
          memberAccess.setResolvedType(
              sun::Types::Function(method->returnType, method->paramTypes));
          receiverImmutable = checkMethodReceiver(
              *memberAccess.getObject(), methodName, method->isConst,
              method->isConstructor, memberAccess.getLocation());
        } else {
          // No overload took these arguments. When the mismatch is the
          // argument *count*, say so here: the fallback below picks an
          // arbitrary overload, and a zero-parameter one leaves nothing for
          // the arity check to compare against (issue #87).
          reportNoMethodForArgCount(*classType, methodName, argTypes,
                                    memberAccess.getLocation());
          // Fall back to first method with this name (will error on type
          // mismatch). Set the type directly (the object is already
          // analyzed) instead of analyzeExpr, so the callee is not converted
          // to a bound-method lambda — call position requires a FunctionType.
          memberAccess.setResolvedType(inferType(memberAccess));
        }
      }
    } else if (const FunctionInfo* modFunc = resolveModuleQualifiedCall(
                   memberAccess, objectType, argTypes)) {
      // Module-qualified call: the overload is chosen from the argument
      // types here. inferType() alone would only see the first overload.
      memberAccess.setResolvedType(
          sun::Types::Function(modFunc->returnType, modFunc->paramTypes));
    } else if (auto* staticPtr = asNonClassStaticPtr(objectType)) {
      // static_ptr<T> builtin methods: length(), raw()
      memberAccess.setResolvedType(inferStaticPtrMethodType(
          *staticPtr, memberAccess.getMemberName(), callExpr.getArgs().size(),
          memberAccess.getLocation()));
    } else {
      // Not a class type (interface, module, ptr-to-class, builtin...).
      // Set the type directly (the object is already analyzed) instead of
      // analyzeExpr, so a ptr-to-class method callee is not converted to a
      // bound-method lambda — call position requires a FunctionType.
      memberAccess.setResolvedType(inferType(memberAccess));
      if (objectType && objectType->isInterface()) {
        const auto* iface =
            static_cast<const sun::InterfaceType*>(objectType.get());
        if (const auto* method =
                iface->getMethod(memberAccess.getMemberName())) {
          receiverImmutable = checkMethodReceiver(
              *memberAccess.getObject(), method->name, method->isConst,
              /*isConstructor=*/false, memberAccess.getLocation());
        }
      }
    }
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
  // A module-qualified constructor call (`m.Point(...)`) resolves the callee
  // to the class type; treat it like `Point(...)` below. Only a member of a
  // module names a class this way — a method that returns a class, such as
  // `t.join()` on a `Thread<Point>`, has the same callee type but is a call,
  // not a construction.
  if (!classType && calleeSunType && calleeSunType->isClass() &&
      callExpr.getCallee()->getType() == ASTNodeType::MEMBER_ACCESS) {
    const auto& calleeMember =
        static_cast<const MemberAccessAST&>(*callExpr.getCallee());
    sun::TypePtr ownerType = calleeMember.getObject()->getResolvedType();
    if (!ownerType) ownerType = inferType(*calleeMember.getObject());
    if (ownerType && ownerType->isModule()) {
      classType = std::static_pointer_cast<sun::ClassType>(calleeSunType);
    }
  }
  std::vector<sun::TypePtr> paramTypes;
  // Whether paramTypes came from a callee whose signature we actually know.
  // An empty parameter list is a real signature (`f()`), so emptiness alone
  // cannot stand in for "unknown" — that is what let calls to zero-parameter
  // methods and lambdas past the arity check (issue #87).
  bool knownSignature = false;

  if (resolvedFunc) {
    paramTypes = resolvedFunc->paramTypes;
    knownSignature = true;
  } else if (calleeSunType && calleeSunType->isFunction()) {
    paramTypes = static_cast<const sun::FunctionType*>(calleeSunType.get())
                     ->getParamTypes();
    knownSignature = true;
  } else if (calleeSunType && calleeSunType->isLambda()) {
    paramTypes = static_cast<const sun::LambdaType*>(calleeSunType.get())
                     ->getParamTypes();
    knownSignature = true;
  } else if (classType && classType->isClass()) {
    // Class constructor call: look up init method with overload resolution
    auto* ct = static_cast<const sun::ClassType*>(classType.get());
    const auto* initMethod =
        accessibleMethodForArgs(*ct, "init", argTypes, callExpr.getLocation());
    if (initMethod) {
      paramTypes = initMethod->paramTypes;
      knownSignature = true;
    } else if (!ct->getMethod("init") && !args.empty()) {
      // No init at all, but arguments were supplied. Field-wise construction
      // is spelled with a struct literal, where each field is named: relying
      // on declaration order would silently change meaning if two same-typed
      // fields were ever reordered.
      logAndThrowError(
          "Class '" + ct->toDisplayString() +
              "' declares no 'init', so it cannot be constructed positionally."
              " Use a struct literal naming each field: `var x: " +
              ct->toDisplayString() + " = { ... };`",
          callExpr.getLocation());
    } else if (ct->getMethod("init")) {
      // The class declares one or more init methods but none are compatible
      // with the supplied arguments.
      std::string argList;
      for (size_t i = 0; i < argTypes.size(); ++i) {
        if (i > 0) argList += ", ";
        argList += argTypes[i] ? argTypes[i]->toDisplayString() : "?";
      }
      // List what the class does declare — with overloads, "no match" alone
      // leaves the caller guessing which one they nearly hit.
      std::string candidates;
      for (const auto& method : ct->getMethods()) {
        if (method.name != "init") continue;
        std::string params;
        for (size_t i = 0; i < method.paramTypes.size(); ++i) {
          if (i > 0) params += ", ";
          params += method.paramTypes[i]
                        ? method.paramTypes[i]->toDisplayString()
                        : "?";
        }
        candidates += "\n       candidate: init(" + params + ")";
      }
      logAndThrowError("No matching constructor for '" + ct->toDisplayString() +
                           "' with arguments (" + argList + ")" + candidates,
                       callExpr.getLocation());
    }
  }

  // What to call the callee in diagnostics: a plain call gives its function
  // name, a method call its member name. Only a plain call can name an
  // intrinsic, so the intrinsic-only conversions below key off that form.
  std::string funcName = "<unknown>";
  if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
    funcName = static_cast<const VariableReferenceAST&>(*callExpr.getCallee())
                   .getName();
  } else if (calleeASTType == ASTNodeType::MEMBER_ACCESS) {
    funcName = static_cast<const MemberAccessAST&>(*callExpr.getCallee())
                   .getMemberName();
  }
  bool calleeIsIntrinsic =
      calleeASTType == ASTNodeType::VARIABLE_REFERENCE && isIntrinsic(funcName);

  // Check argument count. A C-variadic callee fixes only its leading
  // parameters, so extra trailing arguments are allowed.
  bool calleeIsCVariadic = resolvedFunc && resolvedFunc->isCVariadic;
  bool badArgCount = calleeIsCVariadic ? args.size() < paramTypes.size()
                                       : args.size() != paramTypes.size();
  if (knownSignature && !calleeTakesPack && badArgCount) {
    logAndThrowError("Function '" + funcName + "' expects " +
                         (calleeIsCVariadic ? "at least " : "") +
                         std::to_string(paramTypes.size()) +
                         " arguments, got " + std::to_string(args.size()),
                     callExpr.getLocation());
  }

  checkPackedRefArguments(args, paramTypes);
  if (knownSignature) {
    checkArgumentPlaces(args, paramTypes, funcName, callExpr.getLocation());
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

        // Reference parameter accepts the referenced type directly
        if (paramType->isReference()) {
          auto* refType =
              static_cast<const sun::ReferenceType*>(paramType.get());
          if (refType->getReferencedType()->equals(*argType)) {
            compatible = true;
          }
          // A borrow of the other mutability: only ref -> const ref
          if (argType->isReference()) {
            auto* argRef =
                static_cast<const sun::ReferenceType*>(argType.get());
            if (sun::refMutabilityConvertible(*argRef, *refType) &&
                refType->getReferencedType()->equals(
                    *argRef->getReferencedType())) {
              compatible = true;
            }
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

        // raw_ptr<T> is compatible with byte pointers raw_ptr<i8>/raw_ptr<u8>
        // (like C's void*). Only for intrinsics (functions starting with '_')
        // to avoid accidental type erasure in user code
        if (argType->isRawPointer() && paramType->isRawPointer() &&
            calleeIsIntrinsic) {
          auto* paramRawPtr =
              static_cast<const sun::RawPointerType*>(paramType.get());
          if (paramRawPtr->getPointeeType()->isInt8() ||
              paramRawPtr->getPointeeType()->isUInt8()) {
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
          // The common way to land here now: handing a borrowed element to a
          // by-value parameter. Say what to do about it.
          std::string hint;
          if (argType->isReference() && !paramType->isReference() &&
              !sun::typeCopiesByRead(paramType)) {
            hint = ". It is borrowed, and a '" + paramType->toDisplayString() +
                   "' cannot be read out of a borrow: take the parameter by "
                   "'ref', pass a clone(), or move the value out first "
                   "(take()/pop()/remove() on a container)";
          }
          logAndThrowError("Type mismatch in argument " +
                               std::to_string(i + 1) + " of call to '" +
                               funcName + "': expected " +
                               paramType->toDisplayString() + ", got " +
                               argType->toDisplayString() + hint,
                           callExpr.getLocation());
        }
      }
    }
  }

  // Check for error propagation: calling a throwing function or lambda
  // requires either being inside a try block or being in a function declared
  // with ", IError"
  bool calleeThrows = resolvedFunc && resolvedFunc->canThrow;
  if (!calleeThrows) {
    sun::TypePtr calleeType = callExpr.getCallee()->getResolvedType();
    if (calleeType && calleeType->isLambda()) {
      calleeThrows =
          static_cast<const sun::LambdaType*>(calleeType.get())->canThrow();
    }
  }
  if (calleeThrows) {
    if (!isInTryBlock() && !isInThrowingFunction()) {
      logAndThrowError(
          "Call to throwing function '" + funcName +
              "' must be in a try block or in a function declared with ', "
              "IError'",
          callExpr.getLocation());
    }
  }

  // Record how each argument reaches its parameter. Codegen carries these
  // out and never compares Sun types at the call boundary itself.
  if (knownSignature) {
    callExpr.setArgConversions(sun::conversions::classifyArguments(
        callExpr.getResolvedArgTypes(), paramTypes, calleeIsCVariadic, funcName,
        callExpr.getLocation()));
  }

  // A borrow handed out by a method seen through an immutable receiver may
  // only be read through
  sun::TypePtr resultType = inferType(callExpr);
  if (receiverImmutable) resultType = createConstView(resultType);
  callExpr.setResolvedType(resultType);
}

// -------------------------------------------------------------------
// Intrinsic call analysis (e.g., _load<T>, _store<T>, _address_of<T>)
// -------------------------------------------------------------------

void SemanticAnalyzer::expandPackArguments(
    std::vector<std::unique_ptr<ExprAST>>& args) {
  auto* fnScope = currentFunctionScope();
  if (!fnScope || !fnScope->variadicParam) return;
  const auto& [packName, types] = *fnScope->variadicParam;

  // Is there a pack expansion for this function's variadic param to expand?
  auto isPack = [&](const std::unique_ptr<ExprAST>& a) {
    return a->getType() == ASTNodeType::PACK_EXPANSION &&
           static_cast<const PackExpansionAST&>(*a).getPackName() == packName;
  };
  bool hasPack = false;
  for (const auto& a : args) {
    if (isPack(a)) {
      hasPack = true;
      break;
    }
  }
  if (!hasPack) return;

  // Rewrite `args...` into concrete, already-typed references to the elements
  // the pack was materialized as ("args.0", "args.1", ...). Other args pass
  // through unchanged.
  std::vector<std::unique_ptr<ExprAST>> rebuilt;
  rebuilt.reserve(args.size() + types.size());
  for (auto& a : args) {
    if (isPack(a)) {
      for (size_t i = 0; i < types.size(); ++i) {
        auto vref = std::make_unique<VariableReferenceAST>(packName + "." +
                                                           std::to_string(i));
        vref->setResolvedType(types[i]);
        rebuilt.push_back(std::move(vref));
      }
    } else {
      rebuilt.push_back(std::move(a));
    }
  }
  args = std::move(rebuilt);
}

void SemanticAnalyzer::analyzeIntrinsicCall(GenericCallAST& genericCall) {
  // Intrinsics are handled at codegen time - just analyze arguments
  for (const auto& arg : genericCall.getArgs()) {
    analyzeExpr(const_cast<ExprAST&>(*arg));
  }
  // Expand any variadic pack into concrete typed args (e.g. _init<T>(p,
  // args...))
  expandPackArguments(genericCall.getArgsMutable());
  genericCall.setResolvedType(inferGenericCallType(genericCall));
}

// -------------------------------------------------------------------
// Generic function call analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeGenericFunctionCall(GenericCallAST& genericCall) {
  const std::string& funcName = genericCall.getFunctionName();
  const auto& args = genericCall.getArgs();

  // Resolve the function name through using imports
  sun::QualifiedName resolved = resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  auto* genFuncInfo = lookupGenericFunction(lookupName);
  if (!genFuncInfo) {
    logAndThrowError("Unknown generic function '" + funcName + "'",
                     genericCall.getLocation());
  }

  // Store the generic function AST on the call node for codegen
  genericCall.setGenericFunctionAST(genFuncInfo->AST);

  // A call may name only the leading type parameters — `f<i32>(x)` for
  // `f<T, U>` — and leave the rest to the arguments, as a call with no type
  // arguments does. That needs the argument types first.
  bool argsAnalyzed = false;
  if (genericCall.getResolvedTypeArgs().size() <
      genFuncInfo->typeParameters.size()) {
    std::vector<sun::TypePtr> argTypes;
    for (const auto& arg : args) {
      analyzeExpr(const_cast<ExprAST&>(*arg));
      argTypes.push_back(arg->getResolvedType());
    }
    argsAnalyzed = true;
    std::vector<sun::TypePtr> given = genericCall.getResolvedTypeArgs();
    genericCall.setResolvedTypeArgs(sun::generics::inferGenericTypeArguments(
        *genFuncInfo, argTypes, funcName, genericCall.getLocation(), given));
  }
  const auto& typeArgs = genericCall.getResolvedTypeArgs();

  // Try to get expected parameter types for array literal type propagation
  // Only instantiate if all type arguments are concrete (not type parameters)
  // If we're inside a generic function and T is still a type parameter,
  // we can't create a real specialization yet - it will be created when
  // the outer generic function is instantiated with concrete types.
  std::vector<sun::TypePtr> expectedParamTypes;
  bool allConcrete = std::none_of(typeArgs.begin(), typeArgs.end(),
                                  sun::generics::mentionsTypeParameter);
  if (allConcrete) {
    SpecializedFunctionInfo specializedFunc = requireGenericSpecialization(
        *genFuncInfo, typeArgs, funcName, genericCall.getLocation());
    expectedParamTypes = specializedFunc.paramTypes;
    // Record the name so codegen calls exactly what was instantiated
    genericCall.setSpecializationName(specializedFunc.qualifiedName);
  }

  // Propagate expected types to array literal arguments before analysis
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    if (args[i]->getType() == ASTNodeType::ARRAY_LITERAL) {
      sun::TypePtr paramType = expectedParamTypes[i];
      // Handle ref array<T> -> array<T>
      if (paramType && paramType->isReference()) {
        auto* refType = static_cast<const sun::ReferenceType*>(paramType.get());
        paramType = refType->getReferencedType();
      }
      if (paramType && paramType->isArray()) {
        const_cast<ExprAST&>(*args[i]).setResolvedType(paramType);
      }
    }
  }

  // Analyze all arguments
  if (!argsAnalyzed) {
    for (const auto& arg : args) {
      analyzeExpr(const_cast<ExprAST&>(*arg));
    }
  }

  // Coerce integer literals to the instantiated parameter types (there is
  // exactly one signature, so a non-fitting literal is a hard error)
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                            expectedParamTypes[i], /*throwOnFail=*/true);
  }
  checkArgumentPlaces(args, expectedParamTypes, funcName,
                      genericCall.getLocation());

  if (allConcrete) {
    std::vector<sun::TypePtr> argTypes;
    for (const auto& arg : args) argTypes.push_back(arg->getResolvedType());
    genericCall.setArgConversions(sun::conversions::classifyArguments(
        argTypes, expectedParamTypes, /*cVariadic=*/false, funcName,
        genericCall.getLocation()));
  }

  genericCall.setResolvedType(inferGenericCallType(genericCall));
}

// -------------------------------------------------------------------
// Generic class construction analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeGenericClassConstruction(
    GenericCallAST& genericCall) {
  const std::string& funcName = genericCall.getFunctionName();
  const auto& args = genericCall.getArgs();
  const auto& typeArgs = genericCall.getResolvedTypeArgs();

  // Resolve the class name through using imports
  sun::QualifiedName resolved = resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  auto* genericClassInfo = lookupGenericClass(lookupName);
  if (!genericClassInfo) {
    logAndThrowError("Unknown generic class '" + funcName + "'",
                     genericCall.getLocation());
  }

  // Instantiate the generic class to get init method parameters
  std::vector<sun::TypePtr> expectedParamTypes;
  auto specializedClass = instantiateGenericClass(lookupName, typeArgs);
  if (specializedClass) {
    if (auto* initMethod = accessibleMethod(*specializedClass, "init",
                                            genericCall.getLocation())) {
      expectedParamTypes = initMethod->paramTypes;
    }
  }

  // Propagate expected types to array literal arguments before analysis
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    if (args[i]->getType() == ASTNodeType::ARRAY_LITERAL) {
      sun::TypePtr paramType = expectedParamTypes[i];
      // Handle ref array<T> -> array<T>
      if (paramType && paramType->isReference()) {
        auto* refType = static_cast<const sun::ReferenceType*>(paramType.get());
        paramType = refType->getReferencedType();
      }
      if (paramType && paramType->isArray()) {
        const_cast<ExprAST&>(*args[i]).setResolvedType(paramType);
      }
    }
  }

  // Analyze all arguments
  for (const auto& arg : args) {
    analyzeExpr(const_cast<ExprAST&>(*arg));
  }

  // Pick the init overload from the argument types, exactly as for a
  // non-generic class. Without this, a call with the wrong argument count
  // would silently skip the constructor in codegen.
  if (specializedClass) {
    std::vector<sun::TypePtr> argTypes;
    for (const auto& arg : args) argTypes.push_back(arg->getResolvedType());
    const auto* initMethod = accessibleMethodForArgs(
        *specializedClass, "init", argTypes, genericCall.getLocation());
    if (initMethod) {
      expectedParamTypes = initMethod->paramTypes;
    } else if (!specializedClass->getMethod("init") && !args.empty()) {
      logAndThrowError(
          "Class '" + specializedClass->toDisplayString() +
              "' declares no 'init', so it cannot be constructed positionally."
              " Use a struct literal naming each field.",
          genericCall.getLocation());
    } else if (specializedClass->getMethod("init")) {
      std::string argList;
      for (size_t i = 0; i < argTypes.size(); ++i) {
        if (i > 0) argList += ", ";
        argList += argTypes[i] ? argTypes[i]->toDisplayString() : "?";
      }
      logAndThrowError("No matching constructor for '" +
                           specializedClass->toDisplayString() +
                           "' with arguments (" + argList + ")",
                       genericCall.getLocation());
    }
  }

  // Coerce integer literals to the init method's parameter types
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                            expectedParamTypes[i], /*throwOnFail=*/true);
  }

  if (specializedClass) {
    checkArgumentPlaces(args, expectedParamTypes,
                        specializedClass->toDisplayString() + ".init",
                        genericCall.getLocation());
    std::vector<sun::TypePtr> argTypes;
    for (const auto& arg : args) argTypes.push_back(arg->getResolvedType());
    genericCall.setArgConversions(sun::conversions::classifyArguments(
        argTypes, expectedParamTypes, /*cVariadic=*/false,
        specializedClass->toDisplayString() + ".init",
        genericCall.getLocation()));
  }

  genericCall.setResolvedType(inferGenericCallType(genericCall));
}

// -------------------------------------------------------------------
// Payload enums
