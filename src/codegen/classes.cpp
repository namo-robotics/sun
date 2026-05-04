// classes.cpp - Class-related codegen (class definitions, member access, etc.)

#include <cmath>
#include <cstdint>

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"
#include "intrinsics.h"
#include "parser.h"
#include "semantic_scope.h"

using namespace llvm;

// -------------------------------------------------------------------
// Precompiled class codegen (from linked bitcode)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenPrecompiledClass(const ClassDefinitionAST& expr,
                                               const std::string& className) {
  // Still need to register the class type for type checking
  auto classType = typeRegistry->getClass(className);
  if (classType) {
    // Generate specializations for generic methods on this non-generic
    // precompiled class (e.g., HeapAllocator.create<T>)
    auto savedClass = currentClass;
    auto savedThisPtr = thisPtr;
    currentClass = classType;

    for (const auto& methodDecl : expr.getMethods()) {
      const FunctionAST& methodFunc = *methodDecl.function;
      const PrototypeAST& proto = methodFunc.getProto();

      if (proto.isGeneric()) {
        // Generate any pre-computed specializations from semantic analysis
        for (const auto& [specMangledName, specializedAST] :
             methodFunc.getSpecializations()) {
          if (specializedAST) {
            declareMethodFromAST(*specializedAST, specMangledName);
            generateMethodBody(*specializedAST, specMangledName);
            // Don't add to userDefinedFunctions - these are library
            // specializations
          }
        }
      }
    }

    currentClass = savedClass;
    thisPtr = savedThisPtr;
  }

  // If the class is also generic, store the AST for later instantiation
  if (expr.isGeneric()) {
    genericClassASTs[className] = &expr;

    // Generate specializations that were added during semantic analysis.
    // Some may be library specializations (already in bitcode) - skip those.
    // Others may be user specializations (e.g., Vec<MyUserClass>) - codegen
    // those.
    llvm::errs() << "DEBUG: codegenPrecompiledClass generic: " << className
                 << " specializations=" << expr.getSpecializations().size()
                 << " addr=" << (void*)&expr << "\n";
    for (const auto& [mangledName, specializedAST] :
         expr.getSpecializations()) {
      if (!specializedAST || codegenedClasses.count(mangledName)) {
        llvm::errs() << "DEBUG: skip spec " << mangledName
                     << " null=" << !specializedAST
                     << " already=" << codegenedClasses.count(mangledName)
                     << "\n";
        continue;
      }

      // Check if this specialization already exists in the precompiled library.
      // Pre-declared specializations (e.g., Vec_i32, Matrix_f64) have their
      // methods declared from the bitcode metadata.
      // New user-triggered specializations (e.g., Vec<u32>, Vec<MyClass>) won't
      // have declarations and need codegen.
      std::string initMethodName = mangledName + "_init";
      if (isPrecompiledFunction(initMethodName)) {
        llvm::errs() << "DEBUG: skip precompiled " << mangledName
                     << " (init=" << initMethodName << ")\n";
        // Pre-declared library specialization - skip, bitcode will be linked
        continue;
      }

      llvm::errs() << "DEBUG: codegen spec " << mangledName << "\n";

      // New specialization - needs codegen
      // Mark as library specialization for IR filtering (still comes from
      // library generic, just with user type args)
      librarySpecializations.insert(mangledName);
      codegen(*specializedAST);
    }
  }

  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}

