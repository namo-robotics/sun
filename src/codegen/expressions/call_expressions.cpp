// call_expressions.cpp - Call expression codegen methods

#include "ast.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "codegen/support/scalar_ops.h"
#include "semantic_analysis/semantic_scope.h"

using sun::semantic_analysis::ArgConversion;
using sun::types::ClassType;
using sun::types::InterfaceType;
using sun::types::ReferenceType;
using sun::types::TypePtr;

using sun::ast::CallExprAST;
using sun::ast::ExprAST;
using sun::ast::MemberAccessAST;
using sun::support::logAndThrowError;

using namespace llvm;

/** Translates analyzed Sun programs into LLVM instructions. */
namespace sun::codegen {

// -------------------------------------------------------------------
// Helper for unwrapping error union from call results
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// Helper: Apply move semantics for class arguments passed by value
// -------------------------------------------------------------------

Value* CodegenVisitor::applyMoveSemantics(Value* argVal, TypePtr argSunType) {
  if (!argSunType || !argVal->getType()->isPointerTy()) return argVal;

  // Transfer ownership away from the source. Callers using raw storage must
  // also exclude the extracted slot from future cleanup.
  scopes.markClassAllocationAsDeinited(argVal, argSunType);

  // Payload enums move by loading the storage and poisoning the source tag
  // (never memset: tag 0 is a real variant); a later drop of the source is
  // then a no-op through the drop function's switch default.
  if (isPayloadEnum(argSunType)) {
    auto& enumType = static_cast<sun::types::EnumType&>(*argSunType);
    llvm::StructType* storageTy = typeResolver.getEnumStorageType(enumType);
    Value* structVal = ctx.builder->CreateLoad(storageTy, argVal, "move.enum");
    Value* tagPtr =
        ctx.builder->CreateStructGEP(storageTy, argVal, 0, "move.tag.ptr");
    ctx.builder->CreateStore(
        ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), -1), tagPtr);
    return structVal;
  }

  // Interface values move their two-pointer owner handle as one value. Clearing
  // the source makes any later scope drop a no-op.
  if (argSunType->isInterface()) {
    StructType* fatType = InterfaceType::getFatPointerType(ctx.getContext());
    Value* fat = ctx.builder->CreateLoad(fatType, argVal, "move.interface");
    ctx.builder->CreateStore(Constant::getNullValue(fatType), argVal);
    return fat;
  }

  // A sized array moves its inline storage: load it, then zero the source
  // when its elements own anything, so the source's drop releases nothing
  if (auto* arrayType =
          sun::codegen::support::tryGetType<sun::types::ArrayType>(
              argSunType)) {
    if (arrayType->isUnsized()) return argVal;
    llvm::Type* storageType = arrayType->getDataStorageType(ctx.getContext());
    Value* storageVal =
        ctx.builder->CreateLoad(storageType, argVal, "move.array");
    if (sun::types::typeNeedsDrop(argSunType)) {
      const DataLayout& DL = module->getDataLayout();
      ctx.builder->CreateMemSet(
          argVal, ConstantInt::get(llvm::Type::getInt8Ty(ctx.getContext()), 0),
          DL.getTypeAllocSize(storageType), DL.getABITypeAlign(storageType));
    }
    return storageVal;
  }

  // Only apply move semantics to class types that are pointers (addressable)
  auto* classType = sun::codegen::support::tryGetType<ClassType>(argSunType);
  if (!classType) return argVal;

  llvm::StructType* structType = classType->getStructType(ctx.getContext());

  // Load the struct value from the source
  Value* structVal = ctx.builder->CreateLoad(structType, argVal, "move.val");

  // Clear stale contents after transferring ownership.
  llvm::FunctionCallee memsetFn = module->getOrInsertFunction(
      "memset",
      llvm::FunctionType::get(PointerType::getUnqual(ctx.getContext()),
                              {PointerType::getUnqual(ctx.getContext()),
                               llvm::Type::getInt32Ty(ctx.getContext()),
                               llvm::Type::getInt64Ty(ctx.getContext())},
                              false));
  const DataLayout& DL = module->getDataLayout();
  uint64_t structSize = DL.getTypeAllocSize(structType);
  ctx.builder->CreateCall(
      memsetFn,
      {argVal, ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0),
       ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()), structSize)});

  return structVal;
}

