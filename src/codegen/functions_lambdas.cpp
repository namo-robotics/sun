// functions_lambdas.cpp - Function and lambda codegen methods

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"

using namespace llvm;

// -------------------------------------------------------------------
// Closure helpers
// -------------------------------------------------------------------

llvm::LoadInst* CodegenVisitor::createLoadVarFromClosure(
    const std::string& name) {
  // Search from innermost -> outermost closure
  for (auto it = closureStack.rbegin(); it != closureStack.rend(); ++it) {
    auto& closure = *it;

    auto captureIt = closure.captureIndex.find(name);
    if (captureIt == closure.captureIndex.end()) {
      continue;
    }

    unsigned envFieldIndex = captureIt->second;  // index inside env struct
    llvm::Type* captureType = closure.captureTypes[name];
    llvm::Value* envPtr;

    if (closure.isDirectEnv) {
      // Named function with captures: envOrFatPtr is directly the env*
      envPtr = closure.envOrFatPtr;
    } else {
      // Lambda: envOrFatPtr is fat* = { func*, env* }, extract env*
      // Step 1: get pointer to the env* field (offset 1)
      llvm::Value* envPtrPtr = ctx.builder->CreateStructGEP(
          closure.fatType,      // %closure struct type
          closure.envOrFatPtr,  // Value* of type %closure*
          1,                    // field index 1 = env*
          name + ".env.ptr.ptr");

      // Step 2: load the actual env pointer
      envPtr =
          ctx.builder->CreateLoad(llvm::PointerType::getUnqual(closure.envType),
                                  envPtrPtr, name + ".env.ptr");
    }

    // GEP into the env struct to get the variable
    llvm::Value* varPtr = ctx.builder->CreateStructGEP(
        closure.envType,  // %env*
        envPtr,           // env pointer
        envFieldIndex,    // index of this capture inside env
        name + ".ptr");

    // Load the value
    return ctx.builder->CreateLoad(captureType, varPtr, name);
  }

  return nullptr;
}

llvm::StructType* CodegenVisitor::createEnvTypeForFunc(
    const PrototypeAST& proto) {
  std::vector<llvm::Type*> capturedTypes;
  for (const auto& cap : proto.getCaptures()) {
    capturedTypes.push_back(typeResolver.resolve(cap.type));
  }
  return StructType::create(ctx.getContext(), capturedTypes,
                            proto.getName() + ".env");
}

llvm::StructType* CodegenVisitor::createFatTypeForFunc(
    Function* func, llvm::StructType* envType, const PrototypeAST& proto) {
  if (!func) {
    logAndThrowError("null function passed to createFatTypeForFunc");
    return nullptr;
  }

  // The environment struct type: contains only the captured values
  std::vector<llvm::Type*> capturedTypes;
  for (const auto& cap : proto.getCaptures()) {
    capturedTypes.push_back(typeResolver.resolve(cap.type));
  }

  // The actual fat pointer type we return: { function*, environment* }
  std::vector<llvm::Type*> fatElements = {
      PointerType::getUnqual(
          func->getFunctionType()),  // 0 = function pointer (correct signature)
      PointerType::getUnqual(envType)  // 1 = pointer to captured environment
  };

  return StructType::create(ctx.getContext(), fatElements,
                            proto.getName() + ".fat");
}

