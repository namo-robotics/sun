// functions_lambdas.cpp - Function and lambda codegen methods

#include "ast.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"

using namespace llvm;

// -------------------------------------------------------------------
// Closure helpers
// -------------------------------------------------------------------

// Address of the storage a captured variable refers to: for by-value
// captures the env slot itself (the closure's private copy); for by-ref
// captures, the pointer stored in the slot (the original variable's
// storage). Returns nullptr when name is not a capture of any enclosing
// closure. valueTypeOut receives the capture's value type; byRefOut whether
// the capture was declared [ref name]; ownedOut whether it was listed without
// `ref`, which makes the slot the value's own storage rather than a copy.
llvm::Value* CodegenVisitor::createCaptureSlotAddress(const std::string& name,
                                                      llvm::Type** valueTypeOut,
                                                      bool* byRefOut,
                                                      bool* ownedOut) {
  // Search from innermost -> outermost closure
  for (auto it = closureStack.rbegin(); it != closureStack.rend(); ++it) {
    auto& closure = *it;

    auto captureIt = closure.captureIndex.find(name);
    if (captureIt == closure.captureIndex.end()) {
      continue;
    }

    unsigned envFieldIndex = captureIt->second;  // index inside env struct
    llvm::Value* envPtr;

    if (closure.isDirectEnv) {
      // Named function with captures: envOrFatPtr is directly the env*
      envPtr = closure.envOrFatPtr;
    } else {
      // Lambda: envOrFatPtr is fat* = { func*, env* }, extract env*
      llvm::Value* envPtrPtr = ctx.builder->CreateStructGEP(
          closure.fatType,      // %closure struct type
          closure.envOrFatPtr,  // Value* of type %closure*
          1,                    // field index 1 = env*
          name + ".env.ptr.ptr");
      envPtr =
          ctx.builder->CreateLoad(llvm::PointerType::getUnqual(closure.envType),
                                  envPtrPtr, name + ".env.ptr");
    }

    // GEP into the env struct to get the capture slot
    llvm::Value* slotPtr = ctx.builder->CreateStructGEP(
        closure.envType, envPtr, envFieldIndex, name + ".slot");

    bool byRef = false;
    bool owned = false;
    for (const auto& cap : closure.captures) {
      if (cap.name == name) {
        byRef = cap.kind == CaptureKind::Borrow;
        owned = cap.kind == CaptureKind::Owned;
        break;
      }
    }
    if (valueTypeOut) *valueTypeOut = closure.captureTypes[name];
    if (byRefOut) *byRefOut = byRef;
    if (ownedOut) *ownedOut = owned;

    if (byRef) {
      // The slot holds a pointer to the original storage
      return ctx.builder->CreateLoad(
          llvm::PointerType::getUnqual(ctx.getContext()), slotPtr,
          name + ".ref");
    }
    return slotPtr;
  }

  return nullptr;
}

llvm::LoadInst* CodegenVisitor::createLoadVarFromClosure(
    const std::string& name) {
  llvm::Type* valueType = nullptr;
  llvm::Value* addr = createCaptureSlotAddress(name, &valueType);
  if (!addr) return nullptr;
  return ctx.builder->CreateLoad(valueType, addr, name);
}