// -------------------------------------------------------------------
// Class definition codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const ClassDefinitionAST& expr) {
  // Get the class name - check if there's a qualified name via resolved type
  // The semantic analyzer may have qualified the name (e.g., sun_SliceRange)
  std::string className = expr.getName();
  if (expr.getResolvedType() && expr.getResolvedType()->isClass()) {
    auto* resolvedClass =
        static_cast<sun::ClassType*>(expr.getResolvedType().get());
    className = resolvedClass->getName();
  }

  // Skip precompiled classes - they come from linked bitcode
  if (expr.isPrecompiled()) {
    return codegenPrecompiledClass(expr, className);
  }

  // Skip generic class definitions (templates) - they are instantiated on
  // demand
  if (expr.isGeneric()) {
    // Store the generic class AST for later instantiation (needed for generic
    // method lookup)
    genericClassASTs[className] = &expr;

    // Generate all specializations that were created during semantic analysis
    // This mirrors how generic functions work - specializations are
    // pre-computed and stored on the AST
    for (const auto& [mangledName, specializedAST] :
         expr.getSpecializations()) {
      // Check if already codegenned (not just type-registered)
      if (specializedAST && !codegenedClasses.count(mangledName)) {
        codegen(*specializedAST);
      }
    }

    // Return a void value - generic class templates don't generate code
    return ConstantFP::get(ctx.getContext(), APFloat(0.0));
  }

  // Error if codegen sees an unmarked duplicate — this is a compiler bug
  if (codegenedClasses.count(className)) {
    logAndThrowError("Duplicate class definition reached codegen: " +
                     className);
  }

  // Mark this class as being codegenned
  codegenedClasses.insert(className);

  // Get the class type (already fully built by semantic analyzer)
  auto classType = typeRegistry->getClass(className);

  // Track if this class is user-defined (not from precompiled library)
  // Check both the precompiled flag and if this is a library specialization
  bool isUserDefined =
      !expr.isPrecompiled() && !librarySpecializations.count(className);

  // Create the LLVM struct type for the class
  llvm::StructType* structType = classType->getStructType(ctx.getContext());

  // Save current class context
  auto savedClass = currentClass;
  auto savedThisPtr = thisPtr;
  currentClass = classType;

  // PASS 1: Declare all method functions first (so methods can call each other)
  for (const auto& methodDecl : expr.getMethods()) {
    const FunctionAST& methodFunc = *methodDecl.function;
    const PrototypeAST& proto = methodFunc.getProto();
    const std::string& methodName = proto.getName();

    // Create mangled method name: ClassName_methodName
    std::string mangledName = classType->getMangledMethodName(methodName);

    // Skip generic methods during initial class codegen
    // They will be instantiated on-demand when called with specific type args
    if (proto.isGeneric()) {
      // Declare any pre-computed specializations from semantic analysis
      for (const auto& [specMangledName, specializedAST] :
           methodFunc.getSpecializations()) {
        if (specializedAST) {
          declareMethodFromAST(*specializedAST, specMangledName);
        }
      }

      continue;
    }

    // Declare the non-generic method using the shared helper
    declareMethodFromAST(methodFunc, mangledName);
  }

  // PASS 2: Generate all method bodies
  for (const auto& methodDecl : expr.getMethods()) {
    const FunctionAST& methodFunc = *methodDecl.function;
    const PrototypeAST& proto = methodFunc.getProto();

    // For generic methods, generate bodies for all pre-computed specializations
    if (proto.isGeneric()) {
      for (const auto& [specMangledName, specializedAST] :
           methodFunc.getSpecializations()) {
        if (specializedAST) {
          generateMethodBody(*specializedAST, specMangledName);
          // Track user-defined method specializations for IR filtering
          if (isUserDefined) {
            userDefinedFunctions.insert(specMangledName);
          }
        }
      }
      continue;
    }

    // Create mangled method name: ClassName_methodName
    std::string mangledName = classType->getMangledMethodName(proto.getName());
    generateMethodBody(methodFunc, mangledName);
    // Track user-defined methods for IR filtering
    if (isUserDefined) {
      userDefinedFunctions.insert(mangledName);
    }
  }

  // Generate wrapper methods for interface default implementations
  // that are not explicitly overridden in the class
  // Use classType's implemented interfaces (these have mangled names for
  // generics)
  for (const auto& interfaceName : classType->getImplementedInterfaces()) {
    auto interfaceType = typeRegistry->getInterface(interfaceName);
    if (!interfaceType) {
      llvm::errs() << "Warning: Interface not found for class " << className
                   << ": " << interfaceName << "\n";
      continue;
    }

    for (const auto& interfaceMethod : interfaceType->getMethods()) {
      // Skip methods without default implementations
      if (!interfaceMethod.hasDefaultImpl) continue;

      // Check if class already implements this method
      bool hasOverride = false;
      for (const auto& classMethod : expr.getMethods()) {
        if (classMethod.function->getProto().getName() ==
            interfaceMethod.name) {
          hasOverride = true;
          break;
        }
      }
      if (hasOverride) continue;

      // Generate wrapper method that calls the interface default
      std::string mangledName =
          classType->getMangledMethodName(interfaceMethod.name);
      std::string defaultMangledName =
          interfaceType->getMangledDefaultMethodName(interfaceMethod.name);

      // Build parameter types (with 'this' as first parameter)
      std::vector<llvm::Type*> paramTypes;
      paramTypes.push_back(
          PointerType::getUnqual(ctx.getContext()));  // this ptr
      for (const auto& pt : interfaceMethod.paramTypes) {
        paramTypes.push_back(typeResolver.resolve(pt));
      }

      // Get return type - use resolveForReturn for compound types by value
      llvm::Type* returnType =
          typeResolver.resolveForReturn(interfaceMethod.returnType);

      // Create the wrapper function type
      FunctionType* funcType = FunctionType::get(returnType, paramTypes, false);

      // Create the wrapper function
      Function* func = Function::Create(funcType, Function::ExternalLinkage,
                                        mangledName, module);

      // Create entry basic block
      BasicBlock* BB = BasicBlock::Create(ctx.getContext(), "entry", func);
      ctx.builder->SetInsertPoint(BB);

      // Get the default implementation function
      Function* defaultFunc = module->getFunction(defaultMangledName);
      if (!defaultFunc) {
        logAndThrowError("Default implementation not found: " +
                         defaultMangledName);
        continue;
      }

      // Build argument list (just forward all arguments)
      std::vector<Value*> args;
      for (auto& arg : func->args()) {
        args.push_back(&arg);
      }

      // Call the default implementation
      Value* result = ctx.builder->CreateCall(defaultFunc, args);

      // Return the result
      if (returnType->isVoidTy()) {
        ctx.builder->CreateRetVoid();
      } else {
        ctx.builder->CreateRet(result);
      }

      // Verify the wrapper function
      verifyFunction(*func);
    }
  }

  // PASS 3: Generate pre-computed specializations for generic methods
  // These were created by the semantic analyzer when generic methods were
  // called
  for (const auto& methodDecl : expr.getMethods()) {
    const FunctionAST& methodFunc = *methodDecl.function;
    const PrototypeAST& proto = methodFunc.getProto();

    // Only process generic methods
    if (!proto.isGeneric()) {
      continue;
    }

    // Iterate all specializations stored on this generic method's AST
    for (const auto& [mangledName, specializedAST] :
         methodFunc.getSpecializations()) {
      if (!specializedAST) {
        continue;
      }

      // Declare if not already declared, then generate body
      declareMethodFromAST(*specializedAST, mangledName);
      generateMethodBody(*specializedAST, mangledName);
    }
  }

  // PASS 4: Generate vtables for each implemented interface
  // A vtable contains function pointers for each interface method in
  // declaration order, allowing dynamic dispatch on interface-typed values.
  // Note: Generic methods cannot be included in vtables (they require
  // compile-time type information). Only non-generic methods are included.
  for (const auto& interfaceName : classType->getImplementedInterfaces()) {
    auto interfaceType = typeRegistry->getInterface(interfaceName);
    if (!interfaceType) {
      continue;
    }

    // Build vtable entries: one function pointer per non-generic interface
    // method
    std::vector<Constant*> vtableEntries;
    bool hasVtableMethods = false;
    for (const auto& interfaceMethod : interfaceType->getMethods()) {
      // Skip generic methods - they can't be dispatched via vtable
      if (interfaceMethod.isGeneric()) {
        continue;
      }
      hasVtableMethods = true;

      std::string methodMangledName =
          classType->getMangledMethodName(interfaceMethod.name);
      Function* methodFunc = module->getFunction(methodMangledName);
      if (!methodFunc) {
        logAndThrowError("Method not found for vtable: " + methodMangledName +
                         " in class " + className);
        return nullptr;
      }
      vtableEntries.push_back(methodFunc);
    }

    // Skip creating vtable if there are no non-generic methods
    if (!hasVtableMethods) {
      continue;
    }

    // Create the vtable struct type dynamically based on non-generic methods
    auto* ptrTy = PointerType::getUnqual(ctx.getContext());
    std::string vtableTypeName = className + "_" + interfaceName + "_vtable_t";
    std::vector<llvm::Type*> slotTypes(vtableEntries.size(), ptrTy);
    llvm::StructType* vtableType =
        llvm::StructType::create(ctx.getContext(), slotTypes, vtableTypeName);

    // Create the vtable global variable
    std::string vtableName = className + "_" + interfaceName + "_vtable";
    Constant* vtableInit = ConstantStruct::get(vtableType, vtableEntries);
    GlobalVariable* vtableGlobal = new GlobalVariable(
        *module, vtableType, /*isConstant=*/true, GlobalValue::InternalLinkage,
        vtableInit, vtableName);

    // Store in the vtable map for later lookup during fat pointer creation
    vtableGlobals[{className, interfaceName}] = vtableGlobal;
  }

  // Restore class context
  currentClass = savedClass;
  thisPtr = savedThisPtr;

  // Class definitions return void
  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}

