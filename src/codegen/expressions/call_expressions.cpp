// call_expressions.cpp - Call expression codegen methods

#include "ast.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "codegen/support/scalar_ops.h"
#include "semantic_analysis/semantic_scope.h"

using namespace llvm;

namespace ops = sun::codegen::ops;

// -------------------------------------------------------------------
// Helper for unwrapping error union from call results
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// Helper: Apply move semantics for class arguments passed by value
// -------------------------------------------------------------------

Value* CodegenVisitor::applyMoveSemantics(Value* argVal,
                                          sun::TypePtr argSunType) {
  if (!argSunType || !argVal->getType()->isPointerTy()) return argVal;

  // Whatever tracked the source (a constructor temporary, a local) no longer
  // owns it: the value moves to the destination. Its own drop is also made a
  // no-op below (zeroed / tag-poisoned) for sources not tracked here.
  scopes.markClassAllocationAsDeinited(argVal);

  // Payload enums move by loading the storage and poisoning the source tag
  // (never memset: tag 0 is a real variant); a later drop of the source is
  // then a no-op through the drop function's switch default.
  if (isPayloadEnum(argSunType)) {
    auto& enumType = static_cast<sun::EnumType&>(*argSunType);
    llvm::StructType* storageTy = typeResolver.getEnumStorageType(enumType);
    Value* structVal = ctx.builder->CreateLoad(storageTy, argVal, "move.enum");
    Value* tagPtr =
        ctx.builder->CreateStructGEP(storageTy, argVal, 0, "move.tag.ptr");
    ctx.builder->CreateStore(
        ConstantInt::get(Type::getInt32Ty(ctx.getContext()), -1), tagPtr);
    return structVal;
  }

  // Only apply move semantics to class types that are pointers (addressable)
  auto* classType = sun::tryGetType<sun::ClassType>(argSunType);
  if (!classType) return argVal;

  llvm::StructType* structType = classType->getStructType(ctx.getContext());

  // Load the struct value from the source
  Value* structVal = ctx.builder->CreateLoad(structType, argVal, "move.val");

  // Move semantics: zero out the source to prevent double-free
  llvm::FunctionCallee memsetFn = module->getOrInsertFunction(
      "memset", FunctionType::get(PointerType::getUnqual(ctx.getContext()),
                                  {PointerType::getUnqual(ctx.getContext()),
                                   Type::getInt32Ty(ctx.getContext()),
                                   Type::getInt64Ty(ctx.getContext())},
                                  false));
  const DataLayout& DL = module->getDataLayout();
  uint64_t structSize = DL.getTypeAllocSize(structType);
  ctx.builder->CreateCall(
      memsetFn,
      {argVal, ConstantInt::get(Type::getInt32Ty(ctx.getContext()), 0),
       ConstantInt::get(Type::getInt64Ty(ctx.getContext()), structSize)});

  return structVal;
}

// -------------------------------------------------------------------
// Helper: Create interface fat pointer { data_ptr, vtable_ptr }
// -------------------------------------------------------------------

Value* CodegenVisitor::loadClosureForLambdaParam(Value* argVal,
                                                 sun::TypePtr paramType,
                                                 llvm::Type* expectedTy) {
  if (!paramType || !paramType->isLambda() || !argVal) return argVal;
  if (!expectedTy || !expectedTy->isStructTy()) return argVal;
  if (argVal->getType() == expectedTy) return argVal;

  // Lambda literal: alloca holding the closure - load with the callee's type
  if (argVal->getType()->isPointerTy()) {
    return ctx.builder->CreateLoad(expectedTy, argVal, "closure.arg");
  }

  // Closure value under a differently-named (structurally identical) struct
  // type (e.g. local %closure.N vs imported %closure) - rebuild field-wise
  if (argVal->getType()->isStructTy()) {
    Value* fn = ctx.builder->CreateExtractValue(argVal, {0}, "closure.fn");
    Value* env = ctx.builder->CreateExtractValue(argVal, {1}, "closure.env");
    Value* result = UndefValue::get(expectedTy);
    result = ctx.builder->CreateInsertValue(result, fn, {0});
    result = ctx.builder->CreateInsertValue(result, env, {1});
    return result;
  }
  return argVal;
}

// -------------------------------------------------------------------
// Method closure ABI helpers
// Methods take a ptr to { ptr func, ptr env } as their hidden first
// argument; env holds the receiver ('this'). Method bodies must never
// read field 0 (forwarding wrappers pass their own closure through).
// -------------------------------------------------------------------

Value* CodegenVisitor::materializeMethodClosure(Value* fnPtr,
                                                Value* receiverPtr,
                                                StringRef name) {
  llvm::StructType* closureTy = typeResolver.getClosureType();
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* closureAlloca = createEntryBlockAlloca(func, name, closureTy);
  Value* fnSlot =
      ctx.builder->CreateStructGEP(closureTy, closureAlloca, 0, name + ".fn");
  ctx.builder->CreateStore(fnPtr, fnSlot);
  Value* envSlot =
      ctx.builder->CreateStructGEP(closureTy, closureAlloca, 1, name + ".env");
  ctx.builder->CreateStore(receiverPtr, envSlot);
  return closureAlloca;
}

Value* CodegenVisitor::materializeMethodClosureValue(Value* fnPtr,
                                                     Value* receiverPtr) {
  llvm::StructType* closureTy = typeResolver.getClosureType();
  Value* closure = UndefValue::get(closureTy);
  closure = ctx.builder->CreateInsertValue(closure, fnPtr, {0});
  closure = ctx.builder->CreateInsertValue(closure, receiverPtr, {1});
  return closure;
}

// -------------------------------------------------------------------
// Helper: Widen numeric types if needed (i32->i64, f32->f64)
// -------------------------------------------------------------------