// Value stored into an env slot at closure creation: the current value for
// by-value captures, the referent's address for by-ref captures. Owned
// captures of compound values do not come through here — they move, which
// fillCaptureSlots handles.
llvm::Value* CodegenVisitor::computeCaptureInitValue(const Capture& cap) {
  const std::string& varName = cap.name;

  if (cap.kind == CaptureKind::Borrow) {
    if (AllocaInst* alloca = findVariable(varName)) {
      // Ref-typed variables hold a pointer; flatten so the env points at the
      // referent, not the ref cell (mirror tryCodegenAddress)
      if (cap.type && cap.type->isReference()) {
        const auto* refType =
            static_cast<const sun::ReferenceType*>(cap.type.get());
        llvm::Type* referencedLLVMType =
            typeResolver.resolve(refType->getReferencedType());
        if (alloca->getAllocatedType() != referencedLLVMType) {
          return ctx.builder->CreateLoad(
              PointerType::getUnqual(ctx.getContext()), alloca,
              varName + ".ptr");
        }
      }
      return alloca;
    }
    // Nested capture: the enclosing closure's slot. An enclosing by-ref
    // capture propagates the original address (no indirection stacking);
    // by-ref-of-by-value is rejected in semantic analysis.
    if (Value* addr = createCaptureSlotAddress(varName)) {
      return addr;
    }
    logAndThrowError("Cannot capture variable by reference: " + varName);
    return nullptr;
  }

  Value* capturedValue = createLoadForLocalVar(varName);
  if (!capturedValue) {
    capturedValue = createLoadVarFromClosure(varName);
  }
  if (!capturedValue) {
    capturedValue = createLoadForGlobalVar(varName);
  }
  if (!capturedValue) {
    logAndThrowError("Cannot capture variable for closure: " + varName);
  }
  return capturedValue;
}

llvm::StructType* CodegenVisitor::createEnvTypeForFunc(
    const PrototypeAST& proto) {
  std::vector<llvm::Type*> capturedTypes;
  for (const auto& cap : proto.getCaptures()) {
    // By-ref captures store a pointer to the original storage
    capturedTypes.push_back(cap.kind == CaptureKind::Borrow
                                ? PointerType::getUnqual(ctx.getContext())
                                : typeResolver.resolve(cap.type));
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

  IRBuilder<> entryBuilder(&parentFunc->getEntryBlock(),
                           parentFunc->getEntryBlock().begin());

  AllocaInst* envAlloca = entryBuilder.CreateAlloca(
      envType,  // ← we use the envType we just reconstructed
      nullptr, "closure.env");

  if (!fillCaptureSlots(envType, envAlloca, proto, entryBuilder))
    return nullptr;

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

  if (!fillCaptureSlots(envType, envAlloca, proto, entryBuilder))
    return nullptr;

  return envAlloca;  // returns %env*
}

// Fill in a closure environment's capture slots.
//
// A borrowed capture stores the referent's address and a by-value scalar
// stores a copy, both of which the environment merely holds. An owned
// capture of a compound value is different: the value MOVES into the slot,
// which then owns it, so the slot is registered for drop in the scope that
// built the closure. That scope also joins any thread spawned with this
// lambda first — the thread handle is tracked after these slots, and cleanup
// runs in reverse — so the thread has finished before its captures go away.
//
// The slot addresses come from `entryBuilder` rather than the current insert
// point: cleanup for a scope is emitted wherever the scope ends, which the
// block building the closure need not dominate. Entry-block addresses
// dominate the whole function. For the same reason an owned slot is zeroed up
// front, so a path that never reaches the closure still drops something inert.
bool CodegenVisitor::fillCaptureSlots(StructType* envType,
                                      llvm::Value* envAlloca,
                                      const PrototypeAST& proto,
                                      IRBuilder<>& entryBuilder) {
  const DataLayout& DL = module->getDataLayout();

  for (size_t i = 0; i < proto.getCaptures().size(); ++i) {
    const Capture& cap = proto.getCaptures()[i];

    Value* fieldPtr = entryBuilder.CreateStructGEP(
        envType, envAlloca, (unsigned)i, "env." + cap.name);

    // An owned capture of anything a read cannot honestly duplicate — a class
    // or a payload enum — takes the value rather than a copy of it.
    bool movesIn = cap.kind == CaptureKind::Owned && cap.type &&
                   !sun::typeCopiesByRead(cap.type);
    if (!movesIn) {
      Value* capturedValue = computeCaptureInitValue(cap);
      if (!capturedValue) return false;
      ctx.builder->CreateStore(capturedValue, fieldPtr);
      continue;
    }

    llvm::Type* slotType = typeResolver.resolve(cap.type);
    llvm::FunctionCallee memsetFn = module->getOrInsertFunction(
        "memset", FunctionType::get(PointerType::getUnqual(ctx.getContext()),
                                    {PointerType::getUnqual(ctx.getContext()),
                                     Type::getInt32Ty(ctx.getContext()),
                                     Type::getInt64Ty(ctx.getContext())},
                                    false));
    entryBuilder.CreateCall(
        memsetFn,
        {fieldPtr, ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0),
         ConstantInt::get(Type::getInt64Ty(ctx.getContext()),
                          DL.getTypeAllocSize(slotType))});

    // Compounds are carried by address, so the source is the variable's
    // storage; applyMoveSemantics loads it and invalidates the source.
    Value* sourceAddr = findVariable(cap.name);
    if (!sourceAddr) sourceAddr = createCaptureSlotAddress(cap.name);
    if (!sourceAddr) {
      logAndThrowError("Cannot move variable into closure: " + cap.name);
      return false;
    }
    Value* moved = applyMoveSemantics(sourceAddr, cap.type);
    if (!moved) return false;
    ctx.builder->CreateStore(moved, fieldPtr);

    // Register the drop as soon as the slot owns something, so a capture
    // whose neighbour's initializer unwinds is still released.
    trackClassAllocation(fieldPtr, "env." + cap.name, cap.type);
  }
  return true;
}

