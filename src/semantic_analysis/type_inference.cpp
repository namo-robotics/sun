// semantic_analysis/type_inference.cpp — Type inference

#include <unordered_set>

#include "error.h"
#include "intrinsics.h"
#include "semantic_analyzer.h"

using sun::unwrapRef;

// -------------------------------------------------------------------
// Type inference
// -------------------------------------------------------------------

sun::TypePtr SemanticAnalyzer::inferType(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::NUMBER: {
      const auto& num = static_cast<const NumberExprAST&>(expr);
      if (num.isInteger()) {
        int64_t val = num.getIntVal();
        // Default to i32 for small integers, i64 for larger ones
        // The actual type may be refined by assignment context
        if (val >= INT32_MIN && val <= INT32_MAX) {
          return sun::Types::Int32();
        } else {
          return sun::Types::Int64();
        }
      }
      // Floating point literal
      return sun::Types::Float64();
    }

    case ASTNodeType::STRING_LITERAL: {
      return sun::Types::String();
    }

    case ASTNodeType::NULL_LITERAL: {
      return sun::Types::NullPointer();
    }

    case ASTNodeType::BOOL_LITERAL: {
      return sun::Types::Bool();
    }

    case ASTNodeType::ARRAY_LITERAL: {
      const auto& arrLit = static_cast<const ArrayLiteralAST&>(expr);
      if (arrLit.getElements().empty()) {
        logAndThrowError("Cannot infer type of empty array literal",
                         arrLit.getLocation());
        return nullptr;
      }

      // Check if there's an expected type set (for type propagation from
      // function parameters)
      sun::TypePtr expectedElemType = nullptr;
      if (arrLit.getResolvedType() && arrLit.getResolvedType()->isArray()) {
        auto* expectedArray =
            static_cast<sun::ArrayType*>(arrLit.getResolvedType().get());
        expectedElemType = expectedArray->getElementType();
      }

      // Infer element type from first element
      sun::TypePtr elemType = inferType(*arrLit.getElements()[0]);
      if (!elemType) {
        logAndThrowError("Cannot infer array element type",
                         arrLit.getLocation());
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
      sun::TypePtr baseElemType = elemType;
      if (elemType->isArray()) {
        auto* innerArr = static_cast<sun::ArrayType*>(elemType.get());
        // Append inner dimensions
        for (size_t d : innerArr->getDimensions()) {
          dims.push_back(d);
        }
        baseElemType = innerArr->getElementType();
      }
      return sun::Types::Array(baseElemType, dims);
    }

    case ASTNodeType::INDEX: {
      const auto& arrIdx = static_cast<const IndexAST&>(expr);
      sun::TypePtr targetType = inferType(*arrIdx.getTarget());

      // Unwrap reference type if indexing through a reference
      if (targetType && targetType->isReference()) {
        auto* refType =
            static_cast<const sun::ReferenceType*>(targetType.get());
        targetType = refType->getReferencedType();
      }

      // Check if target is a class with __index__ or __slice__ method
      if (targetType && targetType->isClass()) {
        auto* classType = static_cast<sun::ClassType*>(targetType.get());
        bool hasSlices = arrIdx.hasSlices();

        if (hasSlices) {
          // Look for __slice__ method
          const sun::ClassMethod* sliceMethod =
              classType->getMethod("__slice__");
          if (sliceMethod) {
            return sliceMethod->returnType;
          }
          logAndThrowError("Class " + classType->getMangledName() +
                               " does not implement __slice__ for slicing",
                           arrIdx.getLocation());
          return nullptr;
        } else {
          // Look for __index__ method
          const sun::ClassMethod* indexMethod =
              classType->getMethod("__index__");
          if (indexMethod) {
            return indexMethod->returnType;
          }
          logAndThrowError("Class " + classType->getMangledName() +
                               " does not implement __index__ for indexing",
                           arrIdx.getLocation());
          return nullptr;
        }
      }

      if (!targetType || !targetType->isArray()) {
        logAndThrowError("Cannot index non-array type", arrIdx.getLocation());
        return nullptr;
      }

      auto* arrayType = static_cast<sun::ArrayType*>(targetType.get());

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

    case ASTNodeType::VARIABLE_REFERENCE: {
      const auto& varRef = static_cast<const VariableReferenceAST&>(expr);
      const std::string& name = varRef.getName();

      // Look up as variable
      VariableInfo* info = lookupVariable(name);
      if (info) {
        // Substitute type parameters to get concrete type (e.g., T -> Box)
        sun::TypePtr originalType = substituteTypeParameters(info->type);

        // Check for type narrowing from _is<T> guards
        // getNarrowedType returns the more specific type (class over interface)
        sun::TypePtr narrowedType = getNarrowedType(name, originalType);
        if (narrowedType) {
          return narrowedType;
        }

        return originalType;
      }

      // Check if it's an enum type name (for EnumName.Variant access)
      auto enumType = lookupEnum(name);
      if (enumType) {
        return enumType;
      }

      // Resolve the name through using imports (e.g., Vec -> sun_Vec)
      auto resolved = resolveNameWithUsings(name);

      // Check if it's a named function
      auto funcs = getAllFunctions(resolved.baseName);
      if (funcs.size() == 1) {
        return sun::Types::Function(funcs[0].returnType, funcs[0].paramTypes);
      }
      if (funcs.size() > 1) {
        logAndThrowError("Cannot reference overloaded function '" + name +
                             "' as a value; call it with arguments instead",
                         varRef.getLocation());
      }

      // Check if it's a module name (for mod_x.mod_y.var access)
      if (isModuleName(name)) {
        return sun::Types::Module(getFullModulePath(name));
      }

      // Unknown variable - error in strongly typed language
      logAndThrowError("Unknown variable: '" + name + "'",
                       varRef.getLocation());
    }

    case ASTNodeType::VARIABLE_CREATION: {
      const auto& varCreate = static_cast<const VariableCreationAST&>(expr);
      if (varCreate.hasTypeAnnotation()) {
        sun::TypePtr type =
            typeAnnotationToType(*varCreate.getTypeAnnotation());
        return type;
      }
      // Infer from value expression
      return inferType(*varCreate.getValue());
    }

    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      const auto& varAssign = static_cast<const VariableAssignmentAST&>(expr);
      // Assignment returns the assigned value's type
      return inferType(*varAssign.getValue());
    }

    case ASTNodeType::REFERENCE_CREATION: {
      const auto& refCreate = static_cast<const ReferenceCreationAST&>(expr);
      // Reference type is ref(T) where T is the target's type
      sun::TypePtr targetType = inferType(*refCreate.getTarget());
      return sun::Types::Reference(targetType, refCreate.isMutable());
    }

    case ASTNodeType::BINARY: {
      const auto& binExpr = static_cast<const BinaryExprAST&>(expr);
      // Comparison operators return bool
      switch (binExpr.getOp().kind) {
        case TokenKind::LESS:
        case TokenKind::GREATER:
        case TokenKind::LESS_EQUAL:
        case TokenKind::GREATER_EQUAL:
        case TokenKind::EQUAL_EQUAL:
        case TokenKind::NOT_EQUAL:
          return sun::Types::Bool();
        default: {
          // Arithmetic operators - return LHS type
          // Unwrap references - refs behave like values
          sun::TypePtr lhsType = unwrapRef(inferType(*binExpr.getLHS()));
          return lhsType;
        }
      }
    }

    case ASTNodeType::UNARY: {
      const auto& unaryExpr = static_cast<const UnaryExprAST&>(expr);
      return inferType(*unaryExpr.getOperand());
    }

    case ASTNodeType::CALL: {
      const auto& callExpr = static_cast<const CallExprAST&>(expr);

      // For function calls, check overload resolution FIRST before inferType on
      // callee This avoids errors for overloaded functions referenced by name
      if (callExpr.getCallee()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
        const auto& varRef =
            static_cast<const VariableReferenceAST&>(*callExpr.getCallee());
        // Resolve the name through using imports
        sun::QualifiedName resolved = resolveNameWithUsings(varRef.getName());
        // Infer argument types for overload resolution
        std::vector<sun::TypePtr> argTypes;
        for (const auto& arg : callExpr.getArgs()) {
          argTypes.push_back(inferType(*arg));
        }
        auto funcInfo = lookupFunction(resolved.baseName, argTypes);
        if (funcInfo && funcInfo->returnType) {
          return funcInfo->returnType;
        }
        // Check if this is a stack-allocated class constructor call
        auto classType = lookupClass(resolved.baseName);
        if (classType) {
          // Stack-allocated class instantiation: ClassName(args...)
          return classType;
        }
      }

      // Infer the type of the callee expression
      sun::TypePtr calleeType = inferType(*callExpr.getCallee());
      if (calleeType && calleeType->getKind() == sun::Type::Kind::Function) {
        const auto* funcType =
            static_cast<const sun::FunctionType*>(calleeType.get());
        return funcType->getReturnType();
      }
      if (calleeType && calleeType->getKind() == sun::Type::Kind::Lambda) {
        const auto* lambdaType =
            static_cast<const sun::LambdaType*>(calleeType.get());
        return lambdaType->getReturnType();
      }
      // Check if callee type is a class - this is a stack-allocated constructor
      // call
      if (calleeType && calleeType->isClass()) {
        return calleeType;
      }
      // Handle builtin method calls on pointer types (e.g., raw_ptr<T>.get())
      // For these, inferType on the MemberAccess returns the result type
      // directly
      if (callExpr.getCallee()->getType() == ASTNodeType::MEMBER_ACCESS) {
        const auto& memberAccess =
            static_cast<const MemberAccessAST&>(*callExpr.getCallee());
        sun::TypePtr objectType = inferType(*memberAccess.getObject());
        const std::string& memberName = memberAccess.getMemberName();

        // raw_ptr<T>.get(), static_ptr<T>.get() return T
        if (memberName == "get") {
          if (objectType && objectType->isRawPointer()) {
            if (!isInUnsafeBlock()) {
              logAndThrowError(
                  "Dereferencing 'raw_ptr' can only be done in an unsafe block",
                  memberAccess.getLocation());
            }
            return static_cast<sun::RawPointerType*>(objectType.get())
                ->getPointeeType();
          }
          if (objectType && objectType->isStaticPointer()) {
            return static_cast<sun::StaticPointerType*>(objectType.get())
                ->getPointeeType();
          }
        }

        // Thread<T>.join() returns T
        if (memberName == "join") {
          if (objectType && objectType->isThread()) {
            return static_cast<sun::ThreadType*>(objectType.get())
                ->getResultType();
          }
        }

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

    case ASTNodeType::IF: {
      const auto& ifExpr = static_cast<const IfExprAST&>(expr);
      return sun::Types::Void();
    }

    case ASTNodeType::MATCH: {
      const auto& matchExpr = static_cast<const MatchExprAST&>(expr);
      // Return type is the type of the first arm's body
      // All arms should have compatible types (not enforced yet)
      if (!matchExpr.getArms().empty()) {
        return inferType(*matchExpr.getArms()[0].body);
      }
      return sun::Types::Void();
    }

    case ASTNodeType::FOR_LOOP: {
      // For loops always return 0.0 (f64)
      return sun::Types::Float64();
    }

    case ASTNodeType::FOR_IN_LOOP: {
      // For-in loops always return 0.0 (f64)
      return sun::Types::Float64();
    }

    case ASTNodeType::WHILE_LOOP: {
      // While loops always return 0.0 (f64)
      return sun::Types::Float64();
    }

    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT: {
      // Break and continue don't return a meaningful value
      return sun::Types::Void();
    }

    case ASTNodeType::BLOCK: {
      const auto& block = static_cast<const BlockExprAST&>(expr);
      if (block.isEmpty()) {
        return sun::Types::Void();
      }
      // Look for return statements in the block to determine return type
      sun::TypePtr returnType = nullptr;
      for (const auto& stmt : block.getBody()) {
        if (stmt->isReturn()) {
          const auto& retExpr = static_cast<const ReturnExprAST&>(*stmt);
          if (retExpr.hasValue()) {
            returnType = inferType(*retExpr.getValue());
          } else {
            returnType = sun::Types::Void();
          }
          break;  // Use first return statement for type inference
        }
      }
      // If no return found, block returns void
      return returnType ? returnType : sun::Types::Void();
    }

    case ASTNodeType::FUNCTION: {
      // Function definitions return a function type (for first-class functions)
      const auto& func = static_cast<const FunctionAST&>(expr);
      const auto& proto = func.getProto();

      // Build parameter types
      std::vector<sun::TypePtr> paramTypes;
      for (const auto& [argName, argType] : proto.getArgs()) {
        paramTypes.push_back(typeAnnotationToType(argType));
      }

      // Get return type
      sun::TypePtr returnType;
      if (proto.hasReturnType()) {
        returnType = typeAnnotationToType(*proto.getReturnType());
      } else if (func.hasBody()) {
        // Infer from body (only for non-extern functions)
        returnType = inferType(func.getBody());
      } else {
        // Extern function without return type - default to void
        returnType = sun::Types::Void();
      }

      // Named functions always get FunctionType (direct call)
      return sun::Types::Function(returnType, std::move(paramTypes));
    }

    case ASTNodeType::LAMBDA: {
      // Lambda expressions return a lambda type (fat pointer closure)
      const auto& lambda = static_cast<const LambdaAST&>(expr);
      const auto& proto = lambda.getProto();

      // Build parameter types
      std::vector<sun::TypePtr> paramTypes;
      for (const auto& [argName, argType] : proto.getArgs()) {
        paramTypes.push_back(typeAnnotationToType(argType));
      }

      // Get return type
      sun::TypePtr returnType;
      if (proto.hasReturnType()) {
        returnType = typeAnnotationToType(*proto.getReturnType());
      } else if (lambda.hasBody()) {
        returnType = inferType(lambda.getBody());
      } else {
        returnType = sun::Types::Void();
      }

      // Lambdas always get LambdaType (fat pointer closure)
      return sun::Types::Lambda(returnType, std::move(paramTypes));
    }

    case ASTNodeType::INDEXED_ASSIGNMENT: {
      const auto& assignment = static_cast<const IndexedAssignmentAST&>(expr);
      // Type of indexed assignment is the type of the value being assigned
      return inferType(*assignment.getValue());
    }

    case ASTNodeType::RETURN: {
      const auto& returnExpr = static_cast<const ReturnExprAST&>(expr);
      if (returnExpr.hasValue()) {
        return inferType(*returnExpr.getValue());
      }
      return sun::Types::Void();
    }

    case ASTNodeType::QUALIFIED_NAME: {
      const auto& qualName = static_cast<const QualifiedNameAST&>(expr);
      std::string fullName = qualName.getFullName();

      // Look up in namespaced variables
      VariableInfo* varInfo = lookupQualifiedVariable(fullName);
      if (varInfo) {
        return varInfo->type;
      }

      // Look up in namespaced functions
      const FunctionInfo* funcInfo = lookupQualifiedFunction(fullName);
      if (funcInfo) {
        return sun::Types::Function(funcInfo->returnType, funcInfo->paramTypes);
      }

      // Unknown qualified name - error in strongly typed language
      logAndThrowError(
          "Unknown qualified name: '" + qualName.getFullName() + "'",
          qualName.getLocation());
    }

    case ASTNodeType::MODULE:
    case ASTNodeType::USING:
    case ASTNodeType::IMPORT:
    case ASTNodeType::DECLARE_TYPE:
      return sun::Types::Void();

    case ASTNodeType::CLASS_DEFINITION: {
      // Class definitions themselves don't return a value
      return sun::Types::Void();
    }

    case ASTNodeType::INTERFACE_DEFINITION: {
      // Interface definitions themselves don't return a value
      return sun::Types::Void();
    }

    case ASTNodeType::THIS: {
      // 'this' returns the current class type
      if (currentClass) {
        return currentClass;
      }
      // Error: 'this' used outside of class method
      return sun::Types::Void();
    }

    case ASTNodeType::MEMBER_ACCESS:
      return inferType(static_cast<const MemberAccessAST&>(expr));

    case ASTNodeType::MEMBER_ASSIGNMENT: {
      const auto& memberAssign = static_cast<const MemberAssignmentAST&>(expr);
      // Analyze both sides for side effects and type checking
      inferType(*memberAssign.getObject());
      inferType(*memberAssign.getValue());
      // Assignment expression returns void
      return sun::Types::Void();
    }

    case ASTNodeType::TRY_CATCH: {
      const auto& tryCatchExpr = static_cast<const TryCatchExprAST&>(expr);
      // The type of a try-catch is the type of the try block
      // (catch block should return the same type or be noreturn)
      return inferType(tryCatchExpr.getTryBlock());
    }

    case ASTNodeType::UNSAFE_BLOCK: {
      const auto& unsafeBlock = static_cast<const UnsafeBlockAST&>(expr);
      const auto& body = unsafeBlock.getBody();

      // Set unsafe context for type inference of the body
      const_cast<SemanticAnalyzer*>(this)->enterUnsafeBlock();

      sun::TypePtr resultType;
      // For unsafe blocks, the type is the type of the last expression
      // This allows patterns like: return unsafe { _load<T>(ptr, idx); };
      if (body.getType() == ASTNodeType::BLOCK) {
        const auto& block = static_cast<const BlockExprAST&>(body);
        if (!block.isEmpty()) {
          // Return type of last statement (expression statement)
          const auto& lastStmt = *block.getBody().back();
          resultType = inferType(lastStmt);
        }
      }
      if (!resultType) {
        resultType = inferType(body);
      }

      const_cast<SemanticAnalyzer*>(this)->exitUnsafeBlock();
      return resultType;
    }

    case ASTNodeType::THROW: {
      // Throw expressions don't return a value (they transfer control)
      // We return Void but in practice this is a noreturn
      return sun::Types::Void();
    }

    case ASTNodeType::SPAWN: {
      // spawn(lambda) returns Thread<T> where T is the lambda's return type
      const auto& spawnExpr = static_cast<const SpawnExprAST&>(expr);
      sun::TypePtr lambdaType = inferType(spawnExpr.getLambda());
      if (lambdaType && lambdaType->isLambda()) {
        auto* lambda = static_cast<sun::LambdaType*>(lambdaType.get());
        return std::make_shared<sun::ThreadType>(lambda->getReturnType());
      }
      logAndThrowError("spawn requires a lambda expression, got '" +
                           (lambdaType ? lambdaType->toString() : "unknown") +
                           "'",
                       spawnExpr.getLocation());
    }

    case ASTNodeType::GENERIC_CALL: {
      const auto& genericCall = static_cast<const GenericCallAST&>(expr);
      sun::TypePtr type = inferGenericCallType(genericCall);
      return type;
    }

    case ASTNodeType::PACK_EXPANSION: {
      // Pack expansion (args...) - represents multiple values at compile time
      // Type checking is deferred to codegen where we know the actual types
      // For now, just return void as a placeholder
      return sun::Types::Void();
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

sun::TypePtr SemanticAnalyzer::inferType(const MemberAccessAST& memberAccess) {
  const std::string& memberName = memberAccess.getMemberName();

  // Get the fully-resolved object type:
  // 1. Use resolved type if available, otherwise infer
  // 2. Unwrap references
  // 3. Check for type narrowing from _is<T> guards
  // 4. Unwrap raw_ptr<Class> to Class for member access
  sun::TypePtr objectType = memberAccess.getObject()->getResolvedType();
  if (!objectType) {
    objectType = inferType(*memberAccess.getObject());
  }
  objectType = unwrapRef(objectType);

  if (!objectType) {
    logAndThrowError(
        "Cannot access member '" + memberName + "' on unknown type",
        memberAccess.getLocation());
  }

  // Unwrap raw_ptr<Class> to Class for member access (requires unsafe)
  if (objectType->isRawPointer()) {
    sun::TypePtr pointeeType =
        static_cast<sun::RawPointerType*>(objectType.get())->getPointeeType();
    if (pointeeType && pointeeType->isClass()) {
      if (!isInUnsafeBlock()) {
        logAndThrowError(
            "Dereferencing 'raw_ptr' can only be done in an unsafe block",
            memberAccess.getLocation());
      }
      objectType = pointeeType;
    }
  }

  // Now dispatch based on the resolved object type
  switch (objectType->getKind()) {
    case sun::Type::Kind::Module: {
      // Module member access: mod_x.mod_y or mod_x.varName
      auto* moduleType = static_cast<sun::ModuleType*>(objectType.get());
      std::string modPath = moduleType->getModulePath();

      // Check if it's a nested module first
      std::string nestedModPath = modPath + "." + memberName;
      if (lookupModuleScope(nestedModPath)) {
        return sun::Types::Module(nestedModPath);
      }

      // Use unified symbol lookup to find the member in this module
      SymbolMatch match = findSymbolInModule(modPath, memberName);
      if (match) {
        // Set the resolved qualified name on the AST for codegen
        // e.g., "$d9b854ae$_sun_make_heap_allocator" for
        // sun.make_heap_allocator
        std::string resolvedName =
            mangleModulePath(match.modulePath) + "_" + memberName;
        memberAccess.setResolvedQualifiedName(resolvedName);

        switch (match.kind) {
          case SymbolKind::Class:
            return match.classType;
          case SymbolKind::GenericClass: {
            // If type arguments are provided, instantiate the generic class
            if (memberAccess.hasTypeArguments() && match.genericClassInfo) {
              auto typeArgs = resolveTypeArguments(
                  memberAccess.getTypeArguments(), memberAccess.getLocation(),
                  "generic class instantiation");
              // Store resolved type args on the AST for codegen
              memberAccess.setResolvedTypeArgs(typeArgs);
              // Instantiate the generic class with module-qualified name
              // (modPath.memberName so lookupGenericClass can find it)
              std::string qualifiedName = modPath + "." + memberName;
              auto specializedClass =
                  instantiateGenericClass(qualifiedName, typeArgs);
              if (specializedClass) {
                return specializedClass;
              }
              logAndThrowError(
                  "Failed to instantiate generic class '" + memberName + "'",
                  memberAccess.getLocation());
            }
            // No type arguments - return void as placeholder
            return sun::Types::Void();
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
                  instantiateGenericInterface(qualifiedName, typeArgs);
              if (specializedInterface) {
                return specializedInterface;
              }
              logAndThrowError("Failed to instantiate generic interface '" +
                                   memberName + "'",
                               memberAccess.getLocation());
            }
            // No type arguments - return void as placeholder
            return sun::Types::Void();
          }
          case SymbolKind::Enum:
            return match.enumType;
          case SymbolKind::Function:
            return sun::Types::Function(match.functionInfo->returnType,
                                        match.functionInfo->paramTypes);
          case SymbolKind::Variable:
            return match.variableInfo->type;
          default:
            break;
        }
      }

      logAndThrowError(
          "Unknown member '" + memberName + "' in module '" + modPath + "'",
          memberAccess.getLocation());
    }

    case sun::Type::Kind::Enum: {
      auto* enumType = static_cast<sun::EnumType*>(objectType.get());
      const auto* variant = enumType->getVariant(memberName);
      if (variant) {
        return objectType;  // Enum variant has the enum type
      }
      logAndThrowError("Unknown variant '" + memberName + "' in enum '" +
                           enumType->getName() + "'",
                       memberAccess.getLocation());
    }

    case sun::Type::Kind::Array: {
      auto* arrayType = static_cast<sun::ArrayType*>(objectType.get());
      if (memberName == "shape") {
        if (arrayType->isUnsized()) {
          return sun::Types::Array(sun::Types::Int64(), {});
        }
        size_t ndims = arrayType->getDimensions().size();
        return sun::Types::Array(sun::Types::Int64(), {ndims});
      }
      logAndThrowError(
          "Array has no member '" + memberName + "'; available: 'shape'",
          memberAccess.getLocation());
    }

    case sun::Type::Kind::StaticPointer: {
      auto* staticPtrType =
          static_cast<sun::StaticPointerType*>(objectType.get());
      if (memberName == "length") {
        return sun::Types::Int64();
      }
      if (memberName == "data") {
        return sun::Types::RawPointer(staticPtrType->getPointeeType());
      }
      if (memberName == "_get") {
        return staticPtrType->getPointeeType();
      }
      logAndThrowError("static_ptr has no member '" + memberName +
                           "'; available: 'length', 'data', '_get'",
                       memberAccess.getLocation());
    }

    case sun::Type::Kind::RawPointer: {
      // raw_ptr<T> where T is not a class (class case handled above)
      auto* ptrType = static_cast<sun::RawPointerType*>(objectType.get());
      if (memberName == "_get") {
        if (!isInUnsafeBlock()) {
          logAndThrowError(
              "Dereferencing 'raw_ptr' can only be done in an unsafe block",
              memberAccess.getLocation());
        }
        return ptrType->getPointeeType();
      }
      logAndThrowError(
          "raw_ptr has no member '" + memberName + "'; available: '_get'",
          memberAccess.getLocation());
    }

    case sun::Type::Kind::Class: {
      const auto* classType =
          static_cast<const sun::ClassType*>(objectType.get());

      // Check for field
      const sun::ClassField* field = classType->getField(memberName);
      if (field) {
        return field->type;
      }

      // Check for method
      const sun::ClassMethod* method = classType->getMethod(memberName);
      if (method) {
        sun::TypePtr returnType = method->returnType;

        // Handle generic method calls with type arguments
        if (memberAccess.hasTypeArguments() && method->isGeneric()) {
          const auto& typeArgs = memberAccess.getTypeArguments();
          const auto& typeParams = method->typeParameters;

          std::vector<sun::TypePtr> typeArgPtrs;
          for (const auto& typeArg : typeArgs) {
            auto argType = typeAnnotationToType(*typeArg);
            if (argType) {
              typeArgPtrs.push_back(argType);
            }
          }

          // Store resolved type args on the AST for codegen
          memberAccess.setResolvedTypeArgs(typeArgPtrs);

          if (typeArgPtrs.size() == typeParams.size()) {
            // Instantiate the generic method - this creates and stores the
            // specialized FunctionAST on the generic method for codegen access
            auto mutableClassType =
                std::static_pointer_cast<sun::ClassType>(objectType);
            instantiateGenericMethod(mutableClassType, memberName, typeArgPtrs);

            enterTypeParamScope(typeParams, typeArgPtrs);
            returnType = substituteTypeParameters(returnType);
            std::vector<sun::TypePtr> substitutedParams;
            for (const auto& pt : method->paramTypes) {
              substitutedParams.push_back(substituteTypeParameters(pt));
            }
            exitScope();
            return sun::Types::Function(returnType, substitutedParams);
          }
        }

        return sun::Types::Function(method->returnType, method->paramTypes);
      }

      logAndThrowError("Unknown member '" + memberName + "' on class '" +
                           classType->getMangledName() + "'",
                       memberAccess.getLocation());
    }

    case sun::Type::Kind::Interface: {
      const auto* ifaceType =
          static_cast<const sun::InterfaceType*>(objectType.get());

      // Check for field
      const sun::InterfaceField* field = ifaceType->getField(memberName);
      if (field) {
        return field->type;
      }

      // Check for method
      const sun::InterfaceMethod* method = ifaceType->getMethod(memberName);
      if (method) {
        return sun::Types::Function(method->returnType, method->paramTypes);
      }

      logAndThrowError("Unknown member '" + memberName + "' on interface '" +
                           ifaceType->getName() + "'",
                       memberAccess.getLocation());
    }

    case sun::Type::Kind::TypeParameter: {
      // For type parameters, check if there's a narrowed type from _is<T>
      // This allows member access validation during semantic analysis
      if (memberAccess.getObject()->getType() ==
          ASTNodeType::VARIABLE_REFERENCE) {
        const auto& varRef =
            static_cast<const VariableReferenceAST&>(*memberAccess.getObject());
        // Substitute type parameters first for proper narrowing validation
        sun::TypePtr substitutedType = substituteTypeParameters(objectType);
        sun::TypePtr narrowedType =
            getNarrowedType(varRef.getName(), substitutedType);
        if (narrowedType) {
          // Recursively dispatch with the narrowed type
          if (narrowedType->isClass()) {
            const auto* classType =
                static_cast<const sun::ClassType*>(narrowedType.get());
            const sun::ClassField* field = classType->getField(memberName);
            if (field) return field->type;
            const sun::ClassMethod* method = classType->getMethod(memberName);
            if (method)
              return sun::Types::Function(method->returnType,
                                          method->paramTypes);
            logAndThrowError("Unknown member '" + memberName + "' on class '" +
                                 classType->getMangledName() + "'",
                             memberAccess.getLocation());
          }
          if (narrowedType->isInterface()) {
            const auto* ifaceType =
                static_cast<const sun::InterfaceType*>(narrowedType.get());
            const sun::InterfaceField* field = ifaceType->getField(memberName);
            if (field) return field->type;
            const sun::InterfaceMethod* method =
                ifaceType->getMethod(memberName);
            if (method)
              return sun::Types::Function(method->returnType,
                                          method->paramTypes);
            logAndThrowError("Unknown member '" + memberName +
                                 "' on interface '" + ifaceType->getName() +
                                 "'",
                             memberAccess.getLocation());
          }
        }
      }
      // No narrowing available - type parameter has no known members
      logAndThrowError("Cannot access member '" + memberName +
                           "' on unconstrained type parameter '" +
                           objectType->toString() + "'",
                       memberAccess.getLocation());
    }

    case sun::Type::Kind::Thread: {
      // Thread<T>.join() returns T
      auto* threadType = static_cast<sun::ThreadType*>(objectType.get());
      if (memberName == "join") {
        return threadType->getResultType();
      }
      logAndThrowError(
          "Thread has no member '" + memberName + "'; available: 'join'",
          memberAccess.getLocation());
    }

    default:
      logAndThrowError("Cannot access member '" + memberName + "' on type '" +
                           objectType->toString() + "'",
                       memberAccess.getLocation());
  }
}

sun::TypePtr SemanticAnalyzer::inferGenericCallType(
    const GenericCallAST& genericCall) {
  if (genericCall.hasResolvedType()) {
    return genericCall.getResolvedType();
  }
  const std::string& funcName = genericCall.getFunctionName();
  sun::QualifiedName resolved = resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  // Dispatch based on call type: intrinsic, generic function, or generic class
  bool isIntrinsicCall = sun::isIntrinsic(funcName);
  auto* genericClassInfo = lookupGenericClass(lookupName);
  auto* genFuncInfo = lookupGenericFunction(lookupName);

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

sun::TypePtr SemanticAnalyzer::inferIntrinsicCallType(
    const GenericCallAST& genericCall) {
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  const std::string& funcName = genericCall.getFunctionName();

  if (funcName == "_sizeof" || funcName == "_static_ptr_len") {
    return sun::Types::Int64();
  }
  if (funcName == "_load") {
    return typeArgs.empty() ? nullptr : typeArgs[0];
  }
  if (funcName == "_store" || funcName == "_init") {
    return sun::Types::Void();
  }
  if (funcName == "_static_ptr_data" || funcName == "_ptr_as_raw" ||
      funcName == "_address_of") {
    return typeArgs.empty() ? nullptr : sun::Types::RawPointer(typeArgs[0]);
  }
  if (funcName == "_to_ref") {
    return typeArgs.empty() ? nullptr : sun::Types::Reference(typeArgs[0]);
  }
  if (funcName == "_is") {
    return sun::Types::Bool();
  }

  // Unknown intrinsic - return void as fallback
  return sun::Types::Void();
}

// -------------------------------------------------------------------
// Generic function call type inference
// -------------------------------------------------------------------

sun::TypePtr SemanticAnalyzer::inferGenericFunctionCallType(
    const GenericCallAST& genericCall) {
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  const std::string& funcName = genericCall.getFunctionName();
  sun::QualifiedName resolved = resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  auto* genFuncInfo = lookupGenericFunction(lookupName);
  if (!genFuncInfo) {
    logAndThrowError("Unknown generic function: '" + funcName + "'",
                     genericCall.getLocation());
  }

  // Generate mangled name for specialization lookup
  std::string mangledName = funcName;
  for (const auto& typeArg : typeArgs) {
    mangledName += "_" + typeArg->toString();
  }

  // If specialization exists (type args were concrete), use its return type
  if (genFuncInfo->AST && genFuncInfo->AST->hasSpecialization(mangledName)) {
    const auto& funcAST = genFuncInfo->AST->getSpecialization(mangledName);
    return funcAST->getProto().getResolvedReturnType();
  }

  // No specialization - type args contain type parameters
  // Compute return type from generic function's declared return type
  if (genFuncInfo->returnType.has_value()) {
    const auto& typeParams = genFuncInfo->typeParameters;
    enterTypeParamScope(typeParams, typeArgs);
    sun::TypePtr returnType = typeAnnotationToType(*genFuncInfo->returnType);
    returnType = substituteTypeParameters(returnType);
    exitScope();
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

sun::TypePtr SemanticAnalyzer::inferGenericClassConstructionType(
    const GenericCallAST& genericCall) {
  const auto& typeArgs = genericCall.getResolvedTypeArgs();
  const std::string& funcName = genericCall.getFunctionName();
  sun::QualifiedName resolved = resolveNameWithUsings(funcName);
  const std::string& lookupName = resolved.baseName;

  auto* genericClassInfo = lookupGenericClass(lookupName);
  if (!genericClassInfo) {
    logAndThrowError("Unknown generic class: '" + funcName + "'",
                     genericCall.getLocation());
  }

  std::string genericMangledName = genericClassInfo->AST->getQualifiedName();
  std::string specializedMangledName =
      sun::Types::mangleGenericClassName(genericMangledName, typeArgs);

  auto existing = lookupClass(specializedMangledName);
  if (existing &&
      genericClassInfo->AST->hasSpecialization(specializedMangledName)) {
    return existing;
  }

  // Class not yet instantiated - instantiate it now
  auto specializedClass = instantiateGenericClass(lookupName, typeArgs);
  return specializedClass;
}