// -------------------------------------------------------------------
// Declare a method function from a specialized AST (no body generated)
// -------------------------------------------------------------------

Function* CodegenVisitor::declareMethodFromAST(
    const FunctionAST& specializedAST, const std::string& mangledName) {
  // Skip if already declared
  if (Function* existing = module->getFunction(mangledName)) {
    return existing;
  }

  const PrototypeAST& proto = specializedAST.getProto();

  // Build parameter types: 'this' first, then regular params
  std::vector<llvm::Type*> paramTypes;
  paramTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // this

  if (!proto.hasResolvedParamTypes()) {
    logAndThrowError(
        "Method parameter types not resolved by semantic analysis: " +
        mangledName);
    return nullptr;
  }
  for (const auto& sunType : proto.getResolvedParamTypes()) {
    paramTypes.push_back(typeResolver.resolve(sunType));
  }
  // Add variadic params if present (from _init_args<T> expansion)
  for (const auto& sunType : proto.getResolvedVariadicTypes()) {
    paramTypes.push_back(typeResolver.resolve(sunType));
  }

  // Get return type (must be resolved by semantic analysis)
  llvm::Type* returnType;
  bool canError = proto.hasReturnType() && proto.getReturnType()->canError;
  if (proto.hasResolvedReturnType()) {
    returnType = typeResolver.resolveForReturn(proto.getResolvedReturnType());
  } else if (!proto.hasReturnType()) {
    returnType = Type::getVoidTy(ctx.getContext());
  } else {
    logAndThrowError("Method return type not resolved by semantic analysis: " +
                     mangledName);
    return nullptr;
  }

  // Wrap in error union if needed
  if (canError && returnType && !returnType->isVoidTy()) {
    returnType = llvm::StructType::get(
        ctx.getContext(),
        {llvm::Type::getInt1Ty(ctx.getContext()), returnType});
  }

  // Create the function declaration
  FunctionType* funcType = FunctionType::get(returnType, paramTypes, false);
  Function* func = Function::Create(funcType, Function::ExternalLinkage,
                                    mangledName, module);

  // Set parameter names
  auto argIt = func->arg_begin();
  argIt->setName("this");
  ++argIt;

  for (const auto& [argName, argType] : proto.getArgs()) {
    // Skip variadic param - it's handled below with indexed names
    if (proto.hasVariadicParam() && argName == *proto.getVariadicParamName())
      continue;
    argIt->setName(argName);
    ++argIt;
  }

  // Name variadic params with indexed names (e.g., "args.0", "args.1")
  if (proto.hasVariadicParam() && proto.hasResolvedVariadicTypes()) {
    const std::string& variadicName = *proto.getVariadicParamName();
    for (size_t i = 0; i < proto.getResolvedVariadicTypes().size(); ++i) {
      argIt->setName(variadicName + "." + std::to_string(i));
      ++argIt;
    }
  }

  return func;
}

// -------------------------------------------------------------------
// Generate a method body for an already-declared function
// -------------------------------------------------------------------

void CodegenVisitor::generateMethodBody(const FunctionAST& methodFunc,
                                        const std::string& mangledName) {
  const PrototypeAST& proto = methodFunc.getProto();

  // Get the function (must already be declared)
  Function* func = module->getFunction(mangledName);
  if (!func) return;

  // Skip if the function already has a body
  if (!func->empty()) return;

  // Get return type
  llvm::Type* returnType = func->getReturnType();

  // Check if this method can return errors (declared with ', IError')
  bool canError = proto.hasReturnType() && proto.getReturnType()->canError;
  llvm::Type* valueType = nullptr;
  if (canError) {
    // Get the actual value type (not the error union)
    // Return type must be resolved by semantic analysis
    if (!proto.hasResolvedReturnType()) {
      logAndThrowError(
          "Method return type not resolved by semantic analysis: " +
          mangledName);
      return;
    }
    sun::TypePtr sunRetType = proto.getResolvedReturnType();
    if (sunRetType && sunRetType->toString() != "void") {
      valueType = typeResolver.resolveForReturn(sunRetType);
    }
  }

  // Save and set error handling context
  bool savedCanError = currentFunctionCanError;
  llvm::Type* savedValueType = currentFunctionValueType;
  currentFunctionCanError = canError;
  currentFunctionValueType = canError ? valueType : nullptr;

  // Create entry basic block
  BasicBlock* BB = BasicBlock::Create(ctx.getContext(), "entry", func);
  ctx.builder->SetInsertPoint(BB);

  // Create a new scope for the method
  pushScope();

  // Store 'this' pointer for method body access
  AllocaInst* thisAlloca = ctx.builder->CreateAlloca(
      PointerType::getUnqual(ctx.getContext()), nullptr, "this.addr");
  ctx.builder->CreateStore(&*func->arg_begin(), thisAlloca);
  thisPtr = ctx.builder->CreateLoad(PointerType::getUnqual(ctx.getContext()),
                                    thisAlloca, "this");

  // Store 'this' in scope so it can be found
  scopes.back().variables["this"] = thisAlloca;

  // Store other parameters
  auto argIt = func->arg_begin();
  ++argIt;  // Skip 'this'

  // Use resolved param types if available (for specialized generic classes)
  const auto& protoArgs = proto.getArgs();
  const auto& resolvedTypes = proto.getResolvedParamTypes();
  bool hasResolved = proto.hasResolvedParamTypes();

  for (size_t i = 0; i < protoArgs.size(); ++i) {
    const auto& [argName, argType] = protoArgs[i];
    // Skip variadic param - it's handled below with indexed names
    if (proto.hasVariadicParam() && argName == *proto.getVariadicParamName())
      continue;
    if (!hasResolved || i >= resolvedTypes.size()) {
      logAndThrowError(
          "Method parameter type not resolved by semantic analysis: " +
          mangledName + " param " + argName);
      return;
    }
    llvm::Type* argLLVMType = typeResolver.resolve(resolvedTypes[i]);

    AllocaInst* alloca =
        ctx.builder->CreateAlloca(argLLVMType, nullptr, argName);
    ctx.builder->CreateStore(&*argIt, alloca);
    scopes.back().variables[argName] = alloca;
    ++argIt;
  }

  // Store variadic params with indexed names (e.g., "args.0", "args.1")
  if (proto.hasVariadicParam() && proto.hasResolvedVariadicTypes()) {
    const std::string& variadicName = *proto.getVariadicParamName();
    const auto& variadicTypes = proto.getResolvedVariadicTypes();
    for (size_t i = 0; i < variadicTypes.size(); ++i) {
      std::string indexedName = variadicName + "." + std::to_string(i);
      llvm::Type* argLLVMType = typeResolver.resolve(variadicTypes[i]);
      AllocaInst* alloca =
          ctx.builder->CreateAlloca(argLLVMType, nullptr, indexedName);
      ctx.builder->CreateStore(&*argIt, alloca);
      scopes.back().variables[indexedName] = alloca;
      ++argIt;
    }
  }

  // Generate the method body
  codegen(methodFunc.getBody());

  // Add return void if no explicit return and function returns void
  if (returnType->isVoidTy()) {
    if (!ctx.builder->GetInsertBlock()->getTerminator()) {
      ctx.builder->CreateRetVoid();
    }
  }

  popScope();

  // Restore error handling context
  currentFunctionCanError = savedCanError;
  currentFunctionValueType = savedValueType;

  // Verify the function
  verifyFunction(*func);
}