// -------------------------------------------------------------------
// Prototype codegen
// -------------------------------------------------------------------

std::pair<Function*, llvm::StructType*> CodegenVisitor::codegen(
    const PrototypeAST& proto, llvm::StructType* envType, bool isLambda,
    llvm::Type* returnType) {
  // The qualified name semantic analysis gave it. An anonymous lambda has
  // none to mangle, so give each one its own.
  std::string funcName = proto.getMangledName();
  if (funcName.empty()) {
    funcName = "lambda" + std::to_string(lambdaCounter++);
  }

  // Step 1: Reuse any existing forward declaration of this function.
  // The block pre-pass creates declarations for mutual recursion; erasing
  // them would invalidate call instructions already emitted.
  if (Function* existingFunc = module->getFunction(funcName)) {
    if (!existingFunc->empty()) {
      // Already has a body — this is a genuine redefinition error.
      // Diamond dependency duplicates should have been marked skipCodegen
      // by the semantic analyzer and caught earlier in codegenFunc.
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

  // Append the user-visible args from proto: the fixed parameters, then the
  // elements of any `args...` pack. Semantic analysis must have resolved
  // every one of them.
  std::vector<std::string> argNames = proto.getAllParamNames();
  std::vector<sun::TypePtr> paramTypes = proto.getAllParamTypes();
  if (!proto.hasResolvedParamTypes() || paramTypes.size() != argNames.size()) {
    logAndThrowError(
        "Function parameter types not resolved by semantic analysis: " +
        proto.getName());
    return {nullptr, nullptr};
  }
  for (const auto& sunType : paramTypes) {
    llvm::Type* llvmType = typeResolver.resolve(sunType);
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
      arg.setName(argNames[userArgIdx]);
    }
    argIdx++;
  }

  return {func, typeResolver.getClosureType()};
}

// -------------------------------------------------------------------
// Generic function codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenGenericFunc(FunctionAST& funcAst) {
  // Save current insertion point - specialization codegen will change it
  saveInsertPoint();

  // Generate all specializations that were created during semantic analysis
  for (const auto& [mangledName, specializedAST] :
       funcAst.getSpecializations()) {
    if (!specializedAST) continue;
    // A forward declaration from the block pre-pass still needs its body;
    // only an already-defined function is skipped.
    llvm::Function* existing = module->getFunction(mangledName);
    if (existing && !existing->empty()) continue;
    // Recursively generate the specialized function using the same codegen
    codegenFunc(*specializedAST);
  }

  // Restore insertion point so caller can continue
  restoreInsertPoint();

  return nullptr;
}

