// semantic_analysis/type_inference.cpp — Type inference

#include "error.h"
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
        logAndThrowError("Cannot infer type of empty array literal");
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
        logAndThrowError("Cannot infer array element type");
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
          logAndThrowError("Class " + classType->getName() +
                           " does not implement __slice__ for slicing");
          return nullptr;
        } else {
          // Look for __index__ method
          const sun::ClassMethod* indexMethod =
              classType->getMethod("__index__");
          if (indexMethod) {
            return indexMethod->returnType;
          }
          logAndThrowError("Class " + classType->getName() +
                           " does not implement __index__ for indexing");
          return nullptr;
        }
      }

      if (!targetType || !targetType->isArray()) {
        logAndThrowError("Cannot index non-array type");
        return nullptr;
      }

      auto* arrayType = static_cast<sun::ArrayType*>(targetType.get());

      // For unsized arrays, skip dimension count check (any number of indices
      // allowed)
      if (!arrayType->isUnsized()) {
        // Check dimension count matches for sized arrays
        if (arrIdx.getIndices().size() != arrayType->getDimensions().size()) {
          logAndThrowError("Array index count does not match dimensions");
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
      std::string resolvedName = resolveNameWithUsings(name);

      // Check if it's a named function
      auto funcs = getAllFunctions(resolvedName);
      if (funcs.size() == 1) {
        return sun::Types::Function(funcs[0].returnType, funcs[0].paramTypes);
      }
      if (funcs.size() > 1) {
        logAndThrowError("Cannot reference overloaded function '" + name +
                         "' as a value; call it with arguments instead");
      }

      // Check if it's a module name (for mod_x.mod_y.var access)
      if (isModuleName(name)) {
        return sun::Types::Module(name);
      }

      // Unknown variable - error in strongly typed language
      logAndThrowError("Unknown variable: '" + name + "'");
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
        // Resolve the name through using imports (e.g., Vec -> sun_Vec)
        std::string resolvedName = resolveNameWithUsings(varRef.getName());
        // Infer argument types for overload resolution
        std::vector<sun::TypePtr> argTypes;
        for (const auto& arg : callExpr.getArgs()) {
          argTypes.push_back(inferType(*arg));
        }
        auto funcInfo = lookupFunction(resolvedName, argTypes);
        if (funcInfo && funcInfo->returnType) {
          return funcInfo->returnType;
        }
        // Check if this is a stack-allocated class constructor call
        auto classType = lookupClass(resolvedName);
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
            return static_cast<sun::RawPointerType*>(objectType.get())
                ->getPointeeType();
          }
          if (objectType && objectType->isStaticPointer()) {
            return static_cast<sun::StaticPointerType*>(objectType.get())
                ->getPointeeType();
          }
        }

        // If calleeType is non-null but not a function, it might be the return
        // type of a builtin method (e.g., static_ptr.length returns i64)
        if (calleeType) {
          return calleeType;
        }
      }
      // Unknown function - error in strongly typed language
      logAndThrowError("Cannot infer return type for call expression");
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
      logAndThrowError("Unknown qualified name: '" + qualName.getFullName() +
                       "'");
    }

    case ASTNodeType::NAMESPACE:
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

    case ASTNodeType::THROW: {
      // Throw expressions don't return a value (they transfer control)
      // We return Void but in practice this is a noreturn
      return sun::Types::Void();
    }

    case ASTNodeType::GENERIC_CALL: {
      const auto& genericCall = static_cast<const GenericCallAST&>(expr);
      const std::string& funcName = genericCall.getFunctionName();
      const auto& typeArgsVec = genericCall.getTypeArguments();

      // Resolve the function/class name through using imports (MatrixView ->
      // sun_MatrixView)
      std::string resolvedName = resolveNameWithUsings(funcName);

      // Analyze all arguments
      for (const auto& arg : genericCall.getArgs()) {
        inferType(*arg);
      }

      // Handle compiler intrinsics (start with underscore)
      if (funcName == "_sizeof") {
        // _sizeof<T>() returns i64 - the byte size of type T
        if (!genericCall.getArgs().empty()) {
          logAndThrowError("_sizeof<T>() takes no arguments");
          return sun::Types::Void();
        }
        return sun::Types::Int64();
      }

      if (funcName == "_init") {
        // _init<T>(ptr) constructs T at ptr with forwarded arguments
        // Returns void
        if (genericCall.getArgs().empty()) {
          logAndThrowError("_init<T>() requires a pointer argument");
          return sun::Types::Void();
        }
        return sun::Types::Void();
      }

      if (funcName == "_load") {
        // _load<T>(ptr, index) loads element T at ptr[index]
        // Returns T
        if (genericCall.getArgs().size() != 2) {
          logAndThrowError("_load<T>(ptr, index) requires 2 arguments");
        }
        sun::TypePtr targetType = typeAnnotationToType(*typeArgsVec[0]);
        targetType = substituteTypeParameters(targetType);
        if (!targetType) {
          logAndThrowError("Failed to resolve type argument for _load<T>");
        }
        return targetType;
      }

      if (funcName == "_store") {
        // _store<T>(ptr, index, value) stores value at ptr[index]
        // Returns void
        if (genericCall.getArgs().size() != 3) {
          logAndThrowError("_store<T>(ptr, index, value) requires 3 arguments");
          return sun::Types::Void();
        }
        return sun::Types::Void();
      }

      if (funcName == "_static_ptr_data") {
        // _static_ptr_data<T>(static_ptr<T>) extracts data pointer as
        // raw_ptr<T>
        if (genericCall.getArgs().size() != 1) {
          logAndThrowError("_static_ptr_data<T>(ptr) requires 1 argument");
        }
        sun::TypePtr targetType = typeAnnotationToType(*typeArgsVec[0]);
        targetType = substituteTypeParameters(targetType);
        if (!targetType) {
          logAndThrowError(
              "Failed to resolve type argument for _static_ptr_data<T>");
        }
        return sun::Types::RawPointer(targetType);
      }

      if (funcName == "_static_ptr_len") {
        // _static_ptr_len<T>(static_ptr<T>) extracts length as i64
        if (genericCall.getArgs().size() != 1) {
          logAndThrowError("_static_ptr_len<T>(ptr) requires 1 argument");
        }
        return sun::Types::Int64();
      }

      if (funcName == "_ptr_as_raw") {
        // _ptr_as_raw<T>(ptr<T>) returns raw_ptr<T> without transferring
        // ownership Equivalent to unique_ptr::get() in C++
        if (genericCall.getArgs().size() != 1) {
          logAndThrowError("_ptr_as_raw<T>(ptr) requires 1 argument");
        }
        sun::TypePtr targetType = typeAnnotationToType(*typeArgsVec[0]);
        targetType = substituteTypeParameters(targetType);
        if (!targetType) {
          logAndThrowError(
              "Failed to resolve type argument for _ptr_as_raw<T>");
        }
        return sun::Types::RawPointer(targetType);
      }

      if (funcName == "_is") {
        // _is<T>(value) returns bool - compile-time type check
        // T can be:
        //   - A concrete type (i32, Point, String)
        //   - A built-in type trait (_Integer, _Signed, _Unsigned, _Float,
        //   _Numeric, _Primitive)
        //   - A user-defined interface (IHashable, IIterable)
        if (genericCall.getArgs().size() != 1) {
          logAndThrowError("_is<T>(value) requires exactly 1 argument");
        }
        // Type argument is validated at codegen time
        return sun::Types::Bool();
      }

      // Check for user-defined generic functions
      auto genFuncIt = genericFunctionTable.find(resolvedName);
      if (genFuncIt != genericFunctionTable.end()) {
        const auto& genFunc = genFuncIt->second;

        // Store the generic function AST on the call node for codegen
        genericCall.setGenericFunctionAST(genFunc.AST);

        // Resolve return type with type parameter substitution
        auto resolvedType = genFunc.AST->getResolvedType();
        if (resolvedType) {
          auto genericFuncType =
              static_cast<sun::FunctionType*>(resolvedType.get());
          auto substitutedType = substituteTypeParameters(resolvedType);
          auto funcType =
              static_cast<sun::FunctionType*>(substitutedType.get());
          return funcType->getReturnType();
        } else {
          logAndThrowError("Generic function '" + funcName +
                           "' has unresolved return type for inference");
        }
      }

      // Check for generic class constructor: Box<i32>(42)
      auto* genericClassInfo = lookupGenericClass(resolvedName);
      if (genericClassInfo) {
        // Resolve type arguments
        std::vector<sun::TypePtr> typeArgs;
        const auto& typeArgsVec = genericCall.getTypeArguments();
        if (!typeArgsVec.empty()) {
          for (const auto& ta : typeArgsVec) {
            typeArgs.push_back(typeAnnotationToType(*ta));
          }
        }

        // Instantiate the generic class and return the specialized type
        auto specializedClass = instantiateGenericClass(resolvedName, typeArgs);
        if (specializedClass) {
          return specializedClass;
        }
      }

      logAndThrowError("Unknown generic intrinsic: " + funcName);
      return sun::Types::Void();
    }

    case ASTNodeType::PACK_EXPANSION: {
      // Pack expansion (args...) - represents multiple values at compile time
      // Type checking is deferred to codegen where we know the actual types
      // For now, just return void as a placeholder
      return sun::Types::Void();
    }

    default:
      logAndThrowError("Cannot infer type for expression of kind " +
                       std::to_string(static_cast<int>(expr.getType())));
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
    logAndThrowError("Cannot access member '" + memberName +
                     "' on unknown type");
  }

  // Unwrap raw_ptr<Class> to Class for member access
  if (objectType->isRawPointer()) {
    sun::TypePtr pointeeType =
        static_cast<sun::RawPointerType*>(objectType.get())->getPointeeType();
    if (pointeeType && pointeeType->isClass()) {
      objectType = pointeeType;
    }
  }

  // Now dispatch based on the resolved object type
  switch (objectType->getKind()) {
    case sun::Type::Kind::Module: {
      // Module member access: mod_x.mod_y or mod_x.varName
      auto* moduleType = static_cast<sun::ModuleType*>(objectType.get());
      std::string qualifiedName =
          moduleType->getModulePath() + "_" + memberName;

      // Check if it's a nested module (mod_x.mod_y)
      if (isModuleName(qualifiedName)) {
        return sun::Types::Module(qualifiedName);
      }

      // Check if it's a variable in this module
      VariableInfo* varInfo = lookupQualifiedVariable(qualifiedName);
      if (varInfo) {
        return varInfo->type;
      }

      // Check if it's a function in this module
      const FunctionInfo* funcInfo = lookupQualifiedFunction(qualifiedName);
      if (funcInfo) {
        return sun::Types::Function(funcInfo->returnType, funcInfo->paramTypes);
      }

      // Check if it's a class in this module
      auto classType = lookupClass(qualifiedName);
      if (classType) {
        return classType;
      }

      logAndThrowError("Unknown member '" + memberName + "' in module '" +
                       moduleType->getModulePath() + "'");
    }

    case sun::Type::Kind::Enum: {
      auto* enumType = static_cast<sun::EnumType*>(objectType.get());
      const auto* variant = enumType->getVariant(memberName);
      if (variant) {
        return objectType;  // Enum variant has the enum type
      }
      logAndThrowError("Unknown variant '" + memberName + "' in enum '" +
                       enumType->getName() + "'");
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
      logAndThrowError("Array has no member '" + memberName +
                       "'; available: 'shape'");
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
                       "'; available: 'length', 'data', '_get'");
    }

    case sun::Type::Kind::RawPointer: {
      // raw_ptr<T> where T is not a class (class case handled above)
      auto* ptrType = static_cast<sun::RawPointerType*>(objectType.get());
      if (memberName == "_get") {
        return ptrType->getPointeeType();
      }
      logAndThrowError("raw_ptr has no member '" + memberName +
                       "'; available: '_get'");
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
            auto classTypeShared = std::const_pointer_cast<sun::ClassType>(
                std::shared_ptr<const sun::ClassType>(
                    std::shared_ptr<const sun::ClassType>{}, classType));
            // Need non-const ClassType - look it up from class table
            auto mutableClassType = lookupClass(classType->getName());
            if (mutableClassType) {
              instantiateGenericMethod(mutableClassType, memberName,
                                       typeArgPtrs);
            }

            enterScope();
            addTypeParameterBindings(typeParams, typeArgPtrs);
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
                       classType->getName() + "'");
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
                       ifaceType->getName() + "'");
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
                             classType->getName() + "'");
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
                             "' on interface '" + ifaceType->getName() + "'");
          }
        }
      }
      // No narrowing available - type parameter has no known members
      logAndThrowError("Cannot access member '" + memberName +
                       "' on unconstrained type parameter '" +
                       objectType->toString() + "'");
    }

    default:
      logAndThrowError("Cannot access member '" + memberName + "' on type '" +
                       objectType->toString() + "'");
  }
}