// -------------------------------------------------------------------
// 'this' expression codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const ThisExprAST& expr) {
  if (!thisPtr) {
    logAndThrowError("'this' used outside of a class method");
    return nullptr;
  }
  return thisPtr;
}

// -------------------------------------------------------------------
// Member access codegen (field read)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const MemberAccessAST& expr) {
  const std::string& memberName = expr.getMemberName();

  // Handle module member access: mod_x.mod_y or mod_x.var
  sun::TypePtr objectType = expr.getObject()->getResolvedType();
  if (objectType && objectType->isModule()) {
    auto* moduleType = static_cast<sun::ModuleType*>(objectType.get());
    // Use resolved name from semantic analysis (includes library hash prefix)
    std::string qualifiedName;
    if (expr.hasResolvedQualifiedName()) {
      qualifiedName = expr.getResolvedQualifiedName();
    } else {
      qualifiedName =
          mangleModulePath(moduleType->getModulePath()) + "_" + memberName;
    }

    // Check if the result type is also a module (nested module access)
    sun::TypePtr resultType = expr.getResolvedType();
    if (resultType && resultType->isModule()) {
      // Return null sentinel - next member access will handle it
      return llvm::ConstantPointerNull::get(
          llvm::PointerType::getUnqual(ctx.getContext()));
    }

    // Check for global variable in this module
    GlobalVariable* gv = module->getGlobalVariable(qualifiedName);
    if (gv) {
      sun::TypePtr varType = expr.getResolvedType();
      // Classes and interfaces return the pointer, not a load
      if (varType && (varType->isClass() || varType->isInterface())) {
        return gv;
      }
      return ctx.builder->CreateLoad(gv->getValueType(), gv,
                                     memberName.c_str());
    }

    // Check for function in this module
    if (Function* func = module->getFunction(qualifiedName)) {
      return func;
    }

    logAndThrowError("Cannot find member '" + memberName + "' in module '" +
                     moduleType->getModulePath() + "'");
  }

  // Check for enum variant access: EnumName.VariantName
  if (expr.getObject()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*expr.getObject());
    auto enumType = typeRegistry->getEnum(varRef.getName());
    if (enumType && enumType->getNumVariants() > 0) {
      const auto* variant = enumType->getVariant(memberName);
      if (variant) {
        // Return the variant value as i32 constant
        return ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()),
                                variant->value);
      }
    }
  }

  // Refresh objectType in case it was not set from module handling above
  if (!objectType) {
    objectType = expr.getObject()->getResolvedType();
  }

  // Handle array.shape() - returns a 1D array of dimension sizes
  if (objectType && (objectType->isArray() ||
                     (objectType->isReference() &&
                      static_cast<sun::ReferenceType*>(objectType.get())
                          ->getReferencedType()
                          ->isArray()))) {
    if (memberName == "shape") {
      return codegenArrayShape(expr);
    }
  }

  // Handle static_ptr members (length, data, get)
  if (objectType && objectType->isStaticPointer()) {
    auto* ptrType = static_cast<sun::StaticPointerType*>(objectType.get());
    Value* result = codegenStaticPtrMemberAccess(expr, ptrType);
    if (result) return result;
    // Falls through if pointing to class and member not found
  }

  // Handle raw_ptr<T>.get() - dereference raw pointer
  if (objectType && objectType->isRawPointer()) {
    auto* ptrType = static_cast<sun::RawPointerType*>(objectType.get());
    Value* result = codegenRawPtrMemberAccess(expr, ptrType);
    if (result) return result;
    // Falls through if pointing to class and member not 'get'
  }

  // Get the object value
  Value* objectPtr = codegen(*expr.getObject());
  if (!objectPtr) return nullptr;

  // For generic method bodies, 'this' may have a type parameter type.
  // In that case, use the currentClass which is the specialized type.
  if (dynamic_cast<const ThisExprAST*>(expr.getObject()) && currentClass) {
    objectType = currentClass;
  }

  // Handle pointer-to-class types: raw_ptr<ClassName>.member or
  // static_ptr<ClassName>.member
  // The pointer value is already the 'this' pointer
  if (objectType &&
      (objectType->isRawPointer() || objectType->isStaticPointer())) {
    sun::TypePtr pointeeType = nullptr;
    if (objectType->isRawPointer()) {
      pointeeType =
          static_cast<sun::RawPointerType*>(objectType.get())->getPointeeType();
    } else {
      pointeeType = static_cast<sun::StaticPointerType*>(objectType.get())
                        ->getPointeeType();
    }
    if (pointeeType && pointeeType->isClass()) {
      objectType = pointeeType;
    }
  }

  // Handle reference-to-class types: ref ClassName.member
  // The reference value is already the object pointer
  if (objectType && objectType->isReference()) {
    sun::TypePtr referencedType =
        static_cast<sun::ReferenceType*>(objectType.get())->getReferencedType();
    if (referencedType && referencedType->isClass()) {
      objectType = referencedType;
    }
  }

  if (!objectType || !objectType->isClass()) {
    logAndThrowError("Member access on non-class type");
    return nullptr;
  }

  auto* classType = static_cast<sun::ClassType*>(objectType.get());

  // Check if it's a field access
  const sun::ClassField* field = classType->getField(memberName);
  if (field) {
    // Generate GEP to access the field
    llvm::StructType* structType = classType->getStructType(ctx.getContext());
    Value* fieldPtr = ctx.builder->CreateStructGEP(structType, objectPtr,
                                                   field->index, memberName);

    // For class-typed fields (embedded structs), return the pointer to the
    // embedded struct (class values in Sun are pointers)
    if (field->type->isClass()) {
      return fieldPtr;
    }

    // Load the field value
    llvm::Type* fieldLLVMType = field->type->toLLVMType(ctx.getContext());
    return ctx.builder->CreateLoad(fieldLLVMType, fieldPtr,
                                   memberName + ".val");
  }

  // It's a method - just return the object pointer
  // The actual method call will be handled in CallExprAST
  return objectPtr;
}