// -------------------------------------------------------------------
// Extern function codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenExternFunc(FunctionAST& funcAst) {
  const PrototypeAST& proto = funcAst.getProto();

  // Resolve the signature's Sun types. Everything after this — the C symbol,
  // ABI lowering, byval/sret attributes — belongs to the extern-C boundary
  // and lives in ExternCEmitter.
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

  if (!proto.hasResolvedParamTypes()) {
    logAndThrowError(
        "Extern function parameter types not resolved by semantic "
        "analysis: " +
        proto.getName());
    return nullptr;
  }

  std::vector<llvm::Type*> paramTypes;
  paramTypes.reserve(proto.getResolvedParamTypes().size());
  for (const auto& sunType : proto.getResolvedParamTypes()) {
    paramTypes.push_back(typeResolver.resolve(sunType));
  }

  // A C extern's Sun-side name is module-scoped like any other item, so call
  // sites resolve through a mangled name the C symbol never matches. Guarded:
  // codegenFunc sends every bodyless function here, and a `declare` forward
  // declaration keeps normal Sun mangling.
  if (funcAst.isCExtern()) {
    externC.mapSunName(proto.getMangledName(), proto.getLinkName());
  }
  return externC.declare(proto, returnType, paramTypes);
}

// -------------------------------------------------------------------
// Function signature declaration
// -------------------------------------------------------------------

FuncDeclResult CodegenVisitor::declareFuncSignature(PrototypeAST& proto) {
  // Use captures already set by semantic analyzer
  std::vector<Capture> captures = proto.getCaptures();

  if (proto.hasClosure()) {
    // Record closure info for this function (so we know how to call it later)
    FunctionClosureInfo closureInfo;
    closureInfo.captures = captures;
    // Keyed by the emitted symbol, the same name call sites resolve to.
    functionInfo[proto.getMangledName()] = closureInfo;
  }

  // The semantic analyzer should have already inferred and set the return type
  // on the prototype. Error if no return type is available.
  llvm::Type* returnType = nullptr;
  bool canError = false;
  if (proto.hasResolvedReturnType()) {
    returnType = typeResolver.resolveForReturn(proto.getResolvedReturnType());
    canError = proto.hasReturnType() && proto.getReturnType()->canError;
  } else if (proto.hasReturnType()) {
    logAndThrowError(
        "Function return type not resolved by semantic analysis: " +
        proto.getName());
  }

  if (!returnType) {
    logAndThrowError("Function '" + proto.getName() + "' has no return type. " +
                     "Ensure semantic analysis ran before codegen.");
  }

  // With native LLVM exceptions, a throwing function ('T, IError') returns a
  // plain T — the ', IError' marker only means the function may unwind. We no
  // longer wrap the return type in an error-union struct.
  llvm::Type* valueType = returnType;

  // Create closure env struct type
  auto envType = createEnvTypeForFunc(proto);

  // Generate the function with the correct return type
  auto [func, fatType] =
      codegen(proto, envType, /*isLambda=*/false, returnType);
  if (!func) {
    logAndThrowError("Failed to create function: " + proto.getName());
  }

  // Tag throwing functions so direct call sites know to emit `invoke` when
  // inside a try block. This attribute survives CloneModule into the JIT.
  if (canError) {
    func->addFnAttr("sun.canthrow");
  }

  return {func, fatType, envType, returnType, valueType, canError};
}