llvm::Value* CodegenVisitor::createFatClosure(Function* func,
                                              StructType* fatType,
                                              StructType* envType,
                                              const PrototypeAST& proto) {
  Function* parentFunc = ctx.builder->GetInsertBlock()->getParent();
  if (!parentFunc) {
    logAndThrowError("createFatClosure: no current function");
    return nullptr;
  }

  if (!fatType || fatType->getNumElements() != 2) {
    logAndThrowError(
        "createFatClosure: invalid fat type (expected {ptr, ptr})");
    return nullptr;
  }

  std::vector<llvm::Type*> capturedTypes;
  for (const auto& cap : proto.getCaptures()) {
    capturedTypes.push_back(typeResolver.resolve(cap.type));
  }

  IRBuilder<> entryBuilder(&parentFunc->getEntryBlock(),
                           parentFunc->getEntryBlock().begin());

  AllocaInst* envAlloca = entryBuilder.CreateAlloca(
      envType,  // ← we use the envType we just reconstructed
      nullptr, "closure.env");

  for (size_t i = 0; i < proto.getCaptures().size(); ++i) {
    const std::string& varName = proto.getCaptures()[i].name;

    Value* capturedValue = createLoadForLocalVar(varName);
    if (!capturedValue) {
      capturedValue = createLoadVarFromClosure(varName);
    }
    if (!capturedValue) {
      capturedValue = createLoadForGlobalVar(varName);
    }
    if (!capturedValue) {
      logAndThrowError("Cannot capture variable for fat closure: " + varName);
      return nullptr;
    }

    Value* fieldPtr = ctx.builder->CreateStructGEP(
        envType, envAlloca, (unsigned)i, "env." + varName);

    ctx.builder->CreateStore(capturedValue, fieldPtr);
  }

  AllocaInst* fatAlloca = entryBuilder.CreateAlloca(fatType, nullptr, "fatptr");
  ctx.builder->CreateStore(func,
                           ctx.builder->CreateStructGEP(fatType, fatAlloca, 0));
  ctx.builder->CreateStore(envAlloca,
                           ctx.builder->CreateStructGEP(fatType, fatAlloca, 1));
  return fatAlloca;  // now returns %closure*
}

// Create just an env struct for named functions with captures (no fat pointer)
llvm::Value* CodegenVisitor::createEnvClosure(StructType* envType,
                                              const PrototypeAST& proto) {
  Function* parentFunc = ctx.builder->GetInsertBlock()->getParent();
  if (!parentFunc) {
    logAndThrowError("createEnvClosure: no current function");
    return nullptr;
  }

  IRBuilder<> entryBuilder(&parentFunc->getEntryBlock(),
                           parentFunc->getEntryBlock().begin());

  AllocaInst* envAlloca =
      entryBuilder.CreateAlloca(envType, nullptr, proto.getName() + ".env");

  for (size_t i = 0; i < proto.getCaptures().size(); ++i) {
    const std::string& varName = proto.getCaptures()[i].name;

    Value* capturedValue = createLoadForLocalVar(varName);
    if (!capturedValue) {
      capturedValue = createLoadVarFromClosure(varName);
    }
    if (!capturedValue) {
      capturedValue = createLoadForGlobalVar(varName);
    }
    if (!capturedValue) {
      logAndThrowError("Cannot capture variable for env closure: " + varName);
      return nullptr;
    }

    Value* fieldPtr = ctx.builder->CreateStructGEP(
        envType, envAlloca, (unsigned)i, "env." + varName);
    ctx.builder->CreateStore(capturedValue, fieldPtr);
  }

  return envAlloca;  // returns %env*
}

llvm::Constant* CodegenVisitor::createGlobalFatClosure(
    Function* func, StructType* fatType, const PrototypeAST& proto) {
  llvm::Constant* funcConst = func;

  llvm::Type* expectedFuncTy = fatType->getElementType(0);
  if (funcConst->getType() != expectedFuncTy) {
    funcConst = ConstantExpr::getBitCast(func, expectedFuncTy);
  }

  llvm::Constant* nullEnv =
      ConstantPointerNull::get(cast<PointerType>(fatType->getElementType(1)));

  llvm::Constant* constantFat =
      ConstantStruct::get(fatType, {funcConst, nullEnv});
  return constantFat;
}

// -------------------------------------------------------------------
// Prototype codegen
// -------------------------------------------------------------------