// `ref T` is a real type: a ref-returning call yields the referent's ADDRESS,
// which is what makes `var r = v.get(i); r = 5;` write through to the Vec.
// Reading a reference is what loads through it — see loadIfRef, applied where
// an expression's value is actually consumed. Compound referents (classes,
// payload enums) flow as pointers everywhere, so nothing to do for them.
Value* CodegenVisitor::loadIfRef(Value* value, const sun::TypePtr& type) {
  if (!value || !type || !type->isReference()) return value;
  if (!value->getType()->isPointerTy()) return value;  // already a value
  const sun::TypePtr& referenced =
      static_cast<const sun::ReferenceType&>(*type).getReferencedType();
  // Compound referents (classes, payload enums) and interface fat pointers are
  // carried as addresses everywhere; loading one here would make the aliasing
  // copy a borrow exists to avoid, and interface arguments load their own fat
  // pointer at the call. A type parameter means an unsubstituted template body.
  if (!referenced || referenced->isTypeParameter() ||
      referenced->isCompound()) {
    return value;
  }
  llvm::Type* valueTy = typeResolver.resolve(referenced);
  if (!valueTy) return value;
  return ctx.builder->CreateLoad(valueTy, value, "ref.deref");
}

Value* CodegenVisitor::widenNumericIfNeeded(Value* argVal,
                                            const sun::TypePtr& paramType,
                                            const sun::TypePtr& sourceType) {
  return ops::widenNumericIfNeeded(*ctx.builder, typeResolver, argVal,
                                   paramType, sourceType);
}

// -------------------------------------------------------------------
// Helper: static_ptr<T> -> raw_ptr<T> at a call boundary
// -------------------------------------------------------------------

// A static_ptr<T> is a fat { ptr, i64 } value, but a raw_ptr<T> parameter is
// a bare pointer. The type system already treats the two as compatible
// (StaticPointerType::equals, and the overload matcher), so the narrowing has
// to happen here — otherwise the whole struct is passed and the call fails
// verification. This is what lets a string literal reach a C function.
Value* CodegenVisitor::coerceStaticPtrToRawPtr(Value* argVal,
                                               const sun::TypePtr& argSunType,
                                               const sun::TypePtr& paramType) {
  if (!argVal || !argSunType || !paramType) return argVal;
  if (!argSunType->isStaticPointer() || !paramType->isRawPointer()) {
    return argVal;
  }

  // Loaded or constant fat pointer: take the data field.
  if (argVal->getType()->isStructTy()) {
    return ctx.builder->CreateExtractValue(argVal, 0, "static_ptr.data");
  }

  // Still an address of the fat pointer (e.g. an alloca): load field 0.
  if (argVal->getType()->isPointerTy()) {
    llvm::Type* fatTy = typeResolver.resolve(argSunType);
    if (fatTy && fatTy->isStructTy()) {
      Value* dataPtr = ctx.builder->CreateStructGEP(fatTy, argVal, 0,
                                                    "static_ptr.data.addr");
      return ctx.builder->CreateLoad(
          llvm::PointerType::getUnqual(ctx.getContext()), dataPtr,
          "static_ptr.data");
    }
  }

  return argVal;
}

// -------------------------------------------------------------------
// Helper: Materialize struct return value to caller's stack
// -------------------------------------------------------------------

Value* CodegenVisitor::emitMarshalledExternCall(
    const CallExprAST& expr, const std::vector<sun::TypePtr>& paramTypes,
    Function* func) {
  std::vector<sun::cabi::PreparedArg> preparedArgs;
  if (!emitExternArguments(expr, paramTypes, preparedArgs)) return nullptr;
  return externC.emitCall(
      func, preparedArgs,
      [&](llvm::FunctionType* fnTy, Value* callee,
          llvm::ArrayRef<Value*> callArgs) {
        return errors.emitPossiblyThrowingCall(
            fnTy, callee, std::vector<Value*>(callArgs.begin(), callArgs.end()),
            func->hasFnAttribute("sun.canthrow"), "calltmp");
      });
}

Value* CodegenVisitor::materializeStructReturn(Value* callResult) {
  if (!callResult || !callResult->getType()->isStructTy()) {
    return callResult;
  }

  auto* structType = cast<StructType>(callResult->getType());

  // Check that it's not an error union { i1, T } - those should be unwrapped
  // first
  bool isErrorUnion = structType->getNumElements() == 2 &&
                      structType->getElementType(0)->isIntegerTy(1);
  if (isErrorUnion) {
    return callResult;
  }

  // Check that it's not a well-known internal struct type
  // (closure, static_ptr, interface_fat, array_struct)
  if (structType->hasName()) {
    StringRef name = structType->getName();
    for (const auto& info : sun::StructNames::All) {
      if (name == info.name) {
        return callResult;
      }
    }
  }

  // This is a compound type (class) returned by value
  // Store it to the caller's stack and return a pointer for addressability
  Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* resultAlloca =
      createEntryBlockAlloca(currentFunc, "ret.struct", structType);
  ctx.builder->CreateStore(callResult, resultAlloca);
  return resultAlloca;
}

// -------------------------------------------------------------------
// Helper: Prepare argument for reference parameter
// -------------------------------------------------------------------

