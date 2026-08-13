// analysis.cpp — Main analysis entry points for semantic analyzer

#include <set>
#include <unordered_set>

#include "config.h"
#include "error.h"
#include "intrinsics.h"
#include "semantic_analyzer.h"

using sun::unwrapRef;

// -------------------------------------------------------------------
// Main analysis entry point
// -------------------------------------------------------------------

void SemanticAnalyzer::analyze(ExprAST& expr) { analyzeExpr(expr); }

// -------------------------------------------------------------------
// Expression analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeExpr(ExprAST& expr, sun::TypePtr expectedType) {
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

      // Look up the variable's type first for expected type propagation
      VariableInfo* varInfo = lookupVariable(varAssign.getName());
      if (varInfo && varInfo->isCapture && !varInfo->isByRefCapture) {
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

      if (varInfo) {
        // Check type compatibility for interface polymorphism
        if (rhsType && expectedTargetType &&
            !isAssignableTo(rhsType, expectedTargetType)) {
          // Allow integer literal coercion as a fallback
          if (!tryCoerceIntegerLiteral(
                  const_cast<ExprAST*>(varAssign.getValue()),
                  expectedTargetType, false)) {
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

    case ASTNodeType::COMPOUND_ASSIGNMENT: {
      auto& compound = static_cast<CompoundAssignmentAST&>(expr);

      // By-value captures are immutable (mirror VARIABLE_ASSIGNMENT)
      if (compound.getTarget()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
        const auto& varRef =
            static_cast<const VariableReferenceAST&>(*compound.getTarget());
        VariableInfo* varInfo = lookupVariable(varRef.getName());
        if (varInfo && varInfo->isCapture && !varInfo->isByRefCapture) {
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
      sun::TypePtr targetType =
          sun::unwrapRef(compound.getTarget()->getResolvedType());

      // Analyze the value with the target's type as expected
      analyzeExpr(const_cast<ExprAST&>(*compound.getValue()), targetType);
      sun::TypePtr rhsType = compound.getValue()->getResolvedType();

      if (rhsType && targetType && !isAssignableTo(rhsType, targetType)) {
        // Allow integer literal coercion as a fallback
        if (!tryCoerceIntegerLiteral(const_cast<ExprAST*>(compound.getValue()),
                                     targetType, false)) {
          logAndThrowError("Cannot apply '" + compound.getOp().text +
                               "' with value of type '" + rhsType->toString() +
                               "' to target of type '" +
                               targetType->toString() + "'",
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

      // The target must be an addressable lvalue
      ASTNodeType targetKind = refCreate.getTarget()->getType();
      if (targetKind != ASTNodeType::VARIABLE_REFERENCE &&
          targetKind != ASTNodeType::MEMBER_ACCESS &&
          targetKind != ASTNodeType::INDEX) {
        logAndThrowError(
            "Reference target must be a variable, field, or array element",
            expr.getLocation());
      }
      if (targetKind == ASTNodeType::INDEX) {
        const auto& indexExpr =
            static_cast<const IndexAST&>(*refCreate.getTarget());
        auto baseType =
            sun::unwrapRef(indexExpr.getTarget()->getResolvedType());
        if (baseType && baseType->isClass()) {
          logAndThrowError(
              "Cannot create a reference to a class __index__ element - it "
              "has no storage address",
              expr.getLocation());
        }
        if (indexExpr.hasSlices()) {
          logAndThrowError("Cannot create a reference to a slice",
                           expr.getLocation());
        }
      }
      checkPackedFieldNotBorrowed(*refCreate.getTarget(), expr.getLocation());
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
        registernFunctionInCurrentScope(funcInfo.qualifiedName.baseName,
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
      // Analyze each arm, propagating expectedType to arm bodies
      for (const auto& arm : matchExpr.getArms()) {
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
                  classType->getDisplayName() + "' does not implement either",
              forInExpr.getLocation());
        }
      } else {
        logAndThrowError(
            "for-in loop requires a class type that implements IIterator<T> "
            "or IIterable<T>",
            forInExpr.getLocation());
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

      TokenKind op = unaryExpr.getOp().kind;
      auto operandType = sun::unwrapRef(inferType(*unaryExpr.getOperand()));

      // Unresolved generic operands are validated again at instantiation
      if (operandType && !operandType->isTypeParameter()) {
        const std::string name = operandType->toDisplayString();
        switch (op) {
          case TokenKind::NOT:
            if (!operandType->isBool()) {
              logAndThrowError("'not' requires a bool operand, got '" + name +
                                   "'",
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
              logAndThrowError("Unary minus requires a numeric operand, got '" +
                                   name + "'",
                               expr.getLocation());
            }
            if (operandType->isUnsigned()) {
              logAndThrowError("Cannot negate a value of unsigned type '" +
                                   name + "'",
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

    case ASTNodeType::MODULE: {
      auto& nsDecl = static_cast<ModuleAST&>(expr);
      // Enter the namespace scope
      enterModuleScope(nsDecl.getName());

      // Analyze the body of the namespace
      // Functions handle their own qualified name registration in FUNCTION case
      for (const auto& bodyExpr : nsDecl.getBody().getBody()) {
        if (bodyExpr->getType() == ASTNodeType::VARIABLE_CREATION) {
          // Variables need special handling to register in namespacedVariables
          analyzeExpr(*bodyExpr);
          auto& varCreate = static_cast<VariableCreationAST&>(*bodyExpr);
          sun::QualifiedName qualifiedName =
              makeQualifiedName(varCreate.getName());
          varCreate.setQualifiedName(qualifiedName);
          if (auto type = varCreate.getResolvedType()) {
            registerModuleVariable(varCreate.getName(), qualifiedName.mangled(),
                                   type);
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
      auto& usingDecl = static_cast<UsingAST&>(expr);
      // Check if this is "using A.B;" where A_B is actually a module name
      // In that case, treat it as "import all from A_B" (wildcard)
      std::string namespacePath = usingDecl.getNamespacePathString();
      std::string target = usingDecl.getTarget();

      if (!usingDecl.isModuleImport()) {
        // Build the dot-separated path: "A.B"
        std::string displayPath =
            namespacePath.empty() ? target : namespacePath + "." + target;
        // Check if this path refers to a module (handles nested modules)
        if (auto* modScope = lookupModuleScope(displayPath)) {
          // Target is a module, convert to wildcard import from that module
          UsingImport import(displayPath, "*");
          addUsingImport(import);
          // Also create scope-based ImportBinding
          addImportBinding(ImportBinding::wildcard(modScope));
          expr.setResolvedType(sun::Types::Void());
          break;
        }
      }

      // Normal case: import symbol or wildcard from namespace
      UsingImport import(namespacePath, target);
      addUsingImport(import);
      // Also create scope-based ImportBinding
      if (auto* modScope = lookupModuleScope(namespacePath)) {
        if (import.isWildcard) {
          addImportBinding(ImportBinding::wildcard(modScope));
        } else {
          addImportBinding(ImportBinding(target, modScope, target));
        }
      }
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

      // Register the class BEFORE processing fields to allow self-referential
      // types (e.g., var next: raw_ptr<Node> inside class Node)
      registerClass(baseName, classType);

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

        checkPackedFieldType(classDef, field, fieldType);

        classType->addField(field.name, fieldType);
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

      // PASS 1: Register all methods first (so methods can call each other)
      for (const auto& methodDecl : classDef.getMethods()) {
        // Get method signature info (pure computation)
        FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);

        PrototypeAST& proto =
            const_cast<PrototypeAST&>(methodDecl.function->getProto());

        // Apply computed info to prototype
        applyFunctionInfoToProto(proto, methodInfo);

        // Add method to class type (include generic type parameters)
        classType->addMethod(proto.getName(), methodInfo.returnType,
                             methodInfo.paramTypes, methodDecl.isConstructor,
                             proto.getTypeParameters(), proto.canThrow());

        // Register the method as a function with mangled name
        std::string mangledName =
            classType->getMangledMethodName(proto.getName());

        // For methods, add 'this' as first parameter type
        std::vector<sun::TypePtr> methodParamTypes;
        methodParamTypes.push_back(classType);  // this parameter
        for (const auto& pt : methodInfo.paramTypes) {
          methodParamTypes.push_back(pt);
        }
        registernFunctionInCurrentScope(
            mangledName, {methodInfo.returnType, methodParamTypes, {}});
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
        // Get method signature info (pure computation)
        FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);
        PrototypeAST& proto =
            const_cast<PrototypeAST&>(methodDecl.function->getProto());

        // Apply computed info to prototype
        applyFunctionInfoToProto(proto, methodInfo);

        // Add method to interface type (include generic type parameters)
        interfaceType->addMethod(
            proto.getName(), methodInfo.returnType, methodInfo.paramTypes,
            methodDecl.hasDefaultImpl, proto.getTypeParameters());
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
      auto& enumDef = static_cast<EnumDefinitionAST&>(expr);

      // Forbid redefinition of enum in same module
      if (definedSymbols_.count(enumDef.getName())) {
        logAndThrowError("Redefinition of enum '" + enumDef.getName() + "'",
                         enumDef.getLocation());
      }

      // Validate enum name
      validateNotReserved(enumDef.getName(), "Enum name",
                          enumDef.getLocation());

      // Validate variant names and check for duplicates
      std::set<std::string> seenVariants;
      for (const auto& variant : enumDef.getVariants()) {
        validateNotReserved(variant.name, "Enum variant name",
                            variant.location);
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

      // Track symbol for redefinition detection
      definedSymbols_.insert(enumDef.getName());

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
      // A method in value position becomes a bound method reference with
      // lambda type (call-position callees don't route through this case).
      maybeResolveBoundMethodRef(memberAccess, expectedType);
      break;
    }

    case ASTNodeType::MEMBER_ASSIGNMENT: {
      auto& memberAssign = static_cast<MemberAssignmentAST&>(expr);
      // Analyze the object and value expressions
      analyzeExpr(const_cast<ExprAST&>(*memberAssign.getObject()));
      analyzeExpr(const_cast<ExprAST&>(*memberAssign.getValue()));

      // Get the field type for type compatibility check
      sun::TypePtr objectType = memberAssign.getObject()->getResolvedType();
      objectType = unwrapRef(objectType);

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
              logAndThrowError("Cannot assign value of type '" +
                                   rhsType->toString() + "' to field '" +
                                   memberAssign.getMemberName() +
                                   "' of type '" + fieldType->toString() + "'",
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
                  (bindingType ? bindingType->toString() : std::string("?")) +
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

        // Check if it's the IError interface itself (e.g., re-throwing caught error)
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
                  errorType->toString() + "'",
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
        logAndThrowError("spawn requires a lambda expression, got '" +
                             (lambdaType ? lambdaType->toString() : "unknown") +
                             "'",
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
                             paramType->toString() +
                             "' which cannot be passed by value. Use 'ref " +
                             paramType->toString() + "' instead.",
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
    // C externs bind to a fixed symbol: no module scope, no overload suffix.
    // This stays the Sun-side name — name resolution rewrites references
    // through it — and codegen maps it to the C symbol when `as "name"`
    // renames the import.
    qualifiedName = sun::QualifiedName({}, proto.getName());
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

      existingClass->addMethod(proto.getName(), methodInfo.returnType,
                               methodInfo.paramTypes, methodDecl.isConstructor,
                               proto.getTypeParameters(), proto.canThrow());
      std::string mangledName =
          existingClass->getMangledMethodName(proto.getName());
      std::vector<sun::TypePtr> methodParamTypes;
      methodParamTypes.push_back(existingClass);
      for (const auto& pt : methodInfo.paramTypes) {
        methodParamTypes.push_back(pt);
      }
      registernFunctionInCurrentScope(
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
                         "'" + classType->getDisplayName() +
                         "(...)' rather than a '{ field: value }' literal.",
                     literal.getLocation());
    return;
  }

  std::set<std::string> seen;
  for (auto& field : literal.getMutableFields()) {
    const sun::ClassField* classField = classType->getField(field.name);
    if (!classField) {
      logAndThrowError("Class '" + classType->getDisplayName() +
                           "' has no field '" + field.name + "'",
                       field.location);
      continue;
    }
    if (!seen.insert(field.name).second) {
      logAndThrowError("Field '" + field.name +
                           "' is initialized more than once",
                       field.location);
      continue;
    }

    analyzeExpr(*field.value, classField->type);
    sun::TypePtr valueType = field.value->getResolvedType();
    if (valueType && classField->type &&
        !isAssignableTo(valueType, classField->type)) {
      if (!tryCoerceIntegerLiteral(field.value.get(), classField->type,
                                   false)) {
        logAndThrowError("Cannot initialize field '" + field.name +
                             "' of type '" +
                             classField->type->toDisplayString() +
                             "' with a value of type '" +
                             valueType->toDisplayString() + "'",
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

const FunctionInfo* SemanticAnalyzer::resolveModuleQualifiedCall(
    const MemberAccessAST& memberAccess, const sun::TypePtr& objectType,
    const std::vector<sun::TypePtr>& argTypes) const {
  if (!objectType || !objectType->isModule()) return nullptr;

  auto* moduleType = static_cast<sun::ModuleType*>(objectType.get());
  SymbolMatch match =
      findSymbolInModule(moduleType->getModulePath(),
                         memberAccess.getMemberName(), SymbolKind::Function,
                         &argTypes);
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
    return t ? t->toString() : std::string("<unresolved>");
  };

  // What codegen can lower to a C-compatible signature:
  //  - primitives, which map 1:1
  //  - raw_ptr<T>, a bare pointer
  //  - ref T, which also lowers to a bare pointer and so *is* C's `T*`.
  //    Class layout already matches C (declaration order, natural padding),
  //    so `ref SomeClass` is exactly `struct SomeClass*`.
  //  - classes by value, via SysV eightbyte classification (see sysv_abi.h)
  // Still excluded are the types with no C spelling at all: arrays and slices
  // (fat pointers), interfaces (vtable pairs), lambdas (closures), and
  // error unions.
  auto isABISafeParam = [](const sun::TypePtr& t) {
    return t && (t->isPrimitive() || t->isRawPointer() || t->isReference() ||
                 t->isClass() || t->isEnum());
  };
  // Returns allow the same, minus `ref`: Sun's ref return has auto-deref
  // semantics that do not correspond to anything C returns. Use raw_ptr<T>
  // for a returned pointer.
  auto isABISafeReturn = [](const sun::TypePtr& t) {
    return t && (t->isPrimitive() || t->isRawPointer() || t->isClass() ||
                 t->isEnum());
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
    logAndThrowError("C varargs ('...') are only allowed on 'extern function' "
                     "declarations; '" +
                         proto.getName() + "' has a body",
                     func.getLocation());
  }

  // Compute function signature from qualified name and resolved param types
  // This signature is used to create unique names for nested functions
  std::string funcSig = getFunctionSignature(proto.getMangledName(),
                                             proto.getResolvedParamTypes());

  // Enter function scope with signature for nested function qualification
  // Pass canThrow flag so throw expressions can be validated
  enterFunctionScope(funcSig, proto.getQualifiedName(), proto.canThrow());

  // Declare 'this' for methods (when we're inside a class context)
  if (currentClass) {
    declareVariable("this", currentClass, /*isParam=*/true);
  }

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
    declareVariable(argName, paramType, /*isParam=*/true);
  }

  // Add captured variables to scope (so nested functions can see them),
  // marked as captures so mutation checks and nested capture lists can
  // distinguish them from ordinary locals
  for (const auto& cap : proto.getCaptures()) {
    declareVariable(cap.name, cap.type);
    if (VariableInfo* vi = lookupVariable(cap.name)) {
      vi->isCapture = true;
      vi->isByRefCapture = cap.byRef;
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
  enterFunctionScope("", sun::QualifiedName(), proto.canThrow());

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
      vi->isCapture = true;
      vi->isByRefCapture = cap.byRef;
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
// Method analysis with type bindings
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeMethodWithBindings(
    FunctionAST& methodFunc, std::shared_ptr<sun::ClassType> classType,
    const std::vector<std::string>& typeParams,
    const std::vector<sun::TypePtr>& typeArgs) {
  // Extract module path from class context for type resolution.
  // For specialized generic classes (e.g., "$hash$_sun_Matrix_i64"), look up
  // the generic class definition's qualified name to get the module path
  // (e.g., "$hash$.sun"). This ensures types like HeapAllocator resolve to
  // their full qualified form ($hash$_sun_HeapAllocator) in method bodies.
  std::string modulePrefix;
  int moduleScopesEntered = 0;
  if (classType) {
    std::string modulePath;

    // For specialized classes, look up the generic class to get module path
    if (classType->isSpecialized()) {
      const std::string& baseGenericName = classType->getBaseGenericName();
      // The base generic name may be qualified (e.g., "$hash$_sun_Vec").
      // Generic classes are keyed by base name ("Vec"), so extract it.
      std::string lookupName = baseGenericName;
      // Strip module prefix: find last underscore that precedes a letter
      // e.g., "$hash$_sun_Vec" -> "Vec"
      for (size_t i = lookupName.size(); i > 0; --i) {
        if (lookupName[i - 1] == '_' && i < lookupName.size()) {
          std::string candidate = lookupName.substr(i);
          if (lookupGenericClass(candidate)) {
            lookupName = candidate;
            break;
          }
        }
      }
      auto* genericInfo = lookupGenericClass(lookupName);
      if (genericInfo && genericInfo->AST) {
        modulePath = genericInfo->AST->getQualifiedName().scopePathString();
      }
    }

    // For non-specialized classes, use the first underscore-separated segment
    // as a fallback (works for "sun_HeapAllocator" -> "sun")
    if (modulePath.empty()) {
      const std::string& className = classType->getMangledName();
      size_t underscorePos = className.find('_');
      if (underscorePos != std::string::npos) {
        modulePrefix = className.substr(0, underscorePos);
      }
    } else {
      modulePrefix = modulePath;
    }

    // Make module symbols visible for method body analysis.
    // We add a using import rather than entering module scopes, because
    // entering module scopes from the current context (which may be inside
    // an instantiation's class scope) would create empty shadow scopes
    // instead of finding the existing module scope with registered types.
    if (!modulePrefix.empty()) {
      addUsingImport(UsingImport(modulePrefix, "*"));
    }
  }

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
  enterFunctionScope(
      methodSig,
      sun::QualifiedName(std::vector<std::string>{}, mangledMethodName),
      proto.canThrow());
  if (classType) {
    declareVariable("this", classType, /*isParam=*/true);
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
  for (int i = 0; i < moduleScopesEntered; ++i) {
    exitScope();  // module scope(s)
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

  memberAccess.setResolvedType(sun::Types::Lambda(
      chosen->returnType, chosen->paramTypes, chosen->canThrow));
  memberAccess.setIsBoundMethodRef(true);
}

void SemanticAnalyzer::analyzeCall(CallExprAST& callExpr) {
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
      if (classType) {
        // Set resolved type on the callee to indicate this is a class
        // constructor call (stack-allocated)
        varRef.setResolvedType(classType);
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
      FunctionAST* genericMethod =
          memberAccess.hasTypeArguments()
              ? findGenericMethodAST(classType, methodName)
              : nullptr;
      if (genericMethod && genericMethod->getProto().hasVariadicConstraint()) {
        std::vector<sun::TypePtr> typeArgPtrs;
        for (const auto& ta : memberAccess.getTypeArguments()) {
          typeArgPtrs.push_back(typeAnnotationToType(*ta));
        }
        memberAccess.setResolvedTypeArgs(typeArgPtrs);
        // create<T>(args...) has no fixed params, so all call args are variadic.
        memberAccess.setResolvedVariadicArgTypes(argTypes);

        auto mutableClassType =
            std::static_pointer_cast<sun::ClassType>(objectType);
        instantiateGenericMethod(mutableClassType, methodName, typeArgPtrs,
                                 argTypes);

        const sun::ClassMethod* method = classType->getMethod(methodName);
        if (method) {
          memberAccess.setResolvedType(
              sun::Types::Function(method->returnType, method->paramTypes));
        }
      } else {
        // Try to find a method overload matching the argument types
        const sun::ClassMethod* method =
            classType->getMethodForArgs(methodName, argTypes);
        if (method) {
          // Set the resolved type on the member access for later use
          memberAccess.setResolvedType(
              sun::Types::Function(method->returnType, method->paramTypes));
        } else {
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
    } else {
      // Not a class type (interface, module, ptr-to-class, builtin...).
      // Set the type directly (the object is already analyzed) instead of
      // analyzeExpr, so a ptr-to-class method callee is not converted to a
      // bound-method lambda — call position requires a FunctionType.
      memberAccess.setResolvedType(inferType(memberAccess));
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
    // Class constructor call: look up init method with overload resolution
    auto* ct = static_cast<const sun::ClassType*>(classType.get());
    const auto* initMethod = ct->getMethodForArgs("init", argTypes);
    if (initMethod) {
      paramTypes = initMethod->paramTypes;
    } else if (!ct->getMethod("init") && !args.empty()) {
      // No init at all, but arguments were supplied. Field-wise construction
      // is spelled with a struct literal, where each field is named: relying
      // on declaration order would silently change meaning if two same-typed
      // fields were ever reordered.
      logAndThrowError(
          "Class '" + ct->toString() +
              "' declares no 'init', so it cannot be constructed positionally."
              " Use a struct literal naming each field: `var x: " +
              ct->toString() + " = { ... };`",
          callExpr.getLocation());
    } else if (ct->getMethod("init")) {
      // The class declares one or more init methods but none are compatible
      // with the supplied arguments.
      std::string argList;
      for (size_t i = 0; i < argTypes.size(); ++i) {
        if (i > 0) argList += ", ";
        argList += argTypes[i] ? argTypes[i]->toDisplayString() : "?";
      }
      logAndThrowError("No matching constructor for '" + ct->toString() +
                           "' with arguments (" + argList + ")",
                       callExpr.getLocation());
    }
  }

  // Check argument count. A C-variadic callee fixes only its leading
  // parameters, so extra trailing arguments are allowed.
  bool calleeIsCVariadic = resolvedFunc && resolvedFunc->isCVariadic;
  bool badArgCount = calleeIsCVariadic ? args.size() < paramTypes.size()
                                       : args.size() != paramTypes.size();
  if (!paramTypes.empty() && badArgCount) {
    std::string funcName = "<unknown>";
    if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
      funcName = static_cast<const VariableReferenceAST&>(*callExpr.getCallee())
                     .getName();
    }
    logAndThrowError("Function '" + funcName + "' expects " +
                         (calleeIsCVariadic ? "at least " : "") +
                         std::to_string(paramTypes.size()) +
                         " arguments, got " + std::to_string(args.size()),
                     callExpr.getLocation());
  }

  checkPackedRefArguments(args, paramTypes);

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

        // raw_ptr<T> is compatible with byte pointers raw_ptr<i8>/raw_ptr<u8>
        // (like C's void*). Only for intrinsics (functions starting with '_')
        // to avoid accidental type erasure in user code
        if (argType->isRawPointer() && paramType->isRawPointer() &&
            isIntrinsic(funcName)) {
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
          logAndThrowError("Type mismatch in argument " +
                           std::to_string(i + 1) + " of call to '" + funcName +
                           "': expected " + paramType->toString() + ", got " +
                           argType->toString());
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
      std::string funcName = "<unknown>";
      if (calleeASTType == ASTNodeType::VARIABLE_REFERENCE) {
        funcName =
            static_cast<const VariableReferenceAST&>(*callExpr.getCallee())
                .getName();
      }
      logAndThrowError(
          "Call to throwing function '" + funcName +
              "' must be in a try block or in a function declared with ', "
              "IError'",
          callExpr.getLocation());
    }
  }

  // Note: Move semantics for ptr<T> arguments are tracked by the borrow checker

  callExpr.setResolvedType(inferType(callExpr));
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
        auto vref = std::make_unique<VariableReferenceAST>(
            packName + "." + std::to_string(i));
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
  // Expand any variadic pack into concrete typed args (e.g. _init<T>(p, args...))
  expandPackArguments(genericCall.getArgsMutable());
  genericCall.setResolvedType(inferGenericCallType(genericCall));
}

// -------------------------------------------------------------------
// Generic function call analysis
// -------------------------------------------------------------------

void SemanticAnalyzer::analyzeGenericFunctionCall(GenericCallAST& genericCall) {
  const std::string& funcName = genericCall.getFunctionName();
  const auto& args = genericCall.getArgs();
  const auto& typeArgs = genericCall.getResolvedTypeArgs();

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

  // Try to get expected parameter types for array literal type propagation
  // Only instantiate if all type arguments are concrete (not type parameters)
  // If we're inside a generic function and T is still a type parameter,
  // we can't create a real specialization yet - it will be created when
  // the outer generic function is instantiated with concrete types.
  std::vector<sun::TypePtr> expectedParamTypes;
  bool allConcrete =
      std::all_of(typeArgs.begin(), typeArgs.end(),
                  [](const sun::TypePtr& t) { return !t->isTypeParameter(); });
  if (allConcrete) {
    auto specializedFunc =
        instantiateGenericFunction(genFuncInfo->AST, typeArgs);
    if (specializedFunc) {
      expectedParamTypes = specializedFunc->paramTypes;
      // genericCall.setResolvedType(specializedFunc->returnType);
    } else {
      logAndThrowError("Failed to instantiate generic function '" + funcName +
                           "' with provided type arguments",
                       genericCall.getLocation());
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

  // Coerce integer literals to the instantiated parameter types (there is
  // exactly one signature, so a non-fitting literal is a hard error)
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                            expectedParamTypes[i], /*throwOnFail=*/true);
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
    if (auto* initMethod = specializedClass->getMethod("init")) {
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

  // Coerce integer literals to the init method's parameter types
  for (size_t i = 0; i < args.size() && i < expectedParamTypes.size(); ++i) {
    tryCoerceIntegerLiteral(const_cast<ExprAST*>(args[i].get()),
                            expectedParamTypes[i], /*throwOnFail=*/true);
  }

  genericCall.setResolvedType(inferGenericCallType(genericCall));
}