// -------------------------------------------------------------------
// Stack-allocated class instance codegen: ClassName(args...)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenStackClassInstance(const CallExprAST& expr,
                                                 const std::string& className,
                                                 sun::ClassType& classType) {
  // Get the LLVM struct type for the class
  llvm::StructType* structType = classType.getStructType(ctx.getContext());

  // Create stack allocation (alloca) for the class instance
  Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* alloca =
      createEntryBlockAlloca(currentFunc, "stack.obj", structType);

  // Zero-initialize the memory using memset
  const DataLayout& DL = module->getDataLayout();
  uint64_t structSize = DL.getTypeAllocSize(structType);

  llvm::FunctionCallee memsetFn = module->getOrInsertFunction(
      "memset", FunctionType::get(PointerType::getUnqual(ctx.getContext()),
                                  {PointerType::getUnqual(ctx.getContext()),
                                   Type::getInt32Ty(ctx.getContext()),
                                   Type::getInt64Ty(ctx.getContext())},
                                  false));
  ctx.builder->CreateCall(
      memsetFn,
      {alloca, ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0),
       ConstantInt::get(Type::getInt64Ty(ctx.getContext()), structSize)});

  // Call the constructor (init method) if it exists
  std::string baseCtorName = classType.getMangledMethodName("init");
  Function* ctorFunc = nullptr;
  size_t argCount = expr.getArgs().size();

  // Get the init method info from the class type
  const sun::ClassMethod* initMethod = classType.getMethod("init");

  // Try to find existing function in module
  Function* candidate = module->getFunction(baseCtorName);

  // If not found but init method exists in class type, create declaration
  // This handles precompiled classes where functions are linked later
  if (!candidate && initMethod) {
    std::vector<llvm::Type*> paramLLVMTypes;
    paramLLVMTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // this
    for (const auto& paramType : initMethod->paramTypes) {
      paramLLVMTypes.push_back(typeResolver.resolve(paramType));
    }
    FunctionType* funcType = FunctionType::get(
        Type::getVoidTy(ctx.getContext()), paramLLVMTypes, false);
    candidate = Function::Create(funcType, Function::ExternalLinkage,
                                 baseCtorName, module);
  }

  if (candidate && candidate->arg_size() == argCount + 1) {
    ctorFunc = candidate;
  }

  if (ctorFunc) {
    const auto& paramTypes =
        initMethod ? initMethod->paramTypes : std::vector<sun::TypePtr>{};

    std::vector<Value*> ctorArgs =
        generateCtorArgs(alloca, expr.getArgs(), paramTypes);
    ctx.builder->CreateCall(ctorFunc, ctorArgs);
  }

  return alloca;
}

// -------------------------------------------------------------------
// Generate constructor argument values, handling ref parameters
// -------------------------------------------------------------------

std::vector<Value*> CodegenVisitor::generateCtorArgs(
    Value* thisPtr, const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<sun::TypePtr>& paramTypes) {
  std::vector<Value*> ctorArgs;
  ctorArgs.push_back(thisPtr);  // 'this' pointer

  size_t argIdx = 0;
  for (const auto& arg : args) {
    bool isRefParam = argIdx < paramTypes.size() && paramTypes[argIdx] &&
                      paramTypes[argIdx]->isReference();
    sun::TypePtr argSunType = arg->getResolvedType();

    if (isRefParam) {
      // For reference parameters, pass the address of the argument
      if (auto* varRef = dynamic_cast<const VariableReferenceAST*>(arg.get())) {
        AllocaInst* varAlloca = findVariable(varRef->getName());
        if (varAlloca) {
          // Check if the alloca holds the class struct directly (value type)
          // or holds a pointer to the struct (pointer type)
          if (argSunType && argSunType->isClass()) {
            llvm::Type* allocatedType = varAlloca->getAllocatedType();
            if (allocatedType->isStructTy()) {
              // Class value stored directly in alloca - pass alloca ptr
              ctorArgs.push_back(varAlloca);
            } else {
              // Class stored as pointer - load and pass the pointer
              Value* ptrVal = ctx.builder->CreateLoad(
                  llvm::PointerType::getUnqual(ctx.getContext()), varAlloca,
                  varRef->getName() + ".ptr");
              ctorArgs.push_back(ptrVal);
            }
          } else if (argSunType && argSunType->isReference()) {
            // Variable is already a reference - load the pointer
            Value* ptrVal = ctx.builder->CreateLoad(
                llvm::PointerType::getUnqual(ctx.getContext()), varAlloca,
                varRef->getName() + ".ptr");
            ctorArgs.push_back(ptrVal);
          } else {
            // Pass address of the variable
            ctorArgs.push_back(varAlloca);
          }
        } else {
          logAndThrowError("Cannot find variable for reference parameter: " +
                           varRef->getName());
          return {};
        }
      } else if (argSunType && argSunType->isArray()) {
        // Array value expression - need to create a temporary alloca
        Value* argVal = codegen(*arg);
        if (!argVal) return {};
        llvm::StructType* fatType =
            sun::ArrayType::getArrayStructType(ctx.getContext());
        AllocaInst* tempAlloca =
            ctx.builder->CreateAlloca(fatType, nullptr, "arr.temp");
        ctx.builder->CreateStore(argVal, tempAlloca);
        ctorArgs.push_back(tempAlloca);
      } else if (dynamic_cast<const MemberAccessAST*>(arg.get())) {
        // Member access (e.g., this.alloc) - codegen gives pointer for class
        // fields, loaded value for primitives
        Value* val = codegen(*arg);
        if (!val) return {};
        if (val->getType()->isPointerTy()) {
          ctorArgs.push_back(val);
        } else {
          AllocaInst* tempAlloca =
              ctx.builder->CreateAlloca(val->getType(), nullptr, "ref.member");
          ctx.builder->CreateStore(val, tempAlloca);
          ctorArgs.push_back(tempAlloca);
        }
      } else {
        logAndThrowError(
            "Reference parameter must be passed a variable, not an expression");
        return {};
      }
    } else {
      Value* argVal = codegen(*arg);
      if (!argVal) return {};

      // Handle implicit widening for arguments:
      // Widen smaller integers to larger integers (i32 -> i64)
      if (argIdx < paramTypes.size() && paramTypes[argIdx]) {
        llvm::Type* expectedType = typeResolver.resolve(paramTypes[argIdx]);
        if (argVal->getType()->isIntegerTy() && expectedType->isIntegerTy()) {
          unsigned argBits = argVal->getType()->getIntegerBitWidth();
          unsigned paramBits = expectedType->getIntegerBitWidth();
          if (argBits < paramBits) {
            argVal = ctx.builder->CreateSExt(argVal, expectedType, "widen");
          }
        } else if (argVal->getType()->isFloatTy() &&
                   expectedType->isDoubleTy()) {
          argVal = ctx.builder->CreateFPExt(argVal, expectedType, "widen");
        }
      }

      ctorArgs.push_back(argVal);
    }
    ++argIdx;
  }

  return ctorArgs;
}