Value* CodegenVisitor::prepareRefArgument(const ExprAST* argExpr,
                                          sun::TypePtr argSunType,
                                          bool allowTemporaryCopy) {
  // Auto-deref: if argument is raw_ptr<T> and param is ref T, pass the
  // pointer directly
  if (argSunType && argSunType->isRawPointer()) {
    // raw_ptr<T> passed to ref T - pass the pointer value directly
    Value* argVal = codegen(*argExpr);
    return argVal;
  }

  // A reference-typed expression that is not itself an addressable variable
  // (e.g. `_to_ref<T>(ptr)`) already evaluates to the referent's address.
  if (argSunType && argSunType->isReference() &&
      argExpr->getType() != ASTNodeType::VARIABLE_REFERENCE) {
    Value* argVal = codegen(*argExpr);
    if (argVal && argVal->getType()->isPointerTy()) return argVal;
  }

  // Addressable lvalues (variables, fields, array elements, this): pass the
  // genuine storage address, so callee mutations are visible to the caller
  if (Value* addr = tryCodegenAddress(*argExpr)) {
    return addr;
  }

  // Everything below spills a temporary copy - only valid when the argument
  // legitimately has no storage of its own
  if (!allowTemporaryCopy) {
    logAndThrowError(
        "Reference parameter must be passed an addressable expression");
    return nullptr;
  }

  // Array value expression - need to create a temporary alloca
  if (argSunType && argSunType->isArray()) {
    Value* argVal = codegen(*argExpr);
    if (!argVal) return nullptr;
    llvm::StructType* fatType =
        sun::ArrayType::getArrayStructType(ctx.getContext());
    AllocaInst* tempAlloca =
        ctx.builder->CreateAlloca(fatType, nullptr, "arr.temp");
    ctx.builder->CreateStore(argVal, tempAlloca);
    return tempAlloca;
  }

  // Member access with no addressable field (e.g. through a module or a
  // method receiver shape): fall back to the value, spilling if needed
  if (dynamic_cast<const MemberAccessAST*>(argExpr)) {
    Value* val = codegen(*argExpr);
    if (!val) return nullptr;
    if (val->getType()->isPointerTy()) {
      return val;
    }
    AllocaInst* tempAlloca =
        ctx.builder->CreateAlloca(val->getType(), nullptr, "ref.member");
    ctx.builder->CreateStore(val, tempAlloca);
    return tempAlloca;
  }

  // Class temporary passed by ref: create a temporary alloca to hold the value
  // The caller owns this temporary and will deinit it at scope exit.
  // Borrow checker ensures the callee can't escape refs to this temporary.
  if (argSunType && argSunType->isClass()) {
    auto* classType = dynamic_cast<const sun::ClassType*>(argSunType.get());
    if (classType) {
      // Generate the temporary value
      Value* tempVal = codegen(*argExpr);
      if (!tempVal) return nullptr;

      // If codegen returned a pointer, it's already an alloca - use it
      // directly. The original temporary is already tracked for deinit, no need
      // to copy. Copying would cause double-free since both would try to deinit
      // the same owned resources (e.g., Unique<T> pointers).
      if (tempVal->getType()->isPointerTy()) {
        return tempVal;
      }

      // Codegen returned a struct value - need to materialize it in an alloca
      llvm::Type* llvmType = typeResolver.resolve(argSunType);
      AllocaInst* tempAlloca =
          ctx.builder->CreateAlloca(llvmType, nullptr, "ref.temp");
      ctx.builder->CreateStore(tempVal, tempAlloca);

      // Track for cleanup - caller owns the temporary
      auto classTypePtr = sun::tryGetTypePtr<sun::ClassType>(argSunType);
      if (classTypePtr) {
        scopes.trackClassAllocation(tempAlloca, "ref.temp", classTypePtr);
      }

      return tempAlloca;
    }
  }

  // Other temporary expressions - create alloca for the value
  if (argExpr->isTemporary()) {
    Value* tempVal = codegen(*argExpr);
    if (!tempVal) return nullptr;

    AllocaInst* tempAlloca =
        ctx.builder->CreateAlloca(tempVal->getType(), nullptr, "ref.temp");
    ctx.builder->CreateStore(tempVal, tempAlloca);
    return tempAlloca;
  }

  logAndThrowError(
      "Reference parameter must be passed a variable, not an expression");
  return nullptr;
}

// -------------------------------------------------------------------
// Builtin type method dispatch
// Handles: Thread.join(), static_ptr.length()/.raw()
// Returns nullptr if not a builtin type method (caller should continue).
// -------------------------------------------------------------------

Value* CodegenVisitor::extractStaticPtrField(Value* fatPtr, unsigned index,
                                             const sun::TypePtr& staticPtrType,
                                             const char* name) {
  if (fatPtr->getType()->isStructTy()) {
    return ctx.builder->CreateExtractValue(fatPtr, index, name);
  }
  // Still an address of the fat pointer (e.g. an alloca): load the field.
  llvm::Type* fatTy = typeResolver.resolve(staticPtrType);
  Value* fieldAddr = ctx.builder->CreateStructGEP(fatTy, fatPtr, index);
  llvm::Type* fieldTy =
      index == 0 ? static_cast<llvm::Type*>(
                       llvm::PointerType::getUnqual(ctx.getContext()))
                 : llvm::Type::getInt64Ty(ctx.getContext());
  return ctx.builder->CreateLoad(fieldTy, fieldAddr, name);
}

Value* CodegenVisitor::codegenBuiltinTypeMethod(const CallExprAST& expr,
                                                Value* objectPtr,
                                                sun::TypePtr objectType,
                                                const std::string& methodName) {
  if (!objectType) return nullptr;

  // A ref static_ptr<T> receiver was loaded by codegen(); treat it as the
  // fat pointer value.
  if (auto* refType = sun::tryGetType<sun::ReferenceType>(objectType)) {
    sun::TypePtr inner = refType->getReferencedType();
    if (inner && inner->isStaticPointer()) objectType = inner;
  }

  // static_ptr<T>.length() -> i64, static_ptr<T>.raw() -> raw_ptr<T>.
  // A static_ptr<Class> dispatches to the class's own methods instead.
  if (auto* staticPtr = sun::tryGetType<sun::StaticPointerType>(objectType)) {
    sun::TypePtr pointeeType = staticPtr->getPointeeType();
    if (pointeeType && pointeeType->isClass()) return nullptr;

    if (methodName == "length" || methodName == "raw") {
      if (!expr.getArgs().empty()) {
        logAndThrowError("static_ptr." + methodName + "() takes no arguments");
        return nullptr;
      }
      return methodName == "length"
                 ? extractStaticPtrField(objectPtr, 1, objectType,
                                         "static_ptr.length")
                 : extractStaticPtrField(objectPtr, 0, objectType,
                                         "static_ptr.raw");
    }
  }

  // Not a builtin type method
  return nullptr;
}