// -------------------------------------------------------------------
// Helper: Create interface fat pointer { data_ptr, vtable_ptr }
// -------------------------------------------------------------------

Value* CodegenVisitor::loadClosureForLambdaParam(Value* argVal,
                                                 TypePtr paramType,
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
Value* CodegenVisitor::loadIfRef(Value* value, const TypePtr& type) {
  if (!value || !type || !type->isReference()) return value;
  if (!value->getType()->isPointerTy()) return value;  // already a value
  const TypePtr& referenced =
      static_cast<const ReferenceType&>(*type).getReferencedType();
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
                                            const TypePtr& paramType,
                                            const TypePtr& sourceType) {
  return support::widenNumericIfNeeded(*ctx.builder, typeResolver, argVal,
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
                                               const TypePtr& argSunType,
                                               const TypePtr& paramType) {
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
    const CallExprAST& expr, const std::vector<TypePtr>& paramTypes,
    Function* func) {
  std::vector<sun::codegen::abi::PreparedArg> preparedArgs;
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
  if (!callResult) return callResult;

  // A sized array comes back as its inline storage by value; give it a
  // home on this frame so it can be indexed, moved and dropped by address
  if (callResult->getType()->isArrayTy()) {
    Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
    AllocaInst* resultAlloca =
        createEntryBlockAlloca(currentFunc, "ret.array", callResult->getType());
    ctx.builder->CreateStore(callResult, resultAlloca);
    return resultAlloca;
  }

  if (!callResult->getType()->isStructTy()) {
    return callResult;
  }

  auto* structType = cast<StructType>(callResult->getType());

  // Non-owning internal structs stay as values. An owning interface return is
  // materialized so normal compound move and drop tracking can address it.
  if (structType->hasName()) {
    StringRef name = structType->getName();
    for (const auto& info : sun::semantic_analysis::All) {
      if (name == info.name) {
        if (name == sun::semantic_analysis::InterfaceFat) break;
        return callResult;
      }
    }
  }

  // This is an owning compound type returned by value.
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
                                          TypePtr argSunType,
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
      argExpr->getType() != sun::ast::ASTNodeType::VARIABLE_REFERENCE) {
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

  // A sized array temporary (a literal, a returned array) already sits in
  // storage of its own: its address is the argument
  if (argSunType && argSunType->isArray()) {
    Value* argVal = codegen(*argExpr);
    if (!argVal) return nullptr;
    if (argVal->getType()->isPointerTy()) return argVal;
    AllocaInst* tempAlloca =
        ctx.builder->CreateAlloca(argVal->getType(), nullptr, "arr.temp");
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
    auto* classType = dynamic_cast<const ClassType*>(argSunType.get());
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
      auto classTypePtr =
          sun::codegen::support::tryGetTypePtr<ClassType>(argSunType);
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
                                             const TypePtr& staticPtrType,
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
                                                TypePtr objectType,
                                                const std::string& methodName) {
  if (!objectType) return nullptr;

  // A ref static_ptr<T> receiver was loaded by codegen(); treat it as the
  // fat pointer value.
  if (auto* refType =
          sun::codegen::support::tryGetType<ReferenceType>(objectType)) {
    TypePtr inner = refType->getReferencedType();
    if (inner && inner->isStaticPointer()) objectType = inner;
  }

  // static_ptr<T>.length() -> i64, static_ptr<T>.raw() -> raw_ptr<T>.
  // A static_ptr<Class> dispatches to the class's own methods instead.
  if (auto* staticPtr =
          sun::codegen::support::tryGetType<sun::types::StaticPointerType>(
              objectType)) {
    TypePtr pointeeType = staticPtr->getPointeeType();
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
    const CallExprAST& expr, const std::string& funcName,
    const MemberAccessAST& memberAccess) {
  Function* func =
      functions.lookupFunctionById(memberAccess.getTargetDeclarationId());

  // Build the argument list through the shared coercion path. Semantic
  // analysis records the resolved overload's signature on the member access;
  // without it every coercion degrades to a no-op, which is what this call
  // path used to do unconditionally.
  std::vector<TypePtr> paramTypes;
  if (auto* calleeType =
          sun::codegen::support::tryGetType<sun::types::FunctionType>(
              memberAccess)) {
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

  Value* result = errors.emitPossiblyThrowingCall(
      func->getFunctionType(), func, argValues,
      func->hasFnAttribute("sun.canthrow"), "calltmp");
  return materializeStructReturn(result);
}

// -------------------------------------------------------------------
// Interface method dispatch via vtable
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenInterfaceMethodCall(
    const CallExprAST& expr, Value* objectPtr, InterfaceType* ifaceType,
    const std::string& methodName) {
  const auto& signature =
      sun::codegen::support::requireType<sun::types::FunctionType>(
          *expr.getCallee(), "interface method call");
  int methodIndex =
      ifaceType->getMethodIndex(expr.getCallee()->getTargetDeclarationId());
  if (methodIndex < 0) {
    logAndThrowError("Method not in vtable: " + methodName + " on interface " +
                     ifaceType->getName());
    return nullptr;
  }

  // Load the fat pointer from objectPtr (which is an alloca to the fat struct)
  llvm::StructType* fatPtrType =
      InterfaceType::getFatPointerType(ctx.getContext());
  Value* fatPtr = ctx.builder->CreateLoad(fatPtrType, objectPtr, "iface.fat");

  // Extract data_ptr (element 0) and vtable_ptr (element 1)
  Value* dataPtr = ctx.builder->CreateExtractValue(fatPtr, 0, "iface.data");
  Value* vtablePtr = ctx.builder->CreateExtractValue(fatPtr, 1, "iface.vtable");

  // GEP into vtable to get the function pointer at the method slot
  llvm::Type* ptrTy = PointerType::getUnqual(ctx.getContext());
  Value* funcPtrSlot = ctx.builder->CreateGEP(
      ptrTy, vtablePtr,
      ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), methodIndex),
      "vtable.slot");

  // Load the function pointer from the vtable
  Value* funcPtr = ctx.builder->CreateLoad(ptrTy, funcPtrSlot, "iface.func");

  // Build the function type for the indirect call
  // Parameters: closure ptr, then method params
  std::vector<llvm::Type*> paramTypes;
  paramTypes.push_back(ptrTy);  // closure
  for (const auto& pt : signature.getParamTypes()) {
    paramTypes.push_back(typeResolver.resolve(pt));
  }
  llvm::Type* returnType =
      typeResolver.resolveForReturn(signature.getReturnType());
  llvm::FunctionType* funcType =
      llvm::FunctionType::get(returnType, paramTypes, false);

  // Build argument list: method closure with data_ptr as receiver, then
  // user arguments
  std::vector<Value*> argValues;
  argValues.push_back(
      materializeMethodClosure(funcPtr, dataPtr, "iface.closure"));

  if (!emitCallArguments(expr.getArgs(), expr.getArgConversions(),
                         signature.getParamTypes(), funcType, argValues,
                         methodName)) {
    return nullptr;
  }

  // Make the indirect call. Interface-dispatched methods are not currently
  // marked as throwing (InterfaceMethod carries no canThrow), so this does not
  // route through a local landing pad — a limitation only for throwing methods
  // invoked via an interface value, which the stdlib/tests don't exercise.
  Value* result =
      errors.emitPossiblyThrowingCall(funcType, funcPtr, argValues,
                                      /*canThrow=*/false, "iface.call");
  return materializeStructReturn(result);
}

// -------------------------------------------------------------------
// Class method dispatch (regular and generic methods)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenClassMethodCall(
    const CallExprAST& expr, Value* objectPtr, const std::string& methodName,
    const MemberAccessAST* memberAccess) {
  if (!memberAccess)
    logAndThrowError("Internal error: method call without member access AST");
  const auto& signature =
      sun::codegen::support::requireType<sun::types::FunctionType>(
          *memberAccess, "method '" + methodName + "'");
  Function* methodFunc =
      functions.lookupFunctionById(memberAccess->getTargetDeclarationId());
  std::vector<Value*> argValues;
  argValues.push_back(materializeMethodClosure(methodFunc, objectPtr));
  if (!emitCallArguments(
          expr.getArgs(), expr.getArgConversions(), signature.getParamTypes(),
          methodFunc->getFunctionType(), argValues, methodName)) {
    return nullptr;
  }
  if (methodName == "deinit") {
    scopes.markClassAllocationAsDeinited(objectPtr);
  }
  Value* result = errors.emitPossiblyThrowingCall(
      methodFunc->getFunctionType(), methodFunc, argValues,
      signature.canThrow(), "method.call");
  return materializeStructReturn(result);
}

// -------------------------------------------------------------------
// Top-level method call handler
// Orchestrates: module calls → builtin type methods → interface → class
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenMethodCall(const CallExprAST& expr,
                                         const MemberAccessAST& memberAccess) {
  TypePtr objectType = memberAccess.getObject()->getResolvedType();
  const std::string& methodName = memberAccess.getMemberName();

  // A member access that resolves to a class type is a constructor call only
  // when it reaches through a module, as module-qualified generic class
  // instantiation does (Test.Inner<T>(v)). A method that returns a class —
  // `t.join()` on a Thread<Res> — resolves the same way but is a call.
  if (auto* memberClass =
          sun::codegen::support::tryGetType<ClassType>(memberAccess)) {
    if (!objectType || objectType->isModule()) {
      return classes.codegenStackClassInstance(expr, *memberClass);
    }
  }

  // Handle module-qualified function call: mymod.foo()
  if (auto* moduleType =
          sun::codegen::support::tryGetType<sun::types::ModuleType>(
              objectType)) {
    return codegenModuleFunctionCall(expr, methodName, memberAccess);
  }

  // Enum variant construction: EnumName.Variant(args...). Sema resolved the
  // object to the enum type and validated arity/types.
  if (auto* enumType =
          sun::codegen::support::tryGetType<sun::types::EnumType>(objectType)) {
    const auto* variant = enumType->getVariant(methodName);
    if (variant && variant->hasPayload()) {
      return enums.codegenVariantConstruction(expr, *enumType, *variant);
    }
    logAndThrowError("Variant '" + methodName + "' of enum '" +
                     enumType->getDisplayName() + "' carries no payload");
  }

  // arr.ndims() and arr.dim(i) on arrays and views
  if ((methodName == "ndims" || methodName == "dim") &&
      sun::codegen::support::tryGetType<sun::types::ArrayType>(
          sun::types::unwrapRef(objectType))) {
    return codegenArrayQuery(expr, memberAccess);
  }

  // Generate object pointer
  Value* objectPtr = codegen(*memberAccess.getObject());
  if (!objectPtr) {
    logAndThrowError("Failed to generate object for method call");
    return nullptr;
  }

  // For generic method bodies, 'this' may have a type parameter type.
  // In that case, use the currentClass which is the specialized type.
  if (dynamic_cast<const sun::ast::ThisExprAST*>(memberAccess.getObject()) &&
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
  if (auto* cls = sun::codegen::support::tryGetType<ClassType>(
          sun::codegen::support::getPointeeType(objectType))) {
    auto registeredClass = typeRegistry->getClass(cls->getDeclarationId());
    if (!registeredClass) {
      logAndThrowError("Class not found in type registry: " +
                       cls->getDisplayName());
      return nullptr;
    }
    objectType = registeredClass;
  }

  // Handle reference types - unwrap to get the underlying type
  objectType = sun::types::unwrapRef(objectType);

  // Handle interface dispatch
  if (auto* ifaceType =
          sun::codegen::support::tryGetType<InterfaceType>(objectType)) {
    return codegenInterfaceMethodCall(expr, objectPtr, ifaceType, methodName);
  }

  // Must be a class method call
  auto& classType = sun::codegen::support::requireType<ClassType>(
      objectType, "method call receiver", memberAccess.getLocation());

  return codegenClassMethodCall(expr, objectPtr, methodName, &memberAccess);
}

// -------------------------------------------------------------------
// Call expression dispatch
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const CallExprAST& expr) {
  std::string calleeName = "<call-expression>";

  // Check if this is a method call (MemberAccessAST as callee)
  if (auto* memberAccess =
          dynamic_cast<const MemberAccessAST*>(expr.getCallee())) {
    TypePtr ownerType =
        sun::types::unwrapRef(memberAccess->getObject()->getResolvedType());
    auto* ownerClass = sun::codegen::support::tryGetType<ClassType>(ownerType);
    const sun::types::ClassField* field =
        ownerClass
            ? ownerClass->getField(memberAccess->getTargetDeclarationId())
            : nullptr;
    if (!field || !field->type || !field->type->isCallable()) {
      return scopes.trackCallTemporary(codegenMethodCall(expr, *memberAccess),
                                       expr.getResolvedType());
    }
    calleeName = memberAccess->getMemberName();
  }

  if (auto* varRef = dynamic_cast<const sun::ast::VariableReferenceAST*>(
          expr.getCallee())) {
    calleeName = varRef->getName();

    // Check for built-in functions (bypass type system)
    if (intrinsics.isBuiltinFunction(calleeName)) {
      return intrinsics.codegenBuiltin(calleeName, expr);
    }

    // Check if this is a stack-allocated class constructor call:
    // ClassName(args...)
    if (auto* calleeClass =
            sun::codegen::support::tryGetType<ClassType>(*expr.getCallee())) {
      return classes.codegenStackClassInstance(expr, *calleeClass);
    }
  } else if (auto* qualName = dynamic_cast<const sun::ast::QualifiedNameAST*>(
                 expr.getCallee())) {
    // Resolve the qualified call through its selected declaration.
    std::string fullName = qualName->getFullName();
    calleeName = fullName;
    size_t pos;
    while ((pos = calleeName.find("::")) != std::string::npos) {
      calleeName.replace(pos, 2, "_");
    }
  }

  // Get the resolved function type (from semantic analysis)
  TypePtr calleeSunType = expr.getCallee()->getResolvedType();
  if (!calleeSunType || !calleeSunType->isCallable()) {
    logAndThrowError("Callee is not callable: " + calleeName);
    return nullptr;
  }

  // Handle Lambda type: fat pointer call with closure
  Value* result;
  TypePtr calleeReturnType;
  if (auto* lambdaType =
          sun::codegen::support::tryGetType<sun::types::LambdaType>(
              calleeSunType)) {
    result = codegenLambdaCall(expr, calleeName, *lambdaType);
    calleeReturnType = lambdaType->getReturnType();
  } else {
    // Handle Function type: direct call
    const auto& funcType =
        static_cast<const sun::types::FunctionType&>(*calleeSunType);
    result = codegenFunctionCall(expr, calleeName, funcType);
    calleeReturnType = funcType.getReturnType();
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
    const std::vector<ArgConversion>& conversions,
    const std::vector<TypePtr>& paramTypes, llvm::FunctionType* calleeTy,
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
    TypePtr argSunType = argExpr->getResolvedType();
    size_t paramIndex = i - firstArg;
    TypePtr paramType =
        paramIndex < paramTypes.size() ? paramTypes[paramIndex] : nullptr;
    Value* argVal = nullptr;

    switch (conversions[i]) {
      case ArgConversion::Borrow: {
        // A `ref array<T>` argument is a view value, passed on as it is
        auto* argRef =
            sun::codegen::support::tryGetType<ReferenceType>(argSunType);
        if (argRef && argRef->isUnsizedArrayRef()) {
          argVal = loadArrayView(codegen(*argExpr));
          break;
        }
        argVal = prepareRefArgument(argExpr, argSunType);
        break;
      }

      case ArgConversion::ArrayToView: {
        // The argument's storage, seen with its rank erased
        Value* storage = codegen(*argExpr);
        if (!storage) return false;
        auto* sized = sun::codegen::support::tryGetType<sun::types::ArrayType>(
            sun::types::unwrapRef(argSunType));
        if (!storage->getType()->isPointerTy()) {
          Function* func = ctx.builder->GetInsertBlock()->getParent();
          AllocaInst* temp = createEntryBlockAlloca(
              func, "arr.spill", sized->getDataStorageType(ctx.getContext()));
          ctx.builder->CreateStore(storage, temp);
          storage = temp;
        }
        argVal = emitArrayView(storage, sized->getDimensions());
        break;
      }

      case ArgConversion::RawPtrAsRef:
        // The pointer value is the referent's address
        argVal = codegen(*argExpr);
        break;

      case ArgConversion::ClassToRefInterface: {
        Value* classPtr = prepareRefArgument(argExpr, argSunType);
        if (!classPtr) return false;
        argVal = classes.prepareClassForRefInterface(
            classPtr, sun::types::unwrapRef(argSunType), paramType);
        break;
      }

      case ArgConversion::ClassToInterface: {
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        auto* classType =
            static_cast<ClassType*>(sun::types::unwrapRef(argSunType).get());
        auto* ifaceType = static_cast<InterfaceType*>(paramType.get());
        argVal = classes.createOwnedInterfaceFatPointer(argVal, classType,
                                                        ifaceType);
        break;
      }

      case ArgConversion::InterfaceUpcast:
      case ArgConversion::InterfaceRefUpcast: {
        bool borrowed = conversions[i] == ArgConversion::InterfaceRefUpcast;
        argVal = borrowed ? prepareRefArgument(argExpr, argSunType)
                          : codegen(*argExpr);
        if (!argVal) return false;
        if (borrowed) {
          argVal = classes.createBorrowedInterfaceUpcast(argVal, argSunType,
                                                            paramType);
        } else {
          auto* source = static_cast<InterfaceType*>(
              sun::types::unwrapRef(argSunType).get());
          auto* target = static_cast<InterfaceType*>(
              sun::types::unwrapRef(paramType).get());
          argVal = classes.upcastInterface(argVal, source, target, false);
        }
        break;
      }

      case ArgConversion::Move:
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        argVal = applyMoveSemantics(argVal, argSunType);
        break;

      case ArgConversion::WidenNumeric:
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        argVal = widenNumericIfNeeded(argVal, paramType, argSunType);
        break;

      case ArgConversion::StaticToRawPtr:
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        argVal = coerceStaticPtrToRawPtr(argVal, argSunType, paramType);
        break;

      case ArgConversion::DerefRawPtr:
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        argVal = ctx.builder->CreateLoad(typeResolver.resolve(paramType),
                                         argVal, "auto_deref_arg");
        break;

      case ArgConversion::CVararg:
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        argVal = externC.promoteVararg(argVal, argSunType);
        break;

      case ArgConversion::PassValue: {
        argVal = codegen(*argExpr);
        if (!argVal) return false;
        TypePtr valueType = sun::types::unwrapRef(argSunType);
        // Representation only: an interface value is carried as the address
        // of its fat pointer, a lambda literal as the address of its closure;
        // the parameter takes each by value.
        if (valueType && valueType->isInterface() &&
            argVal->getType()->isPointerTy()) {
          llvm::StructType* fatPtrType =
              InterfaceType::getFatPointerType(ctx.getContext());
          argVal =
              ctx.builder->CreateLoad(fatPtrType, argVal, "iface.arg.load");
        }
        unsigned slot = argValues.size();
        llvm::Type* expectedTy = calleeTy && slot < calleeTy->getNumParams()
                                     ? calleeTy->getParamType(slot)
                                     : nullptr;
        argVal = loadClosureForLambdaParam(argVal, valueType, expectedTy);
        // A sized array is carried by address; the parameter takes the
        // inline storage by value
        if (expectedTy && expectedTy->isArrayTy() &&
            argVal->getType()->isPointerTy()) {
          argVal = ctx.builder->CreateLoad(expectedTy, argVal, "arr.arg");
        }
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
    const CallExprAST& expr, const std::vector<TypePtr>& paramTypes,
    std::vector<sun::codegen::abi::PreparedArg>& out) {
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
    TypePtr paramType = i < paramTypes.size() ? paramTypes[i] : nullptr;
    TypePtr argSunType = argExpr->getResolvedType();

    Value* argVal = nullptr;
    switch (conversions[i]) {
      case ArgConversion::Borrow:
        // `ref T` is C's `T*`: pass the address.
        argVal = prepareRefArgument(argExpr, argSunType);
        break;
      case ArgConversion::WidenNumeric:
        argVal = codegen(*argExpr);
        if (argVal)
          argVal = widenNumericIfNeeded(argVal, paramType, argSunType);
        break;
      case ArgConversion::StaticToRawPtr:
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

Value* CodegenVisitor::codegenFunctionCall(
    const CallExprAST& expr, const std::string& calleeName,
    const sun::types::FunctionType& funcType) {
  // For Function types, we need to:
  // 1. Look up the llvm::Function directly
  // 2. Call it directly without any closure indirection

  llvm::Function* func = nullptr;
  if (expr.getTargetDeclarationId()) {
    func = functions.lookupFunctionById(expr.getTargetDeclarationId());
  }

  if (!func) {
    // A parameter, local, field, returned value, or other expression is an
    // indirect call through the one-word function pointer.
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

  // A throwing callee ('T throws IError') is tagged with "sun.canthrow"; inside
  // a try block it must be `invoke`d so its exception routes to the local
  // landing pad. Exceptions now propagate natively — no error-union unwrapping.
  bool canThrow = func->hasFnAttribute("sun.canthrow") || funcType.canThrow();
  Value* callResult = errors.emitPossiblyThrowingCall(
      func->getFunctionType(), func, argValues, canThrow, "calltmp");

  // Handle struct return values (classes returned by value)
  return materializeStructReturn(callResult);
}

// -------------------------------------------------------------------
// Lambda call codegen (fat pointer/closure call)
// -------------------------------------------------------------------

Value* CodegenVisitor::codegenLambdaCall(
    const CallExprAST& expr, const std::string& calleeName,
    const sun::types::LambdaType& lambdaType) {
  // For Lambda types, we need to:
  // 1. Get the closure pointer (fat pointer)
  // 2. Extract the function pointer from the closure
  // 3. Call with the fat pointer as the first argument

  // Try to get the closure pointer directly without loading (avoids redundant
  // alloca)
  Value* closurePtr = nullptr;
  if (auto* varRef = dynamic_cast<const sun::ast::VariableReferenceAST*>(
          expr.getCallee())) {
    // Check local variable (alloca)
    if (AllocaInst* alloca =
            scopes.findVariable(varRef->getTargetDeclarationId())) {
      closurePtr = alloca;
    }
    // Check global variable
    else if (GlobalVariable* gv =
                 variables.findGlobal(varRef->getTargetDeclarationId())) {
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
  // ('throws IError') are invoked so exceptions route to a local landing pad
  // inside try blocks.
  Value* result = errors.emitPossiblyThrowingCall(
      llvmFuncType, funcPtr, argValues, lambdaType.canThrow(), "calltmp");

  // Materialize struct return values for addressability
  return materializeStructReturn(result);
}

}  // namespace sun::codegen