std::pair<Function*, llvm::StructType*> CodegenVisitor::codegen(
    const PrototypeAST& proto, llvm::StructType* envType, bool isLambda,
    llvm::Type* returnType) {
  // Generate a unique name for anonymous lambdas
  std::string funcName = proto.getName();
  if (funcName.empty()) {
    funcName = "lambda" + std::to_string(lambdaCounter++);
  } else {
    // Use qualified name from semantic analysis
    funcName = proto.getQualifiedName();
  }

  // Step 1: Reuse any existing forward declaration of this function.
  // The block pre-pass creates declarations for mutual recursion; erasing
  // them would invalidate call instructions already emitted.
  if (Function* existingFunc = module->getFunction(funcName)) {
    if (!existingFunc->empty()) {
      // Already has a body — skip if inside an import scope (duplicate from
      // diamond dependency), otherwise it's a genuine redefinition error.
      if (importScopeDepth > 0) {
        return {existingFunc, nullptr};
      }
      logAndThrowError("Redefinition of function: " + funcName);
    }
    // It's a declaration only — we'll check if its type matches after
    // building the full signature below, and either reuse or replace it.
  }

  // Determine return type: use resolved type from semantic analysis
  if (!returnType) {
    if (proto.hasResolvedReturnType()) {
      returnType = typeResolver.resolveForReturn(proto.getResolvedReturnType());
    } else if (proto.hasReturnType()) {
      logAndThrowError(
          "Function return type not resolved by semantic analysis: " +
          proto.getName());
      return {nullptr, nullptr};
    }
    if (!returnType) {
      logAndThrowError("Unable to determine return type for function: " +
                       proto.getName());
    }
  }

  // Lambdas always use closure calling convention (fat pointer as first arg)
  // Named functions with captures use direct env pointer as first arg
  bool needsClosureArg = isLambda || proto.hasClosure();
  bool useFatPointer = isLambda;  // Only lambdas need fat pointer

  // Build the arg types for the function
  std::vector<Type*> argTypes;

  // Add closure/env pointer as first arg if needed
  if (needsClosureArg) {
    if (useFatPointer) {
      argTypes.push_back(PointerType::getUnqual(typeResolver.getClosureType()));
    } else {
      // Named function with captures: use env* directly
      argTypes.push_back(PointerType::getUnqual(envType));
    }
  }

  // Append the user-visible args from proto
  // Semantic analysis must have resolved all parameter types
  const auto& args = proto.getArgs();
  const auto& resolvedTypes = proto.getResolvedParamTypes();
  bool useResolvedTypes = proto.hasResolvedParamTypes();

  for (size_t i = 0; i < args.size(); i++) {
    llvm::Type* llvmType = nullptr;
    if (useResolvedTypes && i < resolvedTypes.size()) {
      // Use pre-resolved type from semantic analysis
      llvmType = typeResolver.resolve(resolvedTypes[i]);
    } else {
      // Semantic analysis should have resolved this
      const auto& [argName, argType] = args[i];
      logAndThrowError(
          "Function parameter type not resolved by semantic analysis: " +
          proto.getName() + " param " + argName);
      return {nullptr, nullptr};
    }
    argTypes.push_back(llvmType ? llvmType
                                : Type::getDoubleTy(ctx.getContext()));
  }

  // Create the function type
  FunctionType* funcType = FunctionType::get(returnType, argTypes, false);

  // Reuse existing forward declaration if type matches, otherwise replace it
  Function* func = module->getFunction(funcName);
  if (func) {
    if (func->getFunctionType() == funcType) {
      // Type matches — reuse the forward declaration (preserves call uses)
    } else if (func->use_empty()) {
      // Type mismatch but no uses — safe to erase and recreate
      // (e.g., error union functions where pre-pass type differs from actual)
      func->eraseFromParent();
      func = Function::Create(funcType, Function::ExternalLinkage, funcName,
                              module);
    } else {
      logAndThrowError(
          "Forward declaration type mismatch with existing uses: " + funcName);
    }
  } else {
    func =
        Function::Create(funcType, Function::ExternalLinkage, funcName, module);
  }

  // Name the arguments
  unsigned argIdx = 0;
  for (auto& arg : func->args()) {
    if (needsClosureArg && argIdx == 0) {
      arg.setName(useFatPointer ? "fat" : "env");
    } else {
      unsigned userArgIdx = needsClosureArg ? argIdx - 1 : argIdx;
      arg.setName(proto.getArgNames()[userArgIdx]);
    }
    argIdx++;
  }

  return {func, typeResolver.getClosureType()};
}