// -------------------------------------------------------------------
// Member assignment codegen (field write)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const MemberAssignmentAST& expr) {
  // Get the object pointer
  Value* objectPtr = codegen(*expr.getObject());
  if (!objectPtr) return nullptr;

  // Get the object's type
  sun::TypePtr objectType = expr.getObject()->getResolvedType();

  // For generic method bodies, 'this' may have a type parameter type.
  // In that case, use the currentClass which is the specialized type.
  if (dynamic_cast<const ThisExprAST*>(expr.getObject()) && currentClass) {
    objectType = currentClass;
  }

  // Handle pointer-to-class types: ptr<ClassName>.member = value or raw_ptr or
  // Handle pointer-to-class: raw_ptr<ClassName>.member or
  // static_ptr<ClassName>.member The pointer value is already the 'this'
  // pointer
  if (objectType &&
      (objectType->isRawPointer() || objectType->isStaticPointer())) {
    sun::TypePtr pointeeType = nullptr;
    if (objectType->isRawPointer()) {
      pointeeType =
          static_cast<sun::RawPointerType*>(objectType.get())->getPointeeType();
    } else {
      pointeeType = static_cast<sun::StaticPointerType*>(objectType.get())
                        ->getPointeeType();
    }
    if (pointeeType && pointeeType->isClass()) {
      objectType = pointeeType;
    }
  }

  if (!objectType || !objectType->isClass()) {
    logAndThrowError("Member assignment on non-class type");
    return nullptr;
  }

  auto* classType = static_cast<sun::ClassType*>(objectType.get());
  const std::string& memberName = expr.getMemberName();

  // Get the field info
  const sun::ClassField* field = classType->getField(memberName);
  if (!field) {
    logAndThrowError("Unknown field: " + memberName);
    return nullptr;
  }

  // Generate the value to assign
  Value* value = codegen(*expr.getValue());
  if (!value) return nullptr;

  // Get expected field type
  llvm::Type* fieldLLVMType = field->type->toLLVMType(ctx.getContext());
  llvm::Type* valueType = value->getType();

  // Handle ref array<T> assigned to array<T> field - load the fat struct from
  // the ref
  sun::TypePtr valueSunType = expr.getValue()->getResolvedType();
  if (valueSunType && valueSunType->isReference() && field->type->isArray()) {
    auto* refType = static_cast<sun::ReferenceType*>(valueSunType.get());
    if (refType->getReferencedType()->isArray()) {
      // value is a pointer to the fat struct, load it
      llvm::StructType* fatType =
          sun::ArrayType::getArrayStructType(ctx.getContext());
      value = ctx.builder->CreateLoad(fatType, value, "arr.fat.load");
      valueType = value->getType();
    }
  }

  // Generate GEP to access the field
  llvm::StructType* structType = classType->getStructType(ctx.getContext());
  Value* fieldPtr = ctx.builder->CreateStructGEP(
      structType, objectPtr, field->index, memberName + ".ptr");

  // Handle class-typed fields: copy the embedded struct data
  if (field->type->isClass()) {
    auto* fieldClassType = static_cast<sun::ClassType*>(field->type.get());
    llvm::StructType* fieldStructType =
        fieldClassType->getStructType(ctx.getContext());
    const DataLayout& DL = module->getDataLayout();
    uint64_t structSize = DL.getTypeAllocSize(fieldStructType);

    // value is a pointer to the source class instance
    // fieldPtr is a pointer to the embedded struct in the parent class
    // If value is not a pointer (e.g., struct returned by value from a call),
    // materialize it to a stack alloca first so memcpy has a valid source.
    if (!value->getType()->isPointerTy()) {
      AllocaInst* tempAlloca = ctx.builder->CreateAlloca(
          fieldStructType, nullptr, memberName + ".tmp");
      ctx.builder->CreateStore(value, tempAlloca);
      value = tempAlloca;
    }
    ctx.builder->CreateMemCpy(fieldPtr, llvm::MaybeAlign(8), value,
                              llvm::MaybeAlign(8), structSize);
    return value;
  }

  // Handle implicit widening for literals assigned to wider types
  if (valueType != fieldLLVMType) {
    bool valueIsLiteral = expr.getValue()->getType() == ASTNodeType::NUMBER;

    if (valueIsLiteral) {
      // Integer widening
      if (valueType->isIntegerTy() && fieldLLVMType->isIntegerTy()) {
        unsigned valueBits = valueType->getIntegerBitWidth();
        unsigned fieldBits = fieldLLVMType->getIntegerBitWidth();
        if (valueBits < fieldBits) {
          value = ctx.builder->CreateSExt(value, fieldLLVMType, "widen");
        }
      }
      // Float widening
      else if (valueType->isFloatTy() && fieldLLVMType->isDoubleTy()) {
        value = ctx.builder->CreateFPExt(value, fieldLLVMType, "widen");
      }
    }
  }

  // Store the value
  ctx.builder->CreateStore(value, fieldPtr);

  // Return the value (like C assignment)
  return value;
}