// -------------------------------------------------------------------
// Module-qualified function call: mymod.foo() -> mymod_foo()
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenModuleFunctionCall(
    const CallExprAST& expr, sun::ModuleType* moduleType,
    const std::string& funcName, const MemberAccessAST& memberAccess) {
  // Use resolved name from semantic analysis (includes library hash prefix)
  std::string qualifiedName;
  if (memberAccess.hasQualifiedName()) {
    qualifiedName = memberAccess.getQualifiedName().mangled();
  } else {
    qualifiedName =
        mangleModulePath(moduleType->getModulePath()) + "_" + funcName;
  }

  // Get or declare the function
  Function* func = functions.lookupCallTarget(qualifiedName);
  if (!func) {
    logAndThrowError("Unknown function: " + qualifiedName);
    return nullptr;
  }

  // Build the argument list through the shared coercion path. Semantic
  // analysis records the resolved overload's signature on the member access;
  // without it every coercion degrades to a no-op, which is what this call
  // path used to do unconditionally.
  std::vector<sun::TypePtr> paramTypes;
  if (auto* calleeType = sun::tryGetType<sun::FunctionType>(memberAccess)) {
    paramTypes = calleeType->getParamTypes();
  }

  // An extern "C" target whose signature needed ABI rewriting must be
  // marshalled, exactly as in codegenFunctionCall — its LLVM parameters no
  // longer line up with the Sun arguments.
  if (externC.needsMarshalling(func)) {
    return emitMarshalledExternCall(expr, paramTypes, func);
  }

  std::vector<Value*> argValues;
  if (!emitCallArguments(expr.getArgs(), expr.getArgConversions(), paramTypes,
                         func->getFunctionType(), argValues, funcName)) {
    return nullptr;
  }

  Value* result =
      errors.emitPossiblyThrowingCall(func->getFunctionType(), func, argValues,
                               func->hasFnAttribute("sun.canthrow"), "calltmp");
  return materializeStructReturn(result);
}

// -------------------------------------------------------------------
// Interface method dispatch via vtable
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenInterfaceMethodCall(
    const CallExprAST& expr, Value* objectPtr, sun::InterfaceType* ifaceType,
    const std::string& methodName) {
  // Get the method info from the interface
  const sun::InterfaceMethod* ifaceMethod = ifaceType->getMethod(methodName);
  if (!ifaceMethod) {
    logAndThrowError("Unknown method: " + methodName + " on interface " +
                     ifaceType->getName());
    return nullptr;
  }

  // Generic interface methods cannot be dispatched via vtable
  if (ifaceMethod->isGeneric()) {
    logAndThrowError(
        "Cannot dynamically dispatch generic method '" + methodName +
        "' on interface type '" + ifaceType->getName() +
        "'. Generic methods require compile-time type information.");
    return nullptr;
  }

  // Get the vtable slot index for this method
  int methodIndex = ifaceType->getMethodIndex(methodName);
  if (methodIndex < 0) {
    logAndThrowError("Method not in vtable: " + methodName + " on interface " +
                     ifaceType->getName());
    return nullptr;
  }

  // Load the fat pointer from objectPtr (which is an alloca to the fat struct)
  llvm::StructType* fatPtrType =
      sun::InterfaceType::getFatPointerType(ctx.getContext());
  Value* fatPtr = ctx.builder->CreateLoad(fatPtrType, objectPtr, "iface.fat");

  // Extract data_ptr (element 0) and vtable_ptr (element 1)
  Value* dataPtr = ctx.builder->CreateExtractValue(fatPtr, 0, "iface.data");
  Value* vtablePtr = ctx.builder->CreateExtractValue(fatPtr, 1, "iface.vtable");

  // GEP into vtable to get the function pointer at the method slot
  llvm::Type* ptrTy = PointerType::getUnqual(ctx.getContext());
  Value* funcPtrSlot = ctx.builder->CreateGEP(
      ptrTy, vtablePtr,
      ConstantInt::get(Type::getInt32Ty(ctx.getContext()), methodIndex),
      "vtable.slot");

  // Load the function pointer from the vtable
  Value* funcPtr = ctx.builder->CreateLoad(ptrTy, funcPtrSlot, "iface.func");

  // Build the function type for the indirect call
  // Parameters: closure ptr, then method params
  std::vector<llvm::Type*> paramTypes;
  paramTypes.push_back(ptrTy);  // closure
  for (const auto& pt : ifaceMethod->paramTypes) {
    paramTypes.push_back(typeResolver.resolve(pt));
  }
  llvm::Type* returnType =
      typeResolver.resolveForReturn(ifaceMethod->returnType);
  llvm::FunctionType* funcType =
      FunctionType::get(returnType, paramTypes, false);

  // Build argument list: method closure with data_ptr as receiver, then
  // user arguments
  std::vector<Value*> argValues;
  argValues.push_back(
      materializeMethodClosure(funcPtr, dataPtr, "iface.closure"));

  if (!emitCallArguments(expr.getArgs(), expr.getArgConversions(),
                         ifaceMethod->paramTypes, funcType, argValues,
                         methodName)) {
    return nullptr;
  }

  // Make the indirect call. Interface-dispatched methods are not currently
  // marked as throwing (InterfaceMethod carries no canThrow), so this does not
  // route through a local landing pad — a limitation only for throwing methods
  // invoked via an interface value, which the stdlib/tests don't exercise.
  Value* result = errors.emitPossiblyThrowingCall(funcType, funcPtr, argValues,
                                           /*canThrow=*/false, "iface.call");
  return materializeStructReturn(result);
}