// -------------------------------------------------------------------
// Function codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenFunc(FunctionAST& funcAst) {
  if (funcAst.getProto().isGeneric()) {
    // Save current insertion point - specialization codegen will change it
    saveInsertPoint();

    // Generate all specializations that were created during semantic analysis
    for (const auto& [mangledName, specializedAST] :
         funcAst.getSpecializations()) {
      if (specializedAST && !module->getFunction(mangledName)) {
        // Recursively generate the specialized function using the same codegen
        codegenFunc(*specializedAST);
      }
    }

    // Restore insertion point so caller can continue
    restoreInsertPoint();

    return nullptr;
  }

  // For extern function declarations, just declare the function (no body)
  if (funcAst.isExtern()) {
    const PrototypeAST& proto = funcAst.getProto();

    // Get return type from prototype (must be resolved by semantic analysis)
    llvm::Type* returnType = nullptr;
    if (proto.hasResolvedReturnType()) {
      returnType = typeResolver.resolve(proto.getResolvedReturnType());
    } else if (proto.hasReturnType()) {
      logAndThrowError(
          "Extern function return type not resolved by semantic analysis: " +
          proto.getName());
      return nullptr;
    } else {
      returnType = llvm::Type::getVoidTy(ctx.getContext());
    }

    // Build parameter types (must be resolved by semantic analysis)
    std::vector<llvm::Type*> paramTypes;
    if (!proto.hasResolvedParamTypes()) {
      logAndThrowError(
          "Extern function parameter types not resolved by semantic "
          "analysis: " +
          proto.getName());
      return nullptr;
    }
    for (const auto& sunType : proto.getResolvedParamTypes()) {
      paramTypes.push_back(typeResolver.resolve(sunType));
    }

    // Create function type
    llvm::FunctionType* funcType =
        llvm::FunctionType::get(returnType, paramTypes, false);

    // Declare external function
    llvm::Function* externFunc = llvm::Function::Create(
        funcType, llvm::Function::ExternalLinkage, proto.getName(), module);

    // Set parameter names
    unsigned idx = 0;
    for (auto& arg : externFunc->args()) {
      if (idx < proto.getArgs().size()) {
        arg.setName(proto.getArgs()[idx].first);
      }
      idx++;
    }

    return externFunc;
  }

  // Get a mutable reference to the prototype to set captures
  PrototypeAST& proto = const_cast<PrototypeAST&>(funcAst.getProto());

  // Named functions use FunctionType (direct call), anonymous use LambdaType
  // (fat pointer)
  bool isNamedFunction = !proto.getName().empty();

  // Use captures already set by semantic analyzer, or compute if not set
  std::vector<Capture> captures = proto.getCaptures();

  if (proto.hasClosure()) {
    // Record closure info for this function (so we know how to call it later)
    FunctionClosureInfo closureInfo;
    closureInfo.captures = captures;
    functionInfo[proto.getName()] = closureInfo;
  }

  // The semantic analyzer should have already inferred and set the return type
  // on the prototype. Error if no return type is available.
  // All return types must be resolved by semantic analysis.
  llvm::Type* returnType = nullptr;
  bool canError = false;
  if (proto.hasResolvedReturnType()) {
    // Use pre-resolved type from semantic analysis
    returnType = typeResolver.resolveForReturn(proto.getResolvedReturnType());
    canError = proto.hasReturnType() && proto.getReturnType()->canError;
  } else if (proto.hasReturnType()) {
    logAndThrowError(
        "Function return type not resolved by semantic analysis: " +
        proto.getName());
    return nullptr;
  }
  if (!returnType) {
    logAndThrowError("Function '" + proto.getName() + "' has no return type. " +
                     "Ensure semantic analysis ran before codegen.");
    return nullptr;
  }

  // If this function can return errors, wrap the return type in an error union
  // struct
  llvm::Type* valueType = returnType;  // Save the underlying value type
  if (canError) {
    // Error union: { i1 isError, T value }
    returnType = llvm::StructType::get(
        ctx.getContext(), {llvm::Type::getInt1Ty(ctx.getContext()), valueType});
  }

  // Create closure env struct type
  auto envType = createEnvTypeForFunc(proto);

  // Generate the function with the correct return type
  // FunctionAST is always a named function (isLambda=false)
  auto [func, fatType] =
      codegen(proto, envType, /*isLambda=*/false, returnType);
  if (!func) {
    logAndThrowError("Failed to create function: " + proto.getName());
  }

  // Always push a scope for function arguments (even for top-level)

  Value* resultPtr = nullptr;
  // Set up closure context if we have captures

  bool isGlobalScope = scopes.empty();

  // For named functions without captures: return the function pointer directly
  // For lambdas: return the fat closure struct
  // For named functions with captures: return just the env pointer
  if (!isNamedFunction) {
    // Lambda - use fat closure
    if (isGlobalScope) {
      resultPtr = createGlobalFatClosure(func, fatType, proto);
    } else {
      resultPtr = createFatClosure(func, fatType, envType, proto);
    }
  } else if (proto.hasClosure()) {
    // Named function with captures - use direct env pointer
    if (isGlobalScope) {
      // TODO: support global named functions with captures if needed
      resultPtr = createGlobalFatClosure(func, fatType, proto);
    } else {
      resultPtr = createEnvClosure(envType, proto);
      // Store the env in scope so call sites can find it
      // Use the mangled function name (proto.getName() has the mangled name for
      // specializations)
      scopes.back().variables[proto.getName()] = cast<AllocaInst>(resultPtr);
    }
  } else {
    // Named function without captures: just return the function pointer
    resultPtr = func;
  }

  // Set up closure context for function body if needed
  if (!isGlobalScope && proto.hasClosure()) {
    // For the closure context used inside the function body, we need the
    // function's first argument (the env/fat pointer passed by caller).
    Value* firstArg = &*func->arg_begin();
    bool isDirectEnv = isNamedFunction;  // Named functions use direct env
    ClosureContext closureCtx;
    closureCtx.fatType = fatType;
    closureCtx.envType = envType;
    closureCtx.envOrFatPtr = firstArg;
    closureCtx.isDirectEnv = isDirectEnv;
    closureCtx.captures = proto.getCaptures();
    // Build the capture index map and get the struct type using stored types
    for (size_t i = 0; i < proto.getCaptures().size(); i++) {
      const auto& cap = proto.getCaptures()[i];
      closureCtx.captureIndex[cap.name] = i;
      llvm::Type* capType = typeResolver.resolve(cap.type);
      closureCtx.captureTypes[cap.name] = capType;
    }

    // Push closure context onto stack
    closureStack.push_back(closureCtx);
  }

  // Create entry block — **this** is where we ultimately want to keep
  // inserting
  BasicBlock* entryBB = BasicBlock::Create(ctx.getContext(), "entry", func);

  // Switch to it
  ctx.builder->SetInsertPoint(entryBB);

  auto& scope = pushScope();
  scope.isFunctionBoundary = true;  // Mark as function entry scope

  // Arguments - skip the closure pointer if present (named functions have it
  // when they have captures)
  bool hasClosureArg = proto.hasClosure();
  unsigned argIdx = 0;
  for (auto& arg : func->args()) {
    if (hasClosureArg && argIdx == 0) {
      // Skip closure pointer - it's handled via closureStack
      argIdx++;
      continue;
    }
    AllocaInst* alloca =
        createEntryBlockAlloca(func, arg.getName().str(), arg.getType());
    ctx.builder->CreateStore(&arg, alloca);
    scope.variables[std::string(arg.getName())] = alloca;
    argIdx++;
  }

  // Set error handling context for this function
  bool savedCanError = currentFunctionCanError;
  llvm::Type* savedValueType = currentFunctionValueType;
  currentFunctionCanError = canError;
  currentFunctionValueType = canError ? valueType : nullptr;

  // Generate body — may recursively create many other functions
  // The body value is used for implicit return if the body doesn't explicitly
  // return
  Value* bodyValue = codegen(funcAst.getBody());

  // Restore error handling context
  currentFunctionCanError = savedCanError;
  currentFunctionValueType = savedValueType;

  // Pop closure context if we pushed one
  if (!isGlobalScope && proto.hasClosure()) {
    closureStack.pop_back();
  }

  // Check if the current basic block needs a terminator
  llvm::BasicBlock* currentBlock = ctx.builder->GetInsertBlock();
  if (!currentBlock->getTerminator()) {
    Type* retType = func->getReturnType();
    if (retType->isVoidTy()) {
      // Void functions get an implicit return
      ctx.builder->CreateRetVoid();
    } else if (bodyValue && bodyValue->getType() == retType) {
      // Non-void functions: use the body's value as implicit return
      ctx.builder->CreateRet(bodyValue);
    } else if (canError && bodyValue && bodyValue->getType() == valueType) {
      // Error-returning function: wrap success value in error union
      Value* successStruct = UndefValue::get(retType);
      successStruct = ctx.builder->CreateInsertValue(
          successStruct, ConstantInt::getFalse(ctx.getContext()), 0);
      successStruct =
          ctx.builder->CreateInsertValue(successStruct, bodyValue, 1);
      ctx.builder->CreateRet(successStruct);
    } else {
      // No usable body value - create unreachable
      // (semantic analyzer should catch missing returns)
      ctx.builder->CreateUnreachable();
    }
  }

  verifyFunction(*func);

  // Track user-defined functions for IR filtering (exclude precompiled library
  // code)
  if (!funcAst.isPrecompiled()) {
    userDefinedFunctions.insert(func->getName().str());
  }

  // Run the optimizer on the function.
  // ctx.fpm->run(*func, *ctx.fam);

  popScope();

  return resultPtr;
}

