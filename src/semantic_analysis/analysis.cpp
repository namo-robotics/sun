// analysis.cpp — Main analysis entry points for semantic analyzer

#include <set>
#include <unordered_set>

#include "config.h"
#include "error.h"
#include "intrinsics.h"
#include "parser.h"
#include "semantic_analyzer.h"

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

      // Compute qualified name from current module path (includes library hash
      // for moon imports since library scope uses hash as module name)
      sun::QualifiedName qualifiedName = makeQualifiedName(proto.getName());
      proto.setQualifiedName(qualifiedName);

      // Register function BEFORE analyzing body to support recursive calls
      // For nested functions, registerFunction prepends the enclosing
      // function's signature to create unique qualified names per generic
      // instantiation
      funcInfo.qualifiedName = qualifiedName.mangled();
      funcInfo.baseName = proto.getName();
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
        registerFunction(
            proto.getName(),
            {inferredReturn, funcInfo.paramTypes, funcInfo.captures,
             qualifiedName.mangled(), proto.getName()});
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

    case ASTNodeType::IMPORT_SCOPE: {
      // Expanded import scope — analyze the body inside an import scope
      // to enforce non-transitive imports. Direct imports are visible
      // (one level of transparency), but their nested imports are not.
      auto& importScope = static_cast<ImportScopeAST&>(expr);

      // Compute scope key the same way as enterImportScope
      std::string scopeKey;
      if (!importScope.getContentHash().empty()) {
        scopeKey = importScope.getContentHash();
      } else {
        auto hash = std::hash<std::string>{}(importScope.getSourceFile());
        scopeKey = "$import_" + std::to_string(hash & 0xFFFFFFFF) + "$";
      }

      enterImportScope(importScope.getSourceFile(), scopeKey);
      importScopeDepth_++;

      // Skip entire body if this import was already fully analyzed
      // (diamond dependency). The scope was cloned by enterImportScope,
      // so all symbols are already accessible.
      if (analyzedImports_.count(scopeKey)) {
        expr.setSkipCodegen(true);
      } else {
        analyzeBlock(const_cast<BlockExprAST&>(importScope.getBody()));
        analyzedImports_.insert(scopeKey);
        // Update the canonical scope so subsequent diamond-dep clones
        // include all symbols (functions, classes, etc.) registered during
        // analysis of this import scope.
        if (currentScope->parent) {
          auto it = currentScope->parent->childModules.find(scopeKey);
          if (it != currentScope->parent->childModules.end()) {
            importScopesByKey_[scopeKey] = it->second;
          }
        }
      }

      importScopeDepth_--;
      exitScope();
      expr.setResolvedType(sun::Types::Void());
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
          std::string qualifiedName =
              qualifyNameInCurrentModule(varCreate.getName());
          varCreate.setQualifiedName(qualifiedName);
          if (auto type = varCreate.getResolvedType()) {
            registerNamespacedVariable(qualifiedName, type);
          }
        } else if (bodyExpr->getType() == ASTNodeType::REFERENCE_CREATION) {
          analyzeExpr(*bodyExpr);
          auto& refCreate = static_cast<ReferenceCreationAST&>(*bodyExpr);
          std::string qualifiedName =
              qualifyNameInCurrentModule(refCreate.getName());
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
          qualName.setResolvedMangledName(varInfo->qualifiedName);
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
          qualName.setResolvedMangledName(funcInfo->qualifiedName);
        }
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
      const std::string& baseName = classDef.getName();

      // Partial classes: add methods to the primary class.
      // If the primary has already been analyzed, merge immediately.
      // If not yet seen (partial imported before primary), stash for later.
      if (classDef.isPartial()) {
        auto existingClass = lookupClass(baseName);
        if (existingClass) {
          // Primary already analyzed — validate and merge methods now
          for (const auto& extMethod : classDef.getMethods()) {
            const std::string& methodName =
                extMethod.function->getProto().getName();
            if (existingClass->getMethod(methodName)) {
              logAndThrowError("Method '" + methodName +
                                   "' already defined in class '" + baseName +
                                   "'",
                               extMethod.function->getLocation());
            }
          }

          // Register and analyze extension methods on the existing class
          auto savedClass = currentClass;
          setCurrentClass(existingClass);

          // Enter a Class scope to contain extension method scopes
          enterScope(ScopeType::Class);
          currentScope->classBaseName = baseName;
          currentScope->classMangledName = existingClass->getMangledName();

          // Register all extension methods first
          for (const auto& methodDecl : classDef.getMethods()) {
            FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);
            const PrototypeAST& proto = methodDecl.function->getProto();
            sun::TypePtr returnType = methodInfo.returnType
                                          ? methodInfo.returnType
                                          : sun::Types::Void();
            existingClass->addMethod(
                proto.getName(), returnType, methodInfo.paramTypes,
                methodDecl.isConstructor, proto.getTypeParameters());
            std::string mangledName =
                existingClass->getMangledMethodName(proto.getName());
            std::vector<sun::TypePtr> methodParamTypes;
            methodParamTypes.push_back(existingClass);
            for (const auto& pt : methodInfo.paramTypes) {
              methodParamTypes.push_back(pt);
            }
            registerFunction(mangledName, {returnType, methodParamTypes, {}});
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
        return;
      }

      // Qualify class name with module prefix if inside a module
      // For precompiled classes (from .moon), use the qualified name from
      // metadata
      sun::QualifiedName qualifiedClass;
      if (classDef.hasQualifiedName()) {
        // Use pre-set qualified name from metadata (includes content hash
        // prefix)
        qualifiedClass = classDef.getQualifiedNameInfo();
      } else {
        qualifiedClass = makeQualifiedName(baseName);
        // Set qualified name on AST for codegen (source name stays for errors)
        classDef.setQualifiedName(qualifiedClass);
      }
      std::string mangledClassName = qualifiedClass.mangled();

      // Forbid redefinition of class in same module (depth 0)
      if (importScopeDepth_ == 0 && definedSymbols_.count(mangledClassName)) {
        logAndThrowError("Redefinition of class '" + baseName + "'",
                         classDef.getLocation());
      }

      // Validate class name
      if (isReservedIdentifier(classDef.getName())) {
        logAndThrowError("Class name '" + classDef.getName() +
                             "' is invalid: names starting with '_' are "
                             "reserved for builtins",
                         classDef.getLocation());
      }

      // Check for redefinition of builtin types
      if (typeRegistry->isBuiltinTypeName(classDef.getName())) {
        logAndThrowError(
            "Cannot redefine builtin type '" + classDef.getName() + "'",
            classDef.getLocation());
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
          logAndThrowError(
              "Method name '" + methodName +
                  "' is invalid: names starting with '_' are reserved "
                  "for builtins",
              methodDecl.function->getLocation());
        }
      }

      // Check if this is a generic class definition
      if (classDef.isGeneric()) {
        // Register as a generic class template (not instantiated yet)
        GenericClassInfo genericInfo;
        genericInfo.AST = &classDef;
        genericInfo.typeParameters = classDef.getTypeParameters();
        genericInfo.definitionScope = currentScope->shared_from_this();
        registerGenericClass(baseName, genericInfo);
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
        genericInfo.definitionScope = currentScope->shared_from_this();
        registerGenericClass(baseName, genericInfo);
      }

      // Create the class type with the qualified name
      auto classType = typeRegistry->getClass(mangledClassName);
      // Store the user-written base name for error messages
      if (mangledClassName != baseName) {
        classType->setBaseName(baseName);
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

      // Register the class so methods can reference it (use base name)
      registerClass(baseName, classType);

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
      enterScope(ScopeType::Class);
      currentScope->classBaseName = baseName;
      currentScope->classMangledName = mangledClassName;

      // PASS 1: Register all methods first (so methods can call each other)
      for (const auto& methodDecl : classDef.getMethods()) {
        // Get method signature info (converts param types)
        // Note: captures aren't needed here since methods receive 'this' as
        // parameter
        FunctionInfo methodInfo = getFunctionInfo(*methodDecl.function);

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
      }

      // PASS 2: Analyze all method bodies
      for (size_t i = 0; i < classDef.getMethods().size(); ++i) {
        const auto& methodDecl = classDef.getMethods()[i];
        // Set qualified name: modulePath for module, className for class
        // context, method name as base
        PrototypeAST& proto =
            const_cast<PrototypeAST&>(methodDecl.function->getProto());
        proto.setQualifiedName(sun::QualifiedName(
            qualifiedClass.scopeKey, mangledClassName, proto.getName()));
        analyzeFunction(*methodDecl.function);
      }

      // Validate interface implementations
      validateInterfaceImplementation(classDef, classType);

      exitScope();  // Class scope

      // Restore old class context
      setCurrentClass(savedClass);

      // Track symbol for redefinition detection
      if (importScopeDepth_ == 0) {
        definedSymbols_.insert(mangledClassName);
      }

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
        // Use pre-set qualified name from metadata (includes content hash
        // prefix)
        qualifiedInterface = interfaceDef.getQualifiedNameInfo();
      } else {
        qualifiedInterface = makeQualifiedName(interfaceDef.getName());
        // Set qualified name on AST for codegen (source name stays for errors)
        interfaceDef.setQualifiedName(qualifiedInterface);
      }
      std::string interfaceName = qualifiedInterface.mangled();

      // Forbid redefinition of interface in same module (depth 0)
      if (importScopeDepth_ == 0 && definedSymbols_.count(interfaceName)) {
        logAndThrowError(
            "Redefinition of interface '" + interfaceDef.getName() + "'",
            interfaceDef.getLocation());
      }

      // Validate interface name
      if (isReservedIdentifier(interfaceDef.getName())) {
        logAndThrowError("Interface name '" + interfaceDef.getName() +
                             "' is invalid: names starting with '_' are "
                             "reserved for builtins",
                         interfaceDef.getLocation());
      }

      // Check for redefinition of builtin types
      if (typeRegistry->isBuiltinTypeName(interfaceDef.getName())) {
        logAndThrowError("Cannot redefine builtin interface '" +
                             interfaceDef.getName() + "'",
                         interfaceDef.getLocation());
      }

      // Validate field names
      for (const auto& field : interfaceDef.getFields()) {
        if (isReservedIdentifier(field.name)) {
          logAndThrowError(
              "Interface field name '" + field.name +
                  "' is invalid: names starting with '_' are reserved "
                  "for builtins",
              field.location);
        }
      }

      // Validate method names
      for (const auto& methodDecl : interfaceDef.getMethods()) {
        const std::string& methodName =
            methodDecl.function->getProto().getName();
        if (isReservedIdentifier(methodName)) {
          logAndThrowError(
              "Interface method name '" + methodName +
                  "' is invalid: names starting with '_' are reserved "
                  "for builtins",
              methodDecl.function->getLocation());
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
      }

      // Enter Interface scope to contain method scopes
      enterScope(ScopeType::Interface);
      currentScope->classBaseName = interfaceDef.getName();
      currentScope->classMangledName = interfaceName;

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
      if (importScopeDepth_ == 0) {
        definedSymbols_.insert(interfaceName);
      }

      expr.setResolvedType(sun::Types::Void());
      break;
    }

    case ASTNodeType::ENUM_DEFINITION: {
      auto& enumDef = static_cast<EnumDefinitionAST&>(expr);

      // Forbid redefinition of enum in same module (depth 0)
      if (importScopeDepth_ == 0 && definedSymbols_.count(enumDef.getName())) {
        logAndThrowError("Redefinition of enum '" + enumDef.getName() + "'",
                         enumDef.getLocation());
      }

      // Validate enum name
      if (isReservedIdentifier(enumDef.getName())) {
        logAndThrowError("Enum name '" + enumDef.getName() +
                             "' is invalid: names starting with '_' are "
                             "reserved for builtins",
                         enumDef.getLocation());
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

      // Track symbol for redefinition detection
      if (importScopeDepth_ == 0) {
        definedSymbols_.insert(enumDef.getName());
      }

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

      // Track that we're inside a try block for error propagation checking
      enterTryBlock();

      // Analyze the try block
      analyzeBlock(const_cast<BlockExprAST&>(tryCatchExpr.getTryBlock()));

      // Exit try block tracking
      exitTryBlock();

      // Analyze the catch clause
      const auto& catchClause = tryCatchExpr.getCatchClause();

      // Create scope for the catch body
      enterScope();

      // If there's a binding, declare it in scope
      if (!catchClause.bindingName.empty()) {
        if (!catchClause.bindingType.has_value()) {
          logAndThrowError("catch binding '" + catchClause.bindingName +
                               "' requires a type annotation, e.g., catch (" +
                               catchClause.bindingName + ": IError) { ... }",
                           tryCatchExpr.getLocation());
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

        // Check if it's a class that implements IError
        if (errorType->isClass()) {
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
  // Sequential analysis — no hoisting. Types and functions must be
  // defined before use.
  for (const auto& expr : block.getBody()) {
    analyzeExpr(*expr);
  }
}

// -------------------------------------------------------------------
// Helper: resolve parameter types from prototype
// -------------------------------------------------------------------

std::vector<sun::TypePtr> SemanticAnalyzer::validateAndResolveParamTypes(
    PrototypeAST& proto, std::optional<Position> loc) {
  // Validate parameter names
  for (const auto& argName : proto.getArgNames()) {
    if (isReservedIdentifier(argName)) {
      logAndThrowError(
          "Parameter name '" + argName +
              "' is invalid: names starting with '_' are reserved for builtins",
          loc);
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
                             paramType->toString() + "' instead.",
                         loc);
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
            "' is invalid: names starting with '_' are reserved for builtins",
        func.getLocation());
  }

  // Build captures using current scope information
  std::vector<Capture> captures = buildCaptures(func);
  proto.setCaptures(captures);

  // For generic FREE functions (not methods), register in genericFunctionTable
  // so they can be looked up when called with type arguments.
  // For nested functions, include function context in the qualified name
  // to avoid collisions across different generic instantiations.
  // We do NOT skip body analysis — codegen needs resolved types on all nodes.
  if (proto.isGeneric() && !currentClass) {
    GenericFunctionInfo genInfo;
    genInfo.AST = &func;
    genInfo.typeParameters = proto.getTypeParameters();
    if (proto.hasReturnType()) {
      genInfo.returnType = *proto.getReturnType();
    }
    genInfo.params = proto.getArgs();
    // Build QualifiedName with module path and function context
    sun::QualifiedName qname(getCurrentScopeKey(), getCurrentFunctionContext(),
                             proto.getName());
    currentScope->genericFunctions[qname] = genInfo;
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
                           "' must have an explicit return type",
                       func.getLocation());
    }
    return;
  }

  // Compute function signature from qualified name and resolved param types
  // This signature is used to create unique names for nested functions
  std::string funcSig = getFunctionSignature(proto.getQualifiedName(),
                                             proto.getResolvedParamTypes());

  // Enter function scope with signature for nested function qualification
  // Pass canThrow flag so throw expressions can be validated
  enterFunctionScope(funcSig, proto.getQualifiedNameInfo(), proto.canThrow());

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
                           methodFunc.getProto().getName(),
                       methodFunc.getLocation());
    }
    // Transfer the parsed body to the existing FunctionAST
    methodFunc.setBody(std::make_unique<BlockExprAST>(
        std::move(const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
            parsedFunc->getBody().getBody()))));
  }

  // Step 1.5: Extract module path from class context for type resolution.
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
      if (genericInfo && genericInfo->AST &&
          genericInfo->AST->hasQualifiedName()) {
        modulePath = genericInfo->AST->getQualifiedNameInfo().scopeKey;
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
    enterScope(ScopeType::TypeParams);
    addTypeParameterBindings(typeParams, typeArgs);
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
  enterFunctionScope(methodSig, sun::QualifiedName("", "", mangledMethodName),
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
      "_futex_wake"};

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

    // Store the qualified name so codegen doesn't need to do name resolution
    if (resolved.mangled() != varRef.getName()) {
      varRef.setQualifiedName(resolved);
    }

    resolvedFunc = lookupFunction(resolved.baseName, argTypes);
    if (resolvedFunc) {
      // Set resolved type on the callee directly
      varRef.setResolvedType(sun::Types::Function(resolvedFunc->returnType,
                                                  resolvedFunc->paramTypes));
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
                         std::to_string(paramTypes.size()) +
                         " arguments, got " + std::to_string(args.size()),
                     callExpr.getLocation());
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
        // Only for intrinsics (functions starting with '_') to avoid
        // accidental type erasure in user code
        if (argType->isRawPointer() && paramType->isRawPointer() &&
            isIntrinsic(funcName)) {
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

  // Check for error propagation: calling a throwing function requires either
  // being inside a try block or being in a function declared with ", IError"
  if (resolvedFunc && resolvedFunc->canThrow) {
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

void SemanticAnalyzer::analyzeIntrinsicCall(GenericCallAST& genericCall) {
  // Intrinsics are handled at codegen time - just analyze arguments
  for (const auto& arg : genericCall.getArgs()) {
    analyzeExpr(const_cast<ExprAST&>(*arg));
  }
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

  genericCall.setResolvedType(inferGenericCallType(genericCall));
}