// -------------------------------------------------------------------
// Interface definition codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const InterfaceDefinitionAST& expr) {
  const std::string& interfaceName = expr.getName();

  // Skip precompiled interfaces - they come from linked bitcode
  if (expr.isPrecompiled()) {
    return ConstantFP::get(ctx.getContext(), APFloat(0.0));
  }

  // Get the interface type (already fully built by semantic analyzer)
  auto interfaceType = typeRegistry->getInterface(interfaceName);

  // Generate default method implementations
  for (const auto& methodDecl : expr.getMethods()) {
    if (!methodDecl.hasDefaultImpl) {
      // No default implementation - skip (method already registered by semantic
      // analyzer)
      continue;
    }

    // Generate default implementation
    const FunctionAST& methodFunc = *methodDecl.function;
    const PrototypeAST& proto = methodFunc.getProto();
    const std::string& methodName = proto.getName();

    // Create mangled method name for default implementation:
    // InterfaceName_default_methodName
    std::string mangledName =
        interfaceType->getMangledDefaultMethodName(methodName);

    // Build the method parameter types (with 'this' as first parameter - using
    // opaque pointer)
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // this ptr

    // Interface default methods must have resolved param types from semantic
    // analysis
    if (!proto.hasResolvedParamTypes()) {
      logAndThrowError(
          "Interface default method parameter types not resolved by semantic "
          "analysis: " +
          mangledName);
      continue;
    }
    for (const auto& sunType : proto.getResolvedParamTypes()) {
      paramTypes.push_back(typeResolver.resolve(sunType));
    }

    // Get return type (must be resolved by semantic analysis)
    llvm::Type* returnType;
    if (proto.hasResolvedReturnType()) {
      returnType = typeResolver.resolveForReturn(proto.getResolvedReturnType());
    } else if (!proto.hasReturnType()) {
      returnType = Type::getVoidTy(ctx.getContext());
    } else {
      logAndThrowError(
          "Interface default method return type not resolved by semantic "
          "analysis: " +
          mangledName);
      continue;
    }

    // Create the function type
    FunctionType* funcType = FunctionType::get(returnType, paramTypes, false);

    // Create the function
    Function* func = Function::Create(funcType, Function::ExternalLinkage,
                                      mangledName, module);

    // Set parameter names
    auto argIt = func->arg_begin();
    argIt->setName("this");
    ++argIt;

    for (const auto& [argName, argType] : proto.getArgs()) {
      argIt->setName(argName);
      ++argIt;
    }

    // Create entry basic block
    BasicBlock* BB = BasicBlock::Create(ctx.getContext(), "entry", func);
    ctx.builder->SetInsertPoint(BB);

    // Create a new scope for the method
    pushScope();

    // Store 'this' pointer for method body access
    AllocaInst* thisAlloca = ctx.builder->CreateAlloca(
        PointerType::getUnqual(ctx.getContext()), nullptr, "this.addr");
    ctx.builder->CreateStore(&*func->arg_begin(), thisAlloca);
    thisPtr = ctx.builder->CreateLoad(PointerType::getUnqual(ctx.getContext()),
                                      thisAlloca, "this");

    // Store 'this' in scope so it can be found
    scopes.back().variables["this"] = thisAlloca;

    // Store other parameters
    argIt = func->arg_begin();
    ++argIt;  // Skip 'this'

    // Use resolved param types for storing parameters
    const auto& resolvedParamTypes = proto.getResolvedParamTypes();
    size_t paramIdx = 0;
    for (const auto& [argName, argType] : proto.getArgs()) {
      if (paramIdx >= resolvedParamTypes.size()) {
        logAndThrowError(
            "Interface default method parameter type not resolved: " +
            mangledName + " param " + argName);
        break;
      }
      llvm::Type* argLLVMType =
          typeResolver.resolve(resolvedParamTypes[paramIdx]);

      AllocaInst* alloca =
          ctx.builder->CreateAlloca(argLLVMType, nullptr, argName);
      ctx.builder->CreateStore(&*argIt, alloca);
      scopes.back().variables[argName] = alloca;
      ++argIt;
      ++paramIdx;
    }

    // Generate the method body
    codegen(methodFunc.getBody());

    // Add return void if no explicit return and function returns void
    if (returnType->isVoidTy()) {
      // Check if the block is properly terminated
      if (!ctx.builder->GetInsertBlock()->getTerminator()) {
        ctx.builder->CreateRetVoid();
      }
    }

    popScope();
    thisPtr = nullptr;

    // Verify the function
    verifyFunction(*func);
  }

  // Interface definitions return void
  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}

// -------------------------------------------------------------------
// Enum definition codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const EnumDefinitionAST& expr) {
  // Enum definitions are already fully registered by the semantic analyzer
  // in the TypeRegistry. No additional codegen is needed here since enums
  // are represented as i32 constants - the values are emitted inline when
  // enum variants are referenced.

  // Skip precompiled enums - they come from linked bitcode
  // (handled by semantic analyzer)

  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}