// -------------------------------------------------------------------
// Lambda codegen (for LambdaAST)
// -------------------------------------------------------------------

llvm::Value* CodegenVisitor::codegenLambda(LambdaAST& lambdaAst) {
  // Save current insertion point - lambda codegen will change it
  saveInsertPoint();

  // Get a mutable reference to the prototype to set captures
  PrototypeAST& proto = const_cast<PrototypeAST&>(lambdaAst.getProto());

  // Use captures already set by semantic analyzer
  std::vector<Capture> captures = proto.getCaptures();

  // The semantic analyzer should have already inferred and set the return type
  llvm::Type* returnType = nullptr;
  bool canError = false;
  if (proto.hasResolvedReturnType()) {
    returnType = typeResolver.resolveForReturn(proto.getResolvedReturnType());
    canError = proto.hasReturnType() && proto.getReturnType()->canError;
  } else if (proto.hasReturnType()) {
    logAndThrowError("Lambda return type not resolved by semantic analysis");
    return nullptr;
  }
  if (!returnType) {
    logAndThrowError("Lambda has no return type.");
    return nullptr;
  }

  // If function can return errors, wrap the return type in an error union
  llvm::Type* valueType = returnType;
  if (canError) {
    returnType = llvm::StructType::get(
        ctx.getContext(), {llvm::Type::getInt1Ty(ctx.getContext()), valueType});
  }

  // Create closure env struct type
  auto envType = createEnvTypeForFunc(proto);

  // Generate the function with the correct return type (isLambda=true)
  auto [func, fatType] = codegen(proto, envType, /*isLambda=*/true, returnType);
  if (!func) {
    logAndThrowError("Failed to create lambda function");
  }

  Value* resultPtr = nullptr;
  bool isGlobalScope = scopes.empty();

  // Lambdas always return a fat closure struct
  if (isGlobalScope) {
    resultPtr = createGlobalFatClosure(func, fatType, proto);
  } else {
    resultPtr = createFatClosure(func, fatType, envType, proto);
  }

  // Set up closure context for function body if needed
  if (!isGlobalScope && proto.hasClosure()) {
    Value* firstArg = &*func->arg_begin();
    ClosureContext closureCtx;
    closureCtx.fatType = fatType;
    closureCtx.envType = envType;
    closureCtx.envOrFatPtr = firstArg;
    closureCtx.isDirectEnv = false;  // Lambdas use fat pointer
    closureCtx.captures = proto.getCaptures();
    for (size_t i = 0; i < proto.getCaptures().size(); i++) {
      const auto& cap = proto.getCaptures()[i];
      closureCtx.captureIndex[cap.name] = i;
      llvm::Type* capType = typeResolver.resolve(cap.type);
      closureCtx.captureTypes[cap.name] = capType;
    }
    closureStack.push_back(closureCtx);
  }

  // Create entry block
  BasicBlock* entryBB = BasicBlock::Create(ctx.getContext(), "entry", func);
  ctx.builder->SetInsertPoint(entryBB);

  auto& scope = pushScope();
  scope.isFunctionBoundary = true;

  // Arguments - skip the closure pointer (lambdas always have it)
  bool hasClosureArg = true;  // Lambdas always have closure arg
  unsigned argIdx = 0;
  for (auto& arg : func->args()) {
    if (hasClosureArg && argIdx == 0) {
      argIdx++;
      continue;
    }
    AllocaInst* alloca =
        createEntryBlockAlloca(func, arg.getName().str(), arg.getType());
    ctx.builder->CreateStore(&arg, alloca);
    scope.variables[std::string(arg.getName())] = alloca;
    argIdx++;
  }

  // Set error handling context for this function
  bool savedCanError = currentFunctionCanError;
  llvm::Type* savedValueType = currentFunctionValueType;
  currentFunctionCanError = canError;
  currentFunctionValueType = canError ? valueType : nullptr;

  // Generate body
  Value* bodyValue = codegen(lambdaAst.getBody());

  // Restore error handling context
  currentFunctionCanError = savedCanError;
  currentFunctionValueType = savedValueType;

  // Pop closure context if we pushed one
  if (!isGlobalScope && proto.hasClosure()) {
    closureStack.pop_back();
  }

  // Check if the current basic block needs a terminator
  llvm::BasicBlock* currentBlock = ctx.builder->GetInsertBlock();
  if (!currentBlock->getTerminator()) {
    llvm::Type* funcRetType = func->getReturnType();
    if (funcRetType->isVoidTy()) {
      ctx.builder->CreateRetVoid();
    } else if (bodyValue && bodyValue->getType() == funcRetType) {
      ctx.builder->CreateRet(bodyValue);
    } else if (canError && bodyValue) {
      // Wrap success value in error union
      llvm::Value* resultStruct = llvm::UndefValue::get(funcRetType);
      resultStruct = ctx.builder->CreateInsertValue(
          resultStruct, ctx.builder->getInt1(false), {0});
      Value* actualBody = bodyValue;
      if (bodyValue->getType()->isPointerTy() && valueType->isStructTy()) {
        actualBody = ctx.builder->CreateLoad(valueType, bodyValue, "load.ret");
      }
      if (actualBody->getType() == valueType) {
        resultStruct =
            ctx.builder->CreateInsertValue(resultStruct, actualBody, {1});
        ctx.builder->CreateRet(resultStruct);
      } else {
        ctx.builder->CreateRet(llvm::UndefValue::get(funcRetType));
      }
    } else {
      ctx.builder->CreateRet(llvm::UndefValue::get(funcRetType));
    }
  }

  // Verify the function
  if (llvm::verifyFunction(*func, &llvm::errs())) {
    logAndThrowError("Lambda function verification failed");
    return nullptr;
  }

  popScope();

  // Restore insertion point so caller can continue generating code
  restoreInsertPoint();

  return resultPtr;
}