// -------------------------------------------------------------------
// Class method dispatch (regular and generic methods)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenClassMethodCall(
    const CallExprAST& expr, Value* objectPtr, sun::ClassType* classType,
    const std::string& methodName, const MemberAccessAST* memberAccess) {
  // Semantic analysis must have resolved the method overload and stored
  // its signature in the member access's resolved type (a FunctionType).
  const sun::ClassMethod* method = nullptr;

  if (!memberAccess) {
    logAndThrowError("Internal error: method call without member access AST");
    return nullptr;
  }

  // Use the param types from semantic analysis to find the exact method
  const auto& paramTypes =
      sun::requireType<sun::FunctionType>(
          *memberAccess,
          "method '" + methodName + "' on class " + classType->getDisplayName())
          .getParamTypes();
  method = classType->getMethodForArgs(methodName, paramTypes);

  // Fallback: for generic methods, type parameters won't match concrete args,
  // so look up by name alone
  if (!method) {
    method = classType->getMethod(methodName);
  }

  if (!method) {
    logAndThrowError("Unknown method: " + methodName + " on class " +
                     classType->getDisplayName());
    return nullptr;
  }

  // Handle generic method calls with type arguments
  // e.g., allocator.create<Point>(3, 4)
  if (memberAccess && memberAccess->hasResolvedTypeArgs() &&
      method->isGeneric()) {
    // Call exactly what semantic analysis instantiated. Codegen never spells
    // the name itself: it would have to reproduce the type arguments and the
    // pack suffix, and any drift makes the call reach for a missing symbol.
    if (!memberAccess->hasQualifiedName()) {
      logAndThrowError(
          "Generic method specialization not recorded by semantic analysis: " +
          methodName);
      return nullptr;
    }
    std::string mangledName = memberAccess->getQualifiedName().mangled();

    // Look up the specialized method function
    Function* specializedFunc = module->getFunction(mangledName);
    if (!specializedFunc) {
      logAndThrowError("Generic method specialization not found: " +
                       mangledName);
      return nullptr;
    }

    // Build arguments: method closure first, then user arguments. The
    // parameter types are the specialization's (type arguments substituted),
    // recorded by semantic analysis as the callee's type; a `ref U` parameter
    // takes the argument's address like any other ref parameter.
    std::vector<Value*> argValues;
    argValues.push_back(materializeMethodClosure(specializedFunc, objectPtr));

    const auto& paramTypes =
        sun::requireType<sun::FunctionType>(
            *expr.getCallee(), "generic method call '" + methodName + "'")
            .getParamTypes();
    if (!emitCallArguments(expr.getArgs(), expr.getArgConversions(), paramTypes,
                           specializedFunc->getFunctionType(), argValues,
                           methodName)) {
      return nullptr;
    }

    Value* result = errors.emitPossiblyThrowingCall(specializedFunc->getFunctionType(),
                                             specializedFunc, argValues,
                                             method->canThrow, "method.call");
    return materializeStructReturn(result);
  }

  // Get the mangled method name for regular (non-generic) call
  // Include parameter types for overload disambiguation
  std::string mangledName =
      classType->getMangledMethodName(methodName, method->paramTypes);
  Function* methodFunc = module->getFunction(mangledName);
  if (!methodFunc) {
    logAndThrowError(
        "Method function not found: " + classType->getDisplayName() + "." +
        methodName + " (mangled: " + mangledName + ")");
    return nullptr;
  }

  // Build arguments: method closure first, then user arguments
  std::vector<Value*> argValues;
  argValues.push_back(materializeMethodClosure(methodFunc, objectPtr));

  if (!emitCallArguments(expr.getArgs(), expr.getArgConversions(),
                         method->paramTypes, methodFunc->getFunctionType(),
                         argValues, methodName)) {
    return nullptr;
  }

  // If this is an explicit deinit() call, mark as already deinited
  if (methodName == "deinit") {
    scopes.markClassAllocationAsDeinited(objectPtr);
  }

  Value* result =
      errors.emitPossiblyThrowingCall(methodFunc->getFunctionType(), methodFunc,
                               argValues, method->canThrow, "method.call");
  return materializeStructReturn(result);
}