// -------------------------------------------------------------------
// Function codegen
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenFunc(FunctionAST& funcAst) {
  if (funcAst.shouldSkipCodegen()) return nullptr;

  if (funcAst.getProto().isTemplate()) {
    return codegenGenericFunc(funcAst);
  }

  if (funcAst.isExtern()) {
    return codegenExternFunc(funcAst);
  }

  // Precompiled functions are linked from bitcode — skip codegen
  if (funcAst.isPrecompiled()) {
    return nullptr;
  }

  // Get a mutable reference to the prototype to set captures
  PrototypeAST& proto = const_cast<PrototypeAST&>(funcAst.getProto());

  // Declare the function signature and resolve types
  auto decl = declareFuncSignature(proto);
  if (!decl.func) return nullptr;

  auto* func = decl.func;
  auto* fatType = decl.fatType;
  auto* envType = decl.envType;
  auto* returnType = decl.returnType;
  auto* valueType = decl.valueType;
  bool canError = decl.canError;

  // Always push a scope for function arguments (even for top-level)

  Value* resultPtr = nullptr;
  bool isGlobalScope = scopes.empty();

  if (proto.hasClosure()) {
    if (isGlobalScope) {
      logAndThrowError(
          "Global named functions with captures are not supported: " +
          proto.getName());
    }
    resultPtr = createEnvClosure(envType, proto);
    // Keyed by the emitted symbol: a specialization's callers name it by its
    // mangled name, never by the template's.
    scopes.back().variables[proto.getMangledName()] =
        cast<AllocaInst>(resultPtr);
  } else {
    resultPtr = func;
  }

  if (proto.hasClosure()) {
    Value* firstArg = &*func->arg_begin();
    ClosureContext closureCtx;
    closureCtx.fatType = fatType;
    closureCtx.envType = envType;
    closureCtx.envOrFatPtr = firstArg;
    closureCtx.isDirectEnv = true;
    closureCtx.captures = proto.getCaptures();
    for (size_t i = 0; i < proto.getCaptures().size(); i++) {
      const auto& cap = proto.getCaptures()[i];
      closureCtx.captureIndex[cap.name] = i;
      closureCtx.captureTypes[cap.name] = typeResolver.resolve(cap.type);
    }
    closureStack.push_back(closureCtx);
  }

  // Create entry block — **this** is where we ultimately want to keep
  // inserting
  BasicBlock* entryBB = BasicBlock::Create(ctx.getContext(), "entry", func);

  // Switch to it
  ctx.builder->SetInsertPoint(entryBB);

  debugInfo.enterFunction(*ctx.builder, func, proto.getName(),
                          proto.getLocation());

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
    debugDeclareParam(alloca, arg.getName().str(), proto,
                      hasClosureArg ? argIdx - 1 : argIdx);
    argIdx++;
  }

  // Set error handling context for this function
  bool savedCanError = currentFunctionCanError;
  llvm::Type* savedValueType = currentFunctionValueType;
  bool savedReturnsRef = currentFunctionReturnsRef;
  currentFunctionCanError = canError;
  currentFunctionValueType = canError ? valueType : nullptr;
  currentFunctionReturnsRef =
      proto.hasReturnType() && proto.getReturnType()->isReference();

  // Generate body — may recursively create many other functions
  // The body value is used for implicit return if the body doesn't explicitly
  // return
  Value* bodyValue = codegen(funcAst.getBody());

  // Restore error handling context
  currentFunctionCanError = savedCanError;
  currentFunctionValueType = savedValueType;
  currentFunctionReturnsRef = savedReturnsRef;

  // Pop closure context if we pushed one
  if (proto.hasClosure()) {
    closureStack.pop_back();
  }

  // Check if the current basic block needs a terminator
  llvm::BasicBlock* currentBlock = ctx.builder->GetInsertBlock();
  if (!currentBlock->getTerminator()) {
    // Emit scope cleanup before implicit return (deinit classes, free ptrs)
    emitScopeCleanup();

    Type* retType = func->getReturnType();
    if (retType->isVoidTy()) {
      // Void functions get an implicit return
      ctx.builder->CreateRetVoid();
    } else if (canError && !valueType) {
      // void, IError function: return { i1 = false } to indicate success
      Value* successStruct = UndefValue::get(retType);
      successStruct = ctx.builder->CreateInsertValue(
          successStruct, ConstantInt::getFalse(ctx.getContext()), 0);
      ctx.builder->CreateRet(successStruct);
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

  debugInfo.exitFunction(func);

  if (llvm::verifyFunction(*func, &llvm::errs())) {
    logAndThrowError("Function verification failed: " + func->getName().str());
  }

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

  // With native LLVM exceptions, a throwing lambda ('T, IError') returns a
  // plain T — the ', IError' marker only means the lambda may unwind.
  llvm::Type* valueType = returnType;

  // Create closure env struct type
  auto envType = createEnvTypeForFunc(proto);

  // Generate the function with the correct return type (isLambda=true)
  auto [func, fatType] = codegen(proto, envType, /*isLambda=*/true, returnType);
  if (!func) {
    logAndThrowError("Failed to create lambda function");
  }

  // Tag throwing lambdas so call sites emit `invoke` inside try blocks
  if (canError) {
    func->addFnAttr("sun.canthrow");
  }

  Value* resultPtr = nullptr;
  if (scopes.empty()) {
    if (proto.hasClosure()) {
      logAndThrowError(
          "Lambdas at global scope with local captures are not supported");
    }
    // Global scope: create a constant fat pointer { funcPtr, null }
    llvm::Constant* funcConst = func;
    llvm::Type* expectedFuncTy = fatType->getElementType(0);
    if (funcConst->getType() != expectedFuncTy) {
      funcConst = ConstantExpr::getBitCast(func, expectedFuncTy);
    }
    llvm::Constant* nullEnv =
        ConstantPointerNull::get(cast<PointerType>(fatType->getElementType(1)));
    resultPtr = ConstantStruct::get(fatType, {funcConst, nullEnv});
  } else {
    resultPtr = createFatClosure(func, fatType, envType, proto);
  }

  // Set up closure context for function body if needed
  if (proto.hasClosure()) {
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
      closureCtx.captureTypes[cap.name] = typeResolver.resolve(cap.type);
    }
    closureStack.push_back(closureCtx);
  }

  // Create entry block
  BasicBlock* entryBB = BasicBlock::Create(ctx.getContext(), "entry", func);
  ctx.builder->SetInsertPoint(entryBB);

  debugInfo.enterFunction(*ctx.builder, func, "lambda", proto.getLocation());

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
    debugDeclareParam(alloca, arg.getName().str(), proto, argIdx - 1);
    argIdx++;
  }

  // Set error handling context for this function
  bool savedCanError = currentFunctionCanError;
  llvm::Type* savedValueType = currentFunctionValueType;
  bool savedReturnsRef = currentFunctionReturnsRef;
  currentFunctionCanError = canError;
  currentFunctionValueType = canError ? valueType : nullptr;
  currentFunctionReturnsRef =
      proto.hasReturnType() && proto.getReturnType()->isReference();

  // Generate body
  Value* bodyValue = codegen(lambdaAst.getBody());

  // Restore error handling context
  currentFunctionCanError = savedCanError;
  currentFunctionValueType = savedValueType;
  currentFunctionReturnsRef = savedReturnsRef;

  // Pop closure context if we pushed one
  if (proto.hasClosure()) {
    closureStack.pop_back();
  }

  // Check if the current basic block needs a terminator
  llvm::BasicBlock* currentBlock = ctx.builder->GetInsertBlock();
  if (!currentBlock->getTerminator()) {
    // Emit scope cleanup before implicit return (deinit classes, free ptrs)
    emitScopeCleanup();

    llvm::Type* funcRetType = func->getReturnType();
    if (funcRetType->isVoidTy()) {
      ctx.builder->CreateRetVoid();
    } else if (bodyValue && bodyValue->getType() == funcRetType) {
      ctx.builder->CreateRet(bodyValue);
    } else if (bodyValue && bodyValue->getType()->isPointerTy() &&
               funcRetType->isStructTy()) {
      // Body produced a pointer to a struct value (e.g. class return)
      Value* loaded =
          ctx.builder->CreateLoad(funcRetType, bodyValue, "load.ret");
      ctx.builder->CreateRet(loaded);
    } else {
      ctx.builder->CreateRet(llvm::UndefValue::get(funcRetType));
    }
  }

  debugInfo.exitFunction(func);

  // Verify the function
  if (llvm::verifyFunction(*func, &llvm::errs())) {
    logAndThrowError("Lambda function verification failed");
    return nullptr;
  }

  popScope();

  // Restore insertion point so caller can continue generating code
  restoreInsertPoint();
  // The lambda's debug location must not leak into the enclosing function
  debugInfo.clearLocation(*ctx.builder);

  return resultPtr;
}