// -------------------------------------------------------------------
// Generic call codegen: name<Type>(args...)
// For standalone generic functions (not methods)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const GenericCallAST& expr) {
  const std::string& funcName = expr.getFunctionName();
  const auto& typeArgs = expr.getTypeArguments();

  // Get first resolved type argument (semantic analysis must have set this)
  auto getFirstTypeArg = [&]() -> sun::TypePtr {
    if (!expr.hasResolvedTypeArgs() || expr.getResolvedTypeArgs().empty()) {
      logAndThrowError("Type argument not resolved by semantic analysis for: " +
                       funcName);
    }
    return expr.getResolvedTypeArgs()[0];
  };

  // Handle generic intrinsics via switch
  switch (sun::getIntrinsic(funcName)) {
    case sun::Intrinsic::Sizeof:
      return codegenSizeofIntrinsic(getFirstTypeArg());
    case sun::Intrinsic::Init:
      return codegenInitIntrinsic(getFirstTypeArg(), expr.getArgs());
    case sun::Intrinsic::Load:
      return codegenLoadIntrinsic(getFirstTypeArg(), expr.getArgs());
    case sun::Intrinsic::Store:
      return codegenStoreIntrinsic(getFirstTypeArg(), expr.getArgs());
    case sun::Intrinsic::StaticPtrData:
      return codegenStaticPtrDataIntrinsic(expr.getArgs());
    case sun::Intrinsic::StaticPtrLen:
      return codegenStaticPtrLenIntrinsic(expr.getArgs());
    case sun::Intrinsic::PtrAsRaw:
      return codegenPtrAsRawIntrinsic(expr.getArgs());
    case sun::Intrinsic::Is:
      // _is<T> uses the type name for type trait checks (e.g., "_Integer")
      return codegenIsIntrinsic(typeArgs[0]->baseName, expr.getArgs());
    default:
      break;  // Not a generic intrinsic, continue below
  }

  // Check for user-defined generic functions (AST provided by semantic
  // analysis)
  const FunctionAST* genericFuncAST = expr.getGenericFunctionAST();
  if (genericFuncAST) {
    // Use pre-resolved type arguments from semantic analysis
    if (!expr.hasResolvedTypeArgs()) {
      logAndThrowError(
          "Type arguments not resolved by semantic analysis for generic "
          "function: " +
          funcName);
      return nullptr;
    }
    const std::vector<sun::TypePtr>& resolvedTypeArgs =
        expr.getResolvedTypeArgs();

    // Build mangled name to look up the correct specialization
    std::string mangledName = funcName;
    for (const auto& typeArg : resolvedTypeArgs) {
      mangledName += "_" + typeArg->toString();
    }

    // The specialized function should already exist - it was generated when
    // we processed the generic function definition via codegenFunc
    Function* specializedFunc = module->getFunction(mangledName);
    if (!specializedFunc) {
      logAndThrowError(
          "Specialized function not found (should have been generated when "
          "processing the generic function definition): " +
          mangledName);
      return nullptr;
    }

    // Generate arguments for the call
    std::vector<Value*> argValues;

    // If function has captures, the env was stored in scope during codegenFunc
    if (AllocaInst* envPtr = findVariable(mangledName)) {
      argValues.push_back(envPtr);
    }

    for (const auto& arg : expr.getArgs()) {
      Value* val = codegen(*arg);
      if (!val) {
        logAndThrowError("Failed to generate argument for generic call");
        return nullptr;
      }
      argValues.push_back(val);
    }

    return ctx.builder->CreateCall(specializedFunc, argValues);
  }

  // Check for generic class constructor: Box<i32>(42)
  // Use pre-resolved type arguments from semantic analysis
  if (!expr.hasResolvedTypeArgs()) {
    logAndThrowError(
        "Type arguments not resolved by semantic analysis for generic class "
        "constructor: " +
        funcName);
    return nullptr;
  }
  const std::vector<sun::TypePtr>& resolvedTypeArgs =
      expr.getResolvedTypeArgs();

  // Get the mangled class name - use resolved type if available to handle
  // qualified names from using imports (e.g., Unique -> sun_Unique)
  std::string baseName = funcName;
  if (expr.getResolvedType() && expr.getResolvedType()->isClass()) {
    baseName =
        static_cast<sun::ClassType*>(expr.getResolvedType().get())->getName();
    // Strip any trailing template params that may already be in the name
    size_t parenPos = baseName.find('_');
    // Actually the ClassType name should already be the full mangled name
    // e.g., "sun_Unique_Point" - so we can use it directly
    auto classType = typeRegistry->getClass(baseName);
    if (classType) {
      // Create a stack-allocated instance and call constructor
      llvm::StructType* structType = classType->getStructType(ctx.getContext());
      Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
      AllocaInst* alloca =
          createEntryBlockAlloca(currentFunc, "stack.obj", structType);

      // Zero-initialize
      const DataLayout& DL = module->getDataLayout();
      uint64_t structSize = DL.getTypeAllocSize(structType);
      llvm::FunctionCallee memsetFn = module->getOrInsertFunction(
          "memset", FunctionType::get(PointerType::getUnqual(ctx.getContext()),
                                      {PointerType::getUnqual(ctx.getContext()),
                                       Type::getInt32Ty(ctx.getContext()),
                                       Type::getInt64Ty(ctx.getContext())},
                                      false));
      ctx.builder->CreateCall(
          memsetFn,
          {alloca, ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0),
           ConstantInt::get(Type::getInt64Ty(ctx.getContext()), structSize)});

      // Call constructor (init method) if it exists
      std::string baseCtorName = classType->getMangledMethodName("init");
      Function* ctorFunc = nullptr;
      size_t argCount = expr.getArgs().size();

      // Get the init method info from the class type
      const sun::ClassMethod* initMethod = classType->getMethod("init");

      // Try to find existing function in module
      std::string resolvedCtorName = baseCtorName;
      Function* candidate = module->getFunction(resolvedCtorName);

      // If not found but init method exists in class type, create declaration
      // This handles cases where the class codegen hasn't run yet
      if (!candidate && initMethod) {
        std::vector<llvm::Type*> paramLLVMTypes;
        paramLLVMTypes.push_back(
            PointerType::getUnqual(ctx.getContext()));  // this
        for (const auto& paramType : initMethod->paramTypes) {
          paramLLVMTypes.push_back(typeResolver.resolve(paramType));
        }
        FunctionType* funcType = FunctionType::get(
            Type::getVoidTy(ctx.getContext()), paramLLVMTypes, false);
        candidate = Function::Create(funcType, Function::ExternalLinkage,
                                     resolvedCtorName, module);
      }

      if (candidate && candidate->arg_size() == argCount + 1) {
        ctorFunc = candidate;
      }

      if (ctorFunc) {
        const auto& paramTypes =
            initMethod ? initMethod->paramTypes : std::vector<sun::TypePtr>{};

        std::vector<Value*> ctorArgs =
            generateCtorArgs(alloca, expr.getArgs(), paramTypes);
        ctx.builder->CreateCall(ctorFunc, ctorArgs);
      }

      return alloca;
    }
  }

  // Fallback: mangle from funcName if resolved type didn't have the class name
  std::string mangledName =
      sun::Types::mangleGenericClassName(funcName, resolvedTypeArgs);

  // Look up the specialized class type from the registry
  auto fallbackClassType = typeRegistry->getClass(mangledName);
  if (fallbackClassType) {
    // Create a stack-allocated instance and call constructor
    llvm::StructType* structType =
        fallbackClassType->getStructType(ctx.getContext());
    Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
    AllocaInst* alloca =
        createEntryBlockAlloca(currentFunc, "stack.obj", structType);

    // Zero-initialize
    const DataLayout& DL = module->getDataLayout();
    uint64_t structSize = DL.getTypeAllocSize(structType);
    llvm::FunctionCallee memsetFn = module->getOrInsertFunction(
        "memset", FunctionType::get(PointerType::getUnqual(ctx.getContext()),
                                    {PointerType::getUnqual(ctx.getContext()),
                                     Type::getInt32Ty(ctx.getContext()),
                                     Type::getInt64Ty(ctx.getContext())},
                                    false));
    ctx.builder->CreateCall(
        memsetFn,
        {alloca, ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0),
         ConstantInt::get(Type::getInt64Ty(ctx.getContext()), structSize)});

    // Call constructor (init method) if it exists
    std::string baseCtorName = fallbackClassType->getMangledMethodName("init");
    Function* ctorFunc = nullptr;
    size_t argCount = expr.getArgs().size();

    // Get the init method info from the class type
    const sun::ClassMethod* initMethod = fallbackClassType->getMethod("init");

    // Try to find existing function in module
    Function* candidate = module->getFunction(baseCtorName);

    // If not found but init method exists in class type, create declaration
    if (!candidate && initMethod) {
      std::vector<llvm::Type*> paramLLVMTypes;
      paramLLVMTypes.push_back(
          PointerType::getUnqual(ctx.getContext()));  // this
      for (const auto& paramType : initMethod->paramTypes) {
        paramLLVMTypes.push_back(typeResolver.resolve(paramType));
      }
      FunctionType* funcType = FunctionType::get(
          Type::getVoidTy(ctx.getContext()), paramLLVMTypes, false);
      candidate = Function::Create(funcType, Function::ExternalLinkage,
                                   baseCtorName, module);
    }

    if (candidate && candidate->arg_size() == argCount + 1) {
      ctorFunc = candidate;
    }

    if (ctorFunc) {
      const auto& paramTypes =
          initMethod ? initMethod->paramTypes : std::vector<sun::TypePtr>{};

      std::vector<Value*> ctorArgs =
          generateCtorArgs(alloca, expr.getArgs(), paramTypes);
      ctx.builder->CreateCall(ctorFunc, ctorArgs);
    }

    return alloca;
  }

  logAndThrowError("Unknown generic call: " + funcName);
  return nullptr;
}