// -------------------------------------------------------------------
// Top-level method call handler
// Orchestrates: module calls → builtin type methods → interface → class
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenMethodCall(const CallExprAST& expr,
                                         const MemberAccessAST& memberAccess) {
  sun::TypePtr objectType = memberAccess.getObject()->getResolvedType();
  const std::string& methodName = memberAccess.getMemberName();

  // A member access that resolves to a class type is a constructor call only
  // when it reaches through a module, as module-qualified generic class
  // instantiation does (Test.Inner<T>(v)). A method that returns a class —
  // `t.join()` on a Thread<Res> — resolves the same way but is a call.
  if (auto* memberClass = sun::tryGetType<sun::ClassType>(memberAccess)) {
    if (!objectType || objectType->isModule()) {
      return classes.codegenStackClassInstance(expr, methodName, *memberClass);
    }
  }

  // Handle module-qualified function call: mymod.foo()
  if (auto* moduleType = sun::tryGetType<sun::ModuleType>(objectType)) {
    return codegenModuleFunctionCall(expr, moduleType, methodName,
                                     memberAccess);
  }

  // Enum variant construction: EnumName.Variant(args...). Sema resolved the
  // object to the enum type and validated arity/types.
  if (auto* enumType = sun::tryGetType<sun::EnumType>(objectType)) {
    const auto* variant = enumType->getVariant(methodName);
    if (variant && variant->hasPayload()) {
      return codegenEnumVariantConstruction(expr, *enumType, *variant);
    }
    logAndThrowError("Variant '" + methodName + "' of enum '" +
                     enumType->getDisplayName() + "' carries no payload");
  }

  // Handle array.shape() builtin
  if (methodName == "shape" &&
      sun::tryGetType<sun::ArrayType>(sun::unwrapRef(objectType))) {
    if (!expr.getArgs().empty()) {
      logAndThrowError("shape() takes no arguments");
      return nullptr;
    }
    return codegenArrayShape(memberAccess);
  }

  // Generate object pointer
  Value* objectPtr = codegen(*memberAccess.getObject());
  if (!objectPtr) {
    logAndThrowError("Failed to generate object for method call");
    return nullptr;
  }

  // For generic method bodies, 'this' may have a type parameter type.
  // In that case, use the currentClass which is the specialized type.
  if (dynamic_cast<const ThisExprAST*>(memberAccess.getObject()) &&
      currentClass) {
    objectType = currentClass;
  }

  // Try builtin type methods first (Thread.join(), static_ptr.length(), etc.)
  // Note: For pointer-to-class, this returns nullptr to continue with class
  // dispatch
  if (Value* builtinResult =
          codegenBuiltinTypeMethod(expr, objectPtr, objectType, methodName)) {
    return builtinResult;
  }

  // Handle pointer-to-class: unwrap to get the underlying class type
  if (auto* cls =
          sun::tryGetType<sun::ClassType>(sun::getPointeeType(objectType))) {
    auto registeredClass = typeRegistry->getClass(cls->getMangledName());
    if (!registeredClass) {
      logAndThrowError("Class not found in type registry: " +
                       cls->getMangledName());
      return nullptr;
    }
    objectType = registeredClass;
  }

  // Handle reference types - unwrap to get the underlying type
  objectType = sun::unwrapRef(objectType);

  // Handle interface dispatch
  if (auto* ifaceType = sun::tryGetType<sun::InterfaceType>(objectType)) {
    return codegenInterfaceMethodCall(expr, objectPtr, ifaceType, methodName);
  }

  // Must be a class method call
  auto& classType = sun::requireType<sun::ClassType>(
      objectType, "method call receiver", memberAccess.getLocation());

  return codegenClassMethodCall(expr, objectPtr, &classType, methodName,
                                &memberAccess);
}

// -------------------------------------------------------------------
// Call expression dispatch
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const CallExprAST& expr) {
  std::string calleeName = "<call-expression>";

  // Check if this is a method call (MemberAccessAST as callee)
  if (auto* memberAccess =
          dynamic_cast<const MemberAccessAST*>(expr.getCallee())) {
    return scopes.trackCallTemporary(codegenMethodCall(expr, *memberAccess),
                              expr.getResolvedType());
  }

  if (auto* varRef =
          dynamic_cast<const VariableReferenceAST*>(expr.getCallee())) {
    calleeName = varRef->getName();

    // Check for built-in functions (bypass type system)
    if (intrinsics.isBuiltinFunction(calleeName)) {
      return intrinsics.codegenBuiltin(calleeName, expr);
    }

    // Check if this is a stack-allocated class constructor call:
    // ClassName(args...)
    if (auto* calleeClass =
            sun::tryGetType<sun::ClassType>(*expr.getCallee())) {
      return classes.codegenStackClassInstance(expr, calleeName, *calleeClass);
    }
  } else if (auto* qualName =
                 dynamic_cast<const QualifiedNameAST*>(expr.getCallee())) {
    // Qualified name like Math::square - mangle :: to _ for LLVM name
    std::string fullName = qualName->getFullName();
    calleeName = fullName;
    size_t pos;
    while ((pos = calleeName.find("::")) != std::string::npos) {
      calleeName.replace(pos, 2, "_");
    }
  }

  // Get the resolved function type (from semantic analysis)
  sun::TypePtr calleeSunType = expr.getCallee()->getResolvedType();
  if (!calleeSunType || !calleeSunType->isCallable()) {
    logAndThrowError("Callee is not callable: " + calleeName);
    return nullptr;
  }

  // Handle Lambda type: fat pointer call with closure
  Value* result;
  sun::TypePtr calleeReturnType;
  if (auto* lambdaType = sun::tryGetType<sun::LambdaType>(calleeSunType)) {
    result = codegenLambdaCall(expr, calleeName, *lambdaType);
    calleeReturnType = lambdaType->getReturnType();
  } else {
    // Handle Function type: direct call
    const auto& funcType =
        static_cast<const sun::FunctionType&>(*calleeSunType);
    result = codegenFunctionCall(expr, calleeName, funcType);
    calleeReturnType = funcType.getReturnType();
  }

  // Handle array returns: copy data/dims to caller's stack
  // Arrays returned by value have pointers to callee's stack which become
  // dangling after return. Copy to caller's stack to fix this.
  if (auto* arrayType = sun::tryGetType<sun::ArrayType>(expr)) {
    if (result && !arrayType->getDimensions().empty()) {
      result = copyArrayToCallerStack(result, arrayType);
    }
  }

  return scopes.trackCallTemporary(result, expr.getResolvedType());
}

// -------------------------------------------------------------------
// Helper: lower a call's arguments as semantic analysis decided
// -------------------------------------------------------------------

// One argument loop for every kind of call (plain, module-qualified, generic,
// method, lambda, interface, constructor). Semantic analysis recorded an
// ArgConversion per argument; each case below only carries that decision out.
// `paramTypes` supplies the target type where a conversion needs one, and
// `calleeTy` the LLVM parameter types for closure values. `firstArg` skips
// leading arguments the caller has already lowered itself — _spawn's first
// argument is the lambda, which it takes apart rather than passing on.
// Returns false if an argument failed to codegen.
bool CodegenVisitor::emitCallArguments(
    const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<sun::ArgConversion>& conversions,
    const std::vector<sun::TypePtr>& paramTypes, llvm::FunctionType* calleeTy,
    std::vector<Value*>& argValues, const std::string& calleeName,
    size_t firstArg) {
  if (conversions.size() != args.size()) {
    logAndThrowError(
        "Argument conversions not resolved by semantic analysis for call to '" +
        calleeName + "'");
    return false;
  }

  for (size_t i = firstArg; i < args.size(); ++i) {
    const ExprAST* argExpr = args[i].get();
    sun::TypePtr argSunType = argExpr->getResolvedType();
    size_t paramIndex = i - firstArg;
    sun::TypePtr paramType =
        paramIndex < paramTypes.size() ? paramTypes[paramIndex] : nullptr;
    Value* argVal = nullptr;

    switch (conversions[i]) {
      case sun::ArgConversion::Borrow:
        argVal = prepareRefArgument(argExpr, argSunType);
        break;

      case sun::ArgConversion::RawPtrAsRef:
        // The pointer value is the referent's address
        argVal = codegen(*argExpr);
        break;

      case sun::ArgConversion::ClassToRefInterface: {
        Value* classPtr = prepareRefArgument(argExpr, argSunType);
        if (!classPtr) return false;
        argVal = classes.prepareClassForRefInterface(
            classPtr, sun::unwrapRef(argSunType), paramType);
        break;
      }

      case sun::ArgConversion::ClassToInterface: {
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        auto* classType =
            static_cast<sun::ClassType*>(sun::unwrapRef(argSunType).get());
        auto* ifaceType = static_cast<sun::InterfaceType*>(paramType.get());
        argVal = classes.createInterfaceFatPointer(argVal, classType, ifaceType);
        break;
      }

      case sun::ArgConversion::Move:
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        argVal = applyMoveSemantics(argVal, argSunType);
        break;

      case sun::ArgConversion::WidenNumeric:
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        argVal = widenNumericIfNeeded(argVal, paramType, argSunType);
        break;

      case sun::ArgConversion::StaticToRawPtr:
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        argVal = coerceStaticPtrToRawPtr(argVal, argSunType, paramType);
        break;

      case sun::ArgConversion::DerefRawPtr:
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        argVal = ctx.builder->CreateLoad(typeResolver.resolve(paramType),
                                         argVal, "auto_deref_arg");
        break;

      case sun::ArgConversion::CVararg:
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        argVal = externC.promoteVararg(argVal, argSunType);
        break;

      case sun::ArgConversion::PassValue: {
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        sun::TypePtr valueType = sun::unwrapRef(argSunType);
        // Representation only: an interface value is carried as the address
        // of its fat pointer, a lambda literal as the address of its closure;
        // the parameter takes each by value.
        if (valueType && valueType->isInterface() &&
            argVal->getType()->isPointerTy()) {
          llvm::StructType* fatPtrType =
              sun::InterfaceType::getFatPointerType(ctx.getContext());
          argVal =
              ctx.builder->CreateLoad(fatPtrType, argVal, "iface.arg.load");
        }
        unsigned slot = argValues.size();
        llvm::Type* expectedTy = calleeTy && slot < calleeTy->getNumParams()
                                     ? calleeTy->getParamType(slot)
                                     : nullptr;
        argVal = loadClosureForLambdaParam(argVal, valueType, expectedTy);
        break;
      }
    }

    if (!argVal) return false;
    argValues.push_back(argVal);
  }
  return true;
}

// -------------------------------------------------------------------
// Helper: prepare arguments for a call across the C boundary
// -------------------------------------------------------------------

// Generates each argument as a Sun value, carrying out the conversions that
// are Sun's own (taking a `ref` address, numeric widening, static_ptr
// narrowing) as semantic analysis decided them. Everything C-specific —
// aggregate classification, byval copies, sret, and vararg promotions — is
// left to ExternCEmitter, which needs the Sun type alongside the value.
bool CodegenVisitor::emitExternArguments(
    const CallExprAST& expr, const std::vector<sun::TypePtr>& paramTypes,
    std::vector<sun::cabi::PreparedArg>& out) {
  const auto& args = expr.getArgs();
  const auto& conversions = expr.getArgConversions();
  if (conversions.size() != args.size()) {
    logAndThrowError(
        "Argument conversions not resolved by semantic analysis for extern "
        "call");
    return false;
  }

  for (size_t i = 0; i < args.size(); ++i) {
    const ExprAST* argExpr = args[i].get();
    sun::TypePtr paramType = i < paramTypes.size() ? paramTypes[i] : nullptr;
    sun::TypePtr argSunType = argExpr->getResolvedType();

    Value* argVal = nullptr;
    switch (conversions[i]) {
      case sun::ArgConversion::Borrow:
        // `ref T` is C's `T*`: pass the address.
        argVal = prepareRefArgument(argExpr, argSunType);
        break;
      case sun::ArgConversion::WidenNumeric:
        argVal = codegen(*argExpr);
        if (argVal)
          argVal = widenNumericIfNeeded(argVal, paramType, argSunType);
        break;
      case sun::ArgConversion::StaticToRawPtr:
        argVal = codegen(*argExpr);
        if (argVal) {
          argVal = coerceStaticPtrToRawPtr(argVal, argSunType, paramType);
        }
        break;
      default:
        argVal = codegen(*argExpr);
        break;
    }
    if (!argVal) return false;

    out.push_back({argVal, argSunType, paramType});
  }
  return true;
}

// -------------------------------------------------------------------
// Function call codegen (direct call)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenFunctionCall(const CallExprAST& expr,
                                           const std::string& calleeName,
                                           const sun::FunctionType& funcType) {
  // For Function types, we need to:
  // 1. Look up the llvm::Function directly
  // 2. Call it directly without any closure indirection

  // Try to find the function in the module
  llvm::Function* func = nullptr;

  // Check if callee is a variable reference
  if (auto* varRef =
          dynamic_cast<const VariableReferenceAST*>(expr.getCallee())) {
    // Use qualified name from semantic analysis (handles using imports)
    std::string resolvedName = varRef->getMangledName();
    func = functions.lookupCallTarget(resolvedName);
  } else if (auto* qualName =
                 dynamic_cast<const QualifiedNameAST*>(expr.getCallee())) {
    // Qualified name - use the mangled name (calleeName already has :: replaced
    // with _)
    func = functions.lookupCallTarget(calleeName);
  }

  if (!func) {
    // Function not found in module - this happens when calling a function
    // passed as a parameter (e.g., `apply(f: _(i32) i32, x: i32) { return f(x);
    // }`) Load the function pointer from the variable
    Value* funcPtrVal = codegen(*expr.getCallee());
    if (!funcPtrVal) {
      logAndThrowError("Failed to get function pointer for: " + calleeName);
      return nullptr;
    }

    // Get the LLVM function type for the indirect call
    llvm::FunctionType* llvmFuncType =
        typeResolver.resolveDirectFunctionSignature(funcType);

    // Build arguments
    std::vector<Value*> argValues;
    if (!emitCallArguments(expr.getArgs(), expr.getArgConversions(),
                           funcType.getParamTypes(), llvmFuncType, argValues,
                           calleeName)) {
      return nullptr;
    }

    // Indirect call through function pointer
    return errors.emitPossiblyThrowingCall(llvmFuncType, funcPtrVal, argValues,
                                    funcType.canThrow(), "calltmp");
  }

  // Direct call to known function
  std::vector<Value*> argValues;

  // Check if this function has captures (needs closure as first arg). Both
  // the closure info and the environment are keyed by the callee's symbol,
  // which is what the declaration was emitted under.
  if (auto* varRef =
          dynamic_cast<const VariableReferenceAST*>(expr.getCallee())) {
    std::string symbolName = varRef->getMangledName();
    const FunctionClosureInfo* info = functions.closureInfo(symbolName);
    if (info && !info->captures.empty()) {
      if (AllocaInst* closureAlloca = scopes.findVariable(symbolName)) {
        argValues.push_back(closureAlloca);
      } else {
        logAndThrowError("Cannot find closure for function with captures: " +
                         calleeName);
        return nullptr;
      }
    }
  }

  // Get parameter types from the function type
  const auto& paramTypes = funcType.getParamTypes();

  // A C function whose signature needed ABI rewriting cannot go through the
  // normal path: its LLVM parameters no longer line up with the Sun arguments
  // one-to-one. Hand the prepared values to the extern-C emitter instead.
  if (externC.needsMarshalling(func)) {
    return emitMarshalledExternCall(expr, paramTypes, func);
  }

  if (!emitCallArguments(expr.getArgs(), expr.getArgConversions(), paramTypes,
                         func->getFunctionType(), argValues, calleeName)) {
    return nullptr;
  }

  // A throwing callee ('T, IError') is tagged with "sun.canthrow"; inside a try
  // block it must be `invoke`d so its exception routes to the local landing
  // pad. Exceptions now propagate natively — no error-union unwrapping.
  bool canThrow = func->hasFnAttribute("sun.canthrow") || funcType.canThrow();
  Value* callResult = errors.emitPossiblyThrowingCall(func->getFunctionType(), func,
                                               argValues, canThrow, "calltmp");

  // Handle struct return values (classes returned by value)
  return materializeStructReturn(callResult);
}

// -------------------------------------------------------------------
// Lambda call codegen (fat pointer/closure call)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenLambdaCall(const CallExprAST& expr,
                                         const std::string& calleeName,
                                         const sun::LambdaType& lambdaType) {
  // For Lambda types, we need to:
  // 1. Get the closure pointer (fat pointer)
  // 2. Extract the function pointer from the closure
  // 3. Call with the fat pointer as the first argument

  // Try to get the closure pointer directly without loading (avoids redundant
  // alloca)
  Value* closurePtr = nullptr;
  if (auto* varRef =
          dynamic_cast<const VariableReferenceAST*>(expr.getCallee())) {
    // Check local variable (alloca)
    if (AllocaInst* alloca = scopes.findVariable(varRef->getName())) {
      closurePtr = alloca;
    }
    // Check global variable
    else if (GlobalVariable* gv =
                 module->getGlobalVariable(varRef->getName())) {
      closurePtr = gv;
    }
  }

  // If we couldn't get a direct pointer, fall back to loading and storing
  if (!closurePtr) {
    Value* fatPtrVal = codegen(*expr.getCallee());
    if (!fatPtrVal) return nullptr;

    // Create a temporary alloca to hold the closure struct
    Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
    llvm::IRBuilder<> tmpBuilder(&currentFunc->getEntryBlock(),
                                 currentFunc->getEntryBlock().begin());
    AllocaInst* closureAlloca =
        tmpBuilder.CreateAlloca(fatPtrVal->getType(), nullptr, "closure.tmp");
    ctx.builder->CreateStore(fatPtrVal, closureAlloca);
    closurePtr = closureAlloca;
  }

  // Build the LLVM function type using the type resolver
  llvm::FunctionType* llvmFuncType =
      typeResolver.resolveLambdaSignature(lambdaType);

  // Load the closure to extract function pointer
  llvm::Type* closureStructTy = typeResolver.getClosureType();
  Value* loadedClosure =
      ctx.builder->CreateLoad(closureStructTy, closurePtr, "closure.val");
  Value* funcPtr =
      ctx.builder->CreateExtractValue(loadedClosure, {0}, "func.ptr");

  std::vector<Value*> argValues = {closurePtr};

  if (!emitCallArguments(expr.getArgs(), expr.getArgConversions(),
                         lambdaType.getParamTypes(), llvmFuncType, argValues,
                         calleeName)) {
    return nullptr;
  }

  // Indirect call through the extracted function pointer. Throwing lambdas
  // (', IError') are invoked so exceptions route to a local landing pad
  // inside try blocks.
  Value* result = errors.emitPossiblyThrowingCall(llvmFuncType, funcPtr, argValues,
                                           lambdaType.canThrow(), "calltmp");

  // Materialize struct return values for addressability
  return materializeStructReturn(result);
}
