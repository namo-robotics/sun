#include "semantic_analysis/type_analysis/generic_type_arguments.h"
// classes.cpp - Class-related codegen (class definitions, member access, etc.)

#include <cmath>
#include <cstdint>

#include "ast.h"
#include "codegen/classes/class_generator.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics.h"
#include "codegen/support/scalar_ops.h"
#include "codegen/support/struct_access.h"
#include "parsing/parser.h"
#include "semantic_analysis/generic_type_arguments.h"
#include "semantic_analysis/semantic_scope.h"
#include "semantic_analysis/visibility.h"

using sun::types::ClassField;
using sun::types::ClassType;
using sun::types::TypePtr;

using sun::ast::ASTNodeType;
using sun::ast::ClassDefinitionAST;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::ast::PrototypeAST;
using sun::support::logAndThrowError;

using namespace llvm;

/** Provides the generator for class storage and method operations. */
namespace sun::codegen::classes {

using sun::codegen::intrinsics::Intrinsic;

// -------------------------------------------------------------------
// Precompiled class codegen (from linked bitcode)
// -------------------------------------------------------------------

Value* ClassGenerator::codegenPrecompiledClass(const ClassDefinitionAST& expr) {
  // Still need to register the class type for type checking
  auto classType = typeRegistry->getClass(expr.getDeclarationId());
  if (classType) {
    // Generate specializations for generic methods on this non-generic
    // precompiled class (e.g., HeapAllocator.create<T>)
    sun::codegen::CodegenState::ReceiverGuard receiver(state_);
    currentClass = classType;

    for (const auto& methodDecl : expr.getMethods()) {
      const FunctionAST& methodFunc = *methodDecl.function;
      const PrototypeAST& proto = methodFunc.getProto();

      if (proto.isGeneric()) {
        // Generate any pre-computed specializations from semantic analysis
        for (const auto& [instanceId, specializedAST] :
             methodFunc.getSpecializations()) {
          if (!specializedAST) continue;
          declareMethodFromAST(*specializedAST);
          generateMethodBody(*specializedAST);
        }
      }
    }
  }

  // Emit concrete instances of imported generic classes.
  if (expr.isGeneric()) {
    // Generate specializations that were added during semantic analysis.
    // Some may be library specializations (already in bitcode) - skip those.
    // Others may be user specializations (e.g., Vec<MyUserClass>) - codegen
    // those.
    for (const auto& [instanceId, specializedAST] : expr.getSpecializations()) {
      if (!specializedAST) continue;
      if (codegenedClasses.count(instanceId)) {
        continue;
      }

      if (specializedAST->isPrecompiled()) {
        // Ordinary methods are in the bundle; new generic methods may need
        // code.
        codegen(*specializedAST);
        continue;
      }

      librarySpecializations.insert(instanceId);
      codegen(*specializedAST);
    }
  }

  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}

// -------------------------------------------------------------------
// Declare every method of a class (no bodies), so calls can be emitted
// before the class definition is reached
// -------------------------------------------------------------------

void ClassGenerator::declareClassMethods(
    const ClassDefinitionAST& expr,
    const std::shared_ptr<ClassType>& classType) {
  if (!classType) return;

  for (const auto& methodDecl : expr.getMethods()) {
    const FunctionAST& methodFunc = *methodDecl.function;
    const PrototypeAST& proto = methodFunc.getProto();

    // A generic method has no signature of its own — declare the
    // specializations semantic analysis created instead.
    if (proto.isGeneric()) {
      for (const auto& [instanceId, specializedAST] :
           methodFunc.getSpecializations()) {
        if (!specializedAST) continue;
        declareMethodFromAST(*specializedAST);
      }
      continue;
    }

    declareMethodFromAST(methodFunc);
  }
  for (const auto& method : classType->getMethods()) {
    if (!method.defaultImplementation || method.isGeneric()) continue;
    const auto symbol = state_.declarationSymbol(method.declarationId);
    auto* function = module->getFunction(symbol);
    if (!function) {
      std::vector<llvm::Type*> parameters{
          PointerType::getUnqual(ctx.getContext())};
      for (const auto& parameter : method.paramTypes)
        parameters.push_back(typeResolver.resolve(parameter));
      auto* signature = llvm::FunctionType::get(
          typeResolver.resolveForReturn(method.returnType), parameters, false);
      function = Function::Create(signature, Function::ExternalLinkage, symbol,
                                  module);
    }
    functions().registerFunction(method.declarationId, function);
  }
}

// Declare the methods of every class a block defines, before any body is
// emitted, so a method may call one of a class declared further down the file
// (the same courtesy the function pre-pass extends to free functions).
void ClassGenerator::declareBlockClassMethods(const ClassDefinitionAST& expr) {
  if (expr.isPartial()) return;

  // Imported templates can acquire new specializations in this consumer.
  // Their callers need declarations before any library body is emitted.
  if (expr.isGeneric()) {
    for (const auto& [instanceId, specializedAST] : expr.getSpecializations()) {
      if (!specializedAST) continue;
      if (sun::semantic_analysis::type_analysis::mentionsTypeParameter(
              typeRegistry->getClass(instanceId)))
        continue;
      declareBlockClassMethods(*specializedAST);
    }
    return;
  }

  declareClassMethods(expr, typeRegistry->getClass(expr.getDeclarationId()));
}

// -------------------------------------------------------------------
// Class definition codegen
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const ClassDefinitionAST& expr) {
  // Skip precompiled classes - they come from linked bitcode
  if (expr.isPrecompiled()) {
    return codegenPrecompiledClass(expr);
  }

  // Skip partial classes - their methods are merged into the primary class
  if (expr.isPartial()) {
    return ConstantFP::get(ctx.getContext(), APFloat(0.0));
  }

  // Skip generic class definitions (templates) - they are instantiated on
  // demand
  if (expr.isGeneric()) {
    // Generate all specializations that were created during semantic analysis
    // This mirrors how generic functions work - specializations are
    // pre-computed and stored on the AST
    for (const auto& [instanceId, specializedAST] : expr.getSpecializations()) {
      if (!specializedAST) continue;
      // Check if already codegenned (not just type-registered)
      if (!specializedAST || codegenedClasses.count(instanceId)) continue;
      // Resolving a template's own signature instantiates the shape it names
      // — `ref Pair<T>` in `unwrap<T>` yields Pair<T>, whose T is still a
      // type parameter. That shape has no layout to emit; the class the code
      // actually uses is instantiated when unwrap<i32> is.
      if (sun::semantic_analysis::type_analysis::mentionsTypeParameter(
              typeRegistry->getClass(instanceId)))
        continue;
      codegen(*specializedAST);
    }

    // Return a void value - generic class templates don't generate code
    return ConstantFP::get(ctx.getContext(), APFloat(0.0));
  }

  // Error if codegen sees an unmarked duplicate — this is a compiler bug
  if (codegenedClasses.count(expr.getDeclarationId())) {
    logAndThrowError("Duplicate class definition reached codegen: " +
                     expr.getName());
  }

  // Mark this class as being codegenned
  codegenedClasses.insert(expr.getDeclarationId());

  // Get the class type (already fully built by semantic analyzer)
  auto classType = typeRegistry->getClass(expr.getDeclarationId());

  // Track if this class is user-defined (not from precompiled library)
  // Check both the precompiled flag and if this is a library specialization
  bool isUserDefined = !expr.isPrecompiled() &&
                       !librarySpecializations.count(expr.getDeclarationId());

  // Create the LLVM struct type for the class
  llvm::StructType* structType = classType->getStructType(ctx.getContext());

  // Save current class context
  sun::codegen::CodegenState::ReceiverGuard receiver(state_);
  currentClass = classType;

  // PASS 1: Declare all method functions first (so methods can call each other)
  declareClassMethods(expr, classType);

  // PASS 2: Generate all method bodies
  for (const auto& methodDecl : expr.getMethods()) {
    const FunctionAST& methodFunc = *methodDecl.function;
    const PrototypeAST& proto = methodFunc.getProto();

    // For generic methods, generate bodies for all pre-computed specializations
    if (proto.isGeneric()) {
      for (const auto& [instanceId, specializedAST] :
           methodFunc.getSpecializations()) {
        if (!specializedAST) continue;
        const auto specSymbol =
            state_.declarationSymbol(specializedAST->getDeclarationId());
        if (specializedAST) {
          generateMethodBody(*specializedAST);
          // Track user-defined method specializations for IR filtering
          if (isUserDefined) {
            functions().noteUserDefined(specSymbol);
          }
        }
      }
      continue;
    }

    std::string symbol = state_.declarationSymbol(proto.getDeclarationId());
    generateMethodBody(methodFunc);
    // Track user-defined methods for IR filtering
    if (isUserDefined) {
      functions().noteUserDefined(symbol);
    }
  }

  // Each inherited default has a class-owned wrapper with its own identity.
  for (const auto& method : classType->getMethods()) {
    if (!method.defaultImplementation || method.isGeneric()) continue;
    Function* func = functions().lookupFunctionById(method.declarationId);
    if (!func->empty()) continue;
    Function* defaultFunc =
        functions().lookupFunctionById(method.defaultImplementation);
    llvm::Type* returnType = func->getReturnType();
    BasicBlock* BB = BasicBlock::Create(ctx.getContext(), "entry", func);
    ctx.builder->SetInsertPoint(BB);
    debugInfo.clearLocation(*ctx.builder);

    // Build argument list (just forward all arguments). The closure arg
    // (arg 0) is passed through verbatim: its func slot points at this
    // wrapper, not the default impl, which is fine because method bodies
    // only ever read the env slot (field 1).
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
    for (const auto& [instanceId, specializedAST] :
         methodFunc.getSpecializations()) {
      if (!specializedAST) continue;

      // Declare if not already declared, then generate body
      declareMethodFromAST(*specializedAST);
      generateMethodBody(*specializedAST);
    }
  }

  // PASS 4: Generate vtables for each implemented interface
  // A vtable contains function pointers for each interface method in
  // declaration order, allowing dynamic dispatch on interface-typed values.
  // Note: Generic methods cannot be included in vtables (they require
  // compile-time type information). Only non-generic methods are included.
  for (auto interfaceId : classType->getImplementedInterfaces()) {
    auto interfaceType = typeRegistry->getInterface(interfaceId);
    if (!interfaceType) {
      continue;
    }
    getOrCreateInterfaceVtable(classType.get(), interfaceType.get());
  }

  // Class definitions return void
  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}

// -------------------------------------------------------------------
// Declare a method function from a specialized AST (no body generated)
// -------------------------------------------------------------------

Function* ClassGenerator::declareMethodFromAST(
    const FunctionAST& specializedAST) {
  const auto symbol =
      state_.declarationSymbol(specializedAST.getDeclarationId());
  const PrototypeAST& proto = specializedAST.getProto();
  if (Function* existing = module->getFunction(symbol)) {
    functions().registerFunction(proto.getDeclarationId(), existing);
    return existing;
  }

  // Build parameter types: closure ptr first ({ func, env } with the
  // receiver in env), then regular params
  std::vector<llvm::Type*> paramTypes;
  paramTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // closure

  if (!proto.hasResolvedParamTypes()) {
    logAndThrowError(
        "Method parameter types not resolved by semantic analysis: " + symbol);
    return nullptr;
  }
  // The fixed parameters, then the elements of any `args...` pack
  for (const auto& sunType : proto.getAllParamTypes()) {
    paramTypes.push_back(typeResolver.resolve(sunType));
  }

  // Get return type (must be resolved by semantic analysis)
  llvm::Type* returnType;
  bool canError = proto.hasReturnType() && proto.getReturnType()->canError;
  if (proto.hasResolvedReturnType()) {
    returnType = typeResolver.resolveForReturn(proto.getResolvedReturnType());
  } else if (!proto.hasReturnType()) {
    returnType = llvm::Type::getVoidTy(ctx.getContext());
  } else {
    logAndThrowError("Method return type not resolved by semantic analysis: " +
                     symbol);
    return nullptr;
  }

  // With native exceptions a throwing method ('T throws IError') returns plain
  // T; the marker only means it may unwind.

  // Create the function declaration
  llvm::FunctionType* funcType =
      llvm::FunctionType::get(returnType, paramTypes, false);
  Function* func =
      Function::Create(funcType, Function::ExternalLinkage, symbol, module);
  functions().registerFunction(proto.getDeclarationId(), func);
  // Tag throwing methods so call sites emit `invoke` inside a try block.
  if (canError) {
    func->addFnAttr("sun.canthrow");
  }

  // Set parameter names
  auto argIt = func->arg_begin();
  argIt->setName("closure");
  ++argIt;

  for (const auto& argName : proto.getAllParamNames()) {
    argIt->setName(argName);
    ++argIt;
  }

  return func;
}

// -------------------------------------------------------------------
// Method prologue: unwrap the receiver from the closure arg
// -------------------------------------------------------------------

// Method ABI: arg 0 is a ptr to the closure struct { func, env }; the
// receiver ('this') lives in the env slot (field 1). This is the only place
// method bodies touch the closure arg — bodies must never read field 0
// (forwarding wrappers pass their own closure through, so the func slot may
// point at the wrapper rather than the called function).
void ClassGenerator::emitMethodPrologueThis(Function* func) {
  llvm::StructType* closureTy = typeResolver.getClosureType();
  Value* envSlot = ctx.builder->CreateStructGEP(closureTy, &*func->arg_begin(),
                                                1, "this.env");
  Value* thisVal = ctx.builder->CreateLoad(
      PointerType::getUnqual(ctx.getContext()), envSlot, "this.recv");
  AllocaInst* thisAlloca = ctx.builder->CreateAlloca(
      PointerType::getUnqual(ctx.getContext()), nullptr, "this.addr");
  ctx.builder->CreateStore(thisVal, thisAlloca);
  thisPtr = ctx.builder->CreateLoad(PointerType::getUnqual(ctx.getContext()),
                                    thisAlloca, "this");

  debugInfo.declareThisParameter(*ctx.builder, thisAlloca, currentClass);
}

// -------------------------------------------------------------------
// Generate a method body for an already-declared function
// -------------------------------------------------------------------

void ClassGenerator::generateMethodBody(const FunctionAST& methodFunc) {
  const auto symbol = state_.declarationSymbol(methodFunc.getDeclarationId());
  const PrototypeAST& proto = methodFunc.getProto();

  Function* func = functions().lookupFunctionById(proto.getDeclarationId());

  // Skip if the function already has a body
  if (!func->empty()) return;

  // Get return type
  llvm::Type* returnType = func->getReturnType();

  // Check if this method can return errors (declared with 'throws IError')
  bool canError = proto.hasReturnType() && proto.getReturnType()->canError;

  // Save and set error handling context. With native exceptions a throwing
  // method returns plain T, so the value type is just the return type.
  sun::codegen::CodegenState::ReturnGuard returns(state_);
  currentFunctionCanError = canError;
  currentFunctionValueType = canError ? returnType : nullptr;
  currentFunctionReturnsRef =
      proto.hasReturnType() && proto.getReturnType()->isReference();

  // Create entry basic block
  BasicBlock* BB = BasicBlock::Create(ctx.getContext(), "entry", func);
  ctx.builder->SetInsertPoint(BB);

  debugInfo.enterFunction(*ctx.builder, func, proto.getName(),
                          proto.getLocation());

  // Create a new scope for the method
  scopes().push().isFunctionBoundary = true;

  emitMethodPrologueThis(func);

  // Store other parameters
  auto argIt = func->arg_begin();
  ++argIt;  // Skip closure

  // The fixed parameters, then the elements of any `args...` pack — the same
  // order the signature was declared in (specialized generic classes have
  // their types resolved by semantic analysis).
  const std::vector<std::string> paramNames = proto.getAllParamNames();
  const std::vector<TypePtr> paramTypes = proto.getAllParamTypes();
  if (!proto.hasResolvedParamTypes() ||
      paramTypes.size() != paramNames.size()) {
    logAndThrowError(
        "Method parameter types not resolved by semantic analysis: " + symbol);
    return;
  }
  const size_t fixedCount = proto.getArgs().size();

  for (size_t i = 0; i < paramNames.size(); ++i) {
    const std::string& argName = paramNames[i];
    llvm::Type* argLLVMType = typeResolver.resolve(paramTypes[i]);

    AllocaInst* alloca =
        ctx.builder->CreateAlloca(argLLVMType, nullptr, argName);
    ctx.builder->CreateStore(&*argIt, alloca);
    scopes()
        .back()
        .variables[i < fixedCount
                       ? proto.declarationIdentity().parameters.at(i)
                       : proto.declarationIdentity().variadicParameters.at(
                             i - fixedCount)] = alloca;
    // A pack element has no annotation in the source to point a debug entry at
    if (i < fixedCount) {
      debugDeclareParam(alloca, argName, proto, static_cast<unsigned>(i),
                        /*argNoBase=*/2);
    }
    scopes().trackOwnedParam(alloca, argName, paramTypes[i]);
    ++argIt;
  }

  // Defaults retain the bindings selected before parameters became visible.
  const size_t prefixCount = methodFunc.getFieldInitializerCount();
  if (prefixCount) {
    for (size_t i = 0; i < prefixCount; ++i) {
      const auto& assignment =
          static_cast<const sun::ast::MemberAssignmentAST&>(
              *methodFunc.getBody().getBody().at(i));
      codegen(static_cast<const ExprAST&>(assignment));
      const auto* field =
          currentClass->getField(assignment.getTargetDeclarationId());
      if (field && sun::types::typeNeedsDrop(field->type)) {
        auto* address = sun::codegen::support::fieldPtr(
            *ctx.builder, currentClass.get(), thisPtr, *field,
            field->name + ".initialized");
        scopes().trackClassAllocation(address, "this." + field->name,
                                      field->type,
                                      /*unwindOnly=*/true);
      }
    }
  }

  // The source body follows the defaults in the same constructor frame.
  codegen(methodFunc.getBody(), prefixCount);

  // Add implicit return if no explicit return. A non-void body whose last
  // statement always returns/throws (e.g. a match with terminating arms)
  // leaves an unreachable tail block. A void body that falls off the end
  // still owns its locals, so they are dropped here as an explicit
  // `return;` would drop them.
  if (!ctx.builder->GetInsertBlock()->getTerminator()) {
    if (returnType->isVoidTy()) {
      scopes().emitScopeCleanup();
      ctx.builder->CreateRetVoid();
    } else {
      ctx.builder->CreateUnreachable();
    }
  }

  scopes().pop();

  debugInfo.exitFunction(func);

  // Verify the function
  verifyFunction(*func);
}

// -------------------------------------------------------------------
// 'this' expression codegen
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const sun::ast::ThisExprAST& expr) {
  if (!thisPtr) {
    logAndThrowError("'this' used outside of a class method");
    return nullptr;
  }
  return thisPtr;
}

// -------------------------------------------------------------------
// Module member helpers
// -------------------------------------------------------------------

// Retrieve the selected global when the receiver denotes a module.
GlobalVariable* ClassGenerator::moduleMemberGlobal(
    const ExprAST& object, sun::semantic_analysis::DeclarationId id) {
  TypePtr objectType = object.getResolvedType();
  if (!objectType || !objectType->isModule()) return nullptr;
  return gen_.variableGenerator().findGlobal(id);
}

// -------------------------------------------------------------------
// Member access codegen (field read)
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const sun::ast::MemberAccessAST& expr) {
  const std::string& memberName = expr.getMemberName();

  // Handle module member access: mod_x.mod_y or mod_x.var
  TypePtr objectType = expr.getObject()->getResolvedType();
  if (auto* moduleType =
          sun::codegen::support::tryGetType<sun::types::ModuleType>(
              objectType)) {
    // Check if the result type is also a module (nested module access)
    if (sun::codegen::support::tryGetType<sun::types::ModuleType>(expr)) {
      // Return null sentinel - next member access will handle it
      return llvm::ConstantPointerNull::get(
          llvm::PointerType::getUnqual(ctx.getContext()));
    }

    // Check for global variable in this module
    GlobalVariable* gv =
        gen_.variableGenerator().findGlobal(expr.getTargetDeclarationId());
    if (gv) {
      TypePtr varType = expr.getResolvedType();
      // Classes and interfaces return the pointer, not a load
      if (varType && (varType->isClass() || varType->isInterface())) {
        return gv;
      }
      return ctx.builder->CreateLoad(gv->getValueType(), gv,
                                     memberName.c_str());
    }

    if (expr.getResolvedType() && expr.getResolvedType()->isFunction()) {
      return functions().lookupFunctionById(expr.getTargetDeclarationId());
    }

    logAndThrowError(
        "Cannot find member '" + memberName + "' in module '" +
        sun::semantic_analysis::displayModulePath(moduleType->getModulePath()) +
        "'");
  }

  // The analyzed object identifies the enum, including qualified unit variants.
  if (auto enumType =
          sun::codegen::support::tryGetTypePtr<sun::types::EnumType>(
              *expr.getObject())) {
    if (const auto* variant = enumType->getVariant(memberName))
      return gen_.enumGenerator().codegenVariantAccess(*enumType, *variant);
  }

  // Refresh objectType in case it was not set from module handling above
  if (!objectType) {
    objectType = expr.getObject()->getResolvedType();
  }

  // Resolve the object down to (pointer, class type)
  auto [objectPtr, classType] = codegenObjectPtr(*expr.getObject());
  if (!objectPtr) return nullptr;
  if (!classType) {
    logAndThrowError("Member access on non-class type");
    return nullptr;
  }

  // Check if it's a field access
  const ClassField* field = classType->getField(expr.getTargetDeclarationId());
  if (field) {
    Value* fieldPtr = sun::codegen::support::fieldPtr(
        *ctx.builder, classType, objectPtr, *field, memberName);

    // For compound fields carried by address, return their storage pointer.
    // Interface dispatch loads its fat pointer from that address, just as it
    // does for interface locals, parameters and globals. A sized array field
    // is its inline storage; a `ref array<T>` field holds a view value and
    // is loaded like any other reference.
    if (field->type->isClass() || field->type->isInterface() ||
        field->type->isArray() ||
        sun::codegen::CodegenVisitor::isPayloadEnum(field->type)) {
      return fieldPtr;
    }

    // Load the field value
    llvm::Type* fieldLLVMType = field->type->toLLVMType(ctx.getContext());
    return ctx.builder->CreateAlignedLoad(
        fieldLLVMType, fieldPtr,
        sun::codegen::support::fieldAlign(classType, fieldLLVMType,
                                          module->getDataLayout()),
        memberName + ".val");
  }

  // Bound method reference: a method in value position materializes the
  // closure value { methodFn, objectPtr }
  if (expr.isBoundMethodRef()) {
    return codegenBoundMethodReference(expr, objectPtr);
  }

  if (!expr.getResolvedType() || !expr.getResolvedType()->isCallable())
    logAndThrowError("Field access has no registered target",
                     expr.getLocation());

  // Calls consume the selected method separately from its receiver.
  return objectPtr;
}

// -------------------------------------------------------------------
// Bound method reference codegen: obj.method in value position
// Produces a closure struct VALUE { methodFn, objectPtr } (lambda ABI)
// -------------------------------------------------------------------

Value* ClassGenerator::codegenBoundMethodReference(
    const sun::ast::MemberAccessAST& expr, Value* objectPtr) {
  Function* methodFunc =
      functions().lookupFunctionById(expr.getTargetDeclarationId());

  return materializeMethodClosureValue(methodFunc, objectPtr);
}

// -------------------------------------------------------------------
// Stack-allocated class instance codegen: ClassName(args...)
// -------------------------------------------------------------------

Value* ClassGenerator::codegenStackClassInstance(
    const sun::ast::CallExprAST& expr, ClassType& classType) {
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
      "memset",
      llvm::FunctionType::get(PointerType::getUnqual(ctx.getContext()),
                              {PointerType::getUnqual(ctx.getContext()),
                               llvm::Type::getInt32Ty(ctx.getContext()),
                               llvm::Type::getInt64Ty(ctx.getContext())},
                              false));
  ctx.builder->CreateCall(
      memsetFn,
      {alloca, ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0),
       ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()), structSize)});

  // Call the constructor (init method) if it exists
  const auto* ctor = classType.getMethod(expr.getTargetDeclarationId());
  Function* ctorFunc =
      ctor ? functions().lookupFunctionById(ctor->declarationId) : nullptr;
  size_t argCount = expr.getArgs().size();

  if (ctorFunc) {
    const auto& paramTypes = ctor ? ctor->paramTypes : std::vector<TypePtr>{};

    std::vector<Value*> ctorArgs = generateCtorArgs(
        ctorFunc, alloca, expr.getArgs(), expr.getArgConversions(), paramTypes);
    // A throwing constructor unwinds like any other call: inside a try it
    // must be invoked so the exception reaches the landing pad.
    bool ctorCanThrow =
        (ctor && ctor->canThrow) || ctorFunc->hasFnAttribute("sun.canthrow");
    gen_.errorGenerator().emitPossiblyThrowingCall(ctorFunc, ctorArgs,
                                                   ctorCanThrow, "");
  }

  // Track the temporary for deinit ONLY if not moved (ownership
  // transferred). The borrow checker marks temporaries as moved when
  // assigned to a variable or field. Moved temporaries are owned by the
  // destination, which will call deinit. Non-moved temporaries must be
  // deinited here.
  if (!expr.isMoved()) {
    auto classTypePtr = std::make_shared<ClassType>(classType);
    scopes().trackClassAllocation(alloca, "stack.obj", classTypePtr);
  }

  return alloca;
}

// -------------------------------------------------------------------
// Generate constructor argument values, handling ref parameters
// -------------------------------------------------------------------

std::vector<Value*> ClassGenerator::generateCtorArgs(
    llvm::Function* ctorFunc, Value* thisPtr,
    const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<sun::semantic_analysis::ArgConversion>& conversions,
    const std::vector<TypePtr>& paramTypes) {
  std::vector<Value*> ctorArgs;
  ctorArgs.push_back(
      materializeMethodClosure(ctorFunc, thisPtr, "method.closure"));
  if (!emitCallArguments(args, conversions, paramTypes,
                         ctorFunc->getFunctionType(), ctorArgs, "init")) {
    return {};
  }
  return ctorArgs;
}

// -------------------------------------------------------------------
// Member assignment codegen (field write)
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const sun::ast::MemberAssignmentAST& expr) {
  // mod.global = value: the module is compile-time only, so this writes the
  // global directly
  if (GlobalVariable* gv = moduleMemberGlobal(*expr.getObject(),
                                              expr.getTargetDeclarationId())) {
    Value* value = codegen(*expr.getValue());
    if (!value) return nullptr;
    assignToVariableSlot(
        gv, value, sun::types::unwrapRef(expr.getValue()->getResolvedType()),
        expr.getMemberName());
    return value;
  }

  // Resolve the object down to (pointer, class type); the shared helper also
  // unwraps ref-to-class objects, which this path previously rejected
  auto [objectPtr, classType] = codegenObjectPtr(*expr.getObject());
  if (!objectPtr) return nullptr;
  if (!classType) {
    logAndThrowError("Member assignment on non-class type");
    return nullptr;
  }

  const std::string& memberName = expr.getMemberName();

  // Get the field info
  const ClassField* field = classType->getField(expr.getTargetDeclarationId());
  if (!field) {
    logAndThrowError("Unknown field: " + memberName);
    return nullptr;
  }

  // Generate the value to assign
  Value* value = codegen(*expr.getValue());
  if (!value) return nullptr;

  // Get expected field type
  llvm::Type* fieldLLVMType = field->type->toLLVMType(ctx.getContext());

  TypePtr valueSunType = expr.getValue()->getResolvedType();

  // A `ref array<T>` field holds the view value; a view expression may
  // arrive as the value or as a pointer to where it is stored
  if (auto* fieldRef =
          sun::codegen::support::tryGetType<sun::types::ReferenceType>(
              field->type)) {
    if (fieldRef->isUnsizedArrayRef()) {
      value = gen_.loadArrayView(value);
    }
  }

  // Generate GEP to access the field
  Value* fieldPtr = sun::codegen::support::fieldPtr(
      *ctx.builder, classType, objectPtr, *field, memberName + ".ptr");

  // What becomes of the value the field held, as semantic analysis worked it
  // out: a constructor's first write to a field lands on storage that has
  // never held a value and releases nothing, and every other write drops.
  auto dropOverwrittenValue = [&]() {
    switch (expr.fieldWriteKind()) {
      case sun::ast::FieldWriteKind::StartsLife:
        return;
      case sun::ast::FieldWriteKind::ReplacesValue:
        scopes().emitDropInPlace(field->type, fieldPtr, memberName);
        scopes().markInitialized(fieldPtr, field->type);
        return;
    }
  };

  // Interface fields own the complete { data, vtable } value. A concrete
  // source moves into a stable erased box; an interface source transfers its
  // existing owner and is cleared.
  if (auto* fieldInterfaceType =
          sun::codegen::support::tryGetType<sun::types::InterfaceType>(
              field->type)) {
    if (value == fieldPtr) return value;

    Value* fatPtrValue = value;
    TypePtr sourceType = sun::types::unwrapRef(valueSunType);
    if (auto* sourceClassType =
            sun::codegen::support::tryGetType<ClassType>(sourceType)) {
      fatPtrValue = createOwnedInterfaceFatPointer(value, sourceClassType,
                                                   fieldInterfaceType);
      if (!fatPtrValue) return nullptr;
    } else if (sourceType && sourceType->isInterface()) {
      fatPtrValue =
          upcastInterface(value, static_cast<InterfaceType*>(sourceType.get()),
                          fieldInterfaceType, false);
    }

    dropOverwrittenValue();
    ctx.builder->CreateAlignedStore(
        fatPtrValue, fieldPtr,
        sun::codegen::support::fieldAlign(classType, fieldLLVMType,
                                          module->getDataLayout()));
    return fatPtrValue;
  }

  // Sized array fields own their elements inline: the source array MOVES in
  // after the field's old elements are dropped.
  if (auto* fieldArrayType =
          sun::codegen::support::tryGetType<sun::types::ArrayType>(
              field->type)) {
    dropOverwrittenValue();
    gen_.emitArrayTransfer(fieldPtr, value, *fieldArrayType, /*move=*/true);
    return value;
  }

  // Payload-enum fields: the value arrives as a storage pointer. Drop the
  // overwritten field first, then MOVE the source in — never an implicit
  // copy.
  if (sun::codegen::CodegenVisitor::isPayloadEnum(field->type)) {
    Value* structVal = value;
    if (value->getType()->isPointerTy()) {
      dropOverwrittenValue();
      structVal = gen_.applyMoveSemantics(value, field->type);
    }
    ctx.builder->CreateStore(structVal, fieldPtr);
    return structVal;
  }

  // Handle class-typed fields: the source instance MOVES into the field.
  // The overwritten field value is dropped first, then the source is copied
  // in and invalidated.
  if (auto* fieldClassType =
          sun::codegen::support::tryGetType<ClassType>(field->type)) {
    llvm::StructType* fieldStructType =
        fieldClassType->getStructType(ctx.getContext());
    const DataLayout& DL = module->getDataLayout();
    uint64_t structSize = DL.getTypeAllocSize(fieldStructType);

    // Drop whatever the field currently holds
    dropOverwrittenValue();

    // value is a pointer to the source class instance
    // fieldPtr is a pointer to the embedded struct in the parent class
    // If value is not a pointer (e.g., struct returned by value from a call),
    // materialize it to a stack alloca first so memcpy has a valid source.
    llvm::Align srcAlign = DL.getABITypeAlign(fieldStructType);
    bool sourceIsAddressable = value->getType()->isPointerTy();
    if (!sourceIsAddressable) {
      AllocaInst* tempAlloca = ctx.builder->CreateAlloca(
          fieldStructType, nullptr, memberName + ".tmp");
      ctx.builder->CreateStore(value, tempAlloca);
      value = tempAlloca;
      srcAlign = tempAlloca->getAlign();
    }
    // The destination sits inside the parent, so it inherits the parent's
    // packing, not the field struct's own alignment
    ctx.builder->CreateMemCpy(
        fieldPtr,
        sun::codegen::support::fieldAlign(classType, fieldStructType,
                                          module->getDataLayout()),
        value, srcAlign, structSize);
    if (sourceIsAddressable) {
      // The field owns the payload now. Release source ownership and clear
      // the old contents.
      scopes().markClassAllocationAsDeinited(value, valueSunType);
      ctx.builder->CreateMemSet(
          value, ConstantInt::get(llvm::Type::getInt8Ty(ctx.getContext()), 0),
          structSize, srcAlign);
    }
    return value;
  }

  value = sun::codegen::support::widenNumericIfNeeded(
      *ctx.builder, typeResolver, value, field->type, valueSunType);

  // Float literals default to f64 but may initialize an f32 field.
  ASTNodeType valueKind = expr.getValue()->getType();
  bool valueIsLiteral = valueKind == ASTNodeType::NUMBER ||
                        valueKind == ASTNodeType::CHAR_LITERAL;
  if (valueIsLiteral && value->getType()->isDoubleTy() &&
      fieldLLVMType->isFloatTy()) {
    value = ctx.builder->CreateFPTrunc(value, fieldLLVMType, "narrow");
  }

  // Store the value
  ctx.builder->CreateAlignedStore(
      value, fieldPtr,
      sun::codegen::support::fieldAlign(classType, fieldLLVMType,
                                        module->getDataLayout()));

  // Return the value (like C assignment)
  return value;
}

// -------------------------------------------------------------------
// Interface definition codegen
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const sun::ast::InterfaceDefinitionAST& expr) {
  // Default bodies are emitted as concrete class methods.
  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}

// -------------------------------------------------------------------
// Generic call codegen: name<Type>(args...)
// For standalone generic functions (not methods)
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const sun::ast::GenericCallAST& expr) {
  const std::string& funcName = expr.getFunctionName();
  const auto& typeArgs = expr.getTypeArguments();

  // Get first resolved type argument (semantic analysis must have set this)
  auto getFirstTypeArg = [&]() -> TypePtr {
    if (!expr.hasResolvedTypeArgs() || expr.getResolvedTypeArgs().empty()) {
      logAndThrowError("Type argument not resolved by semantic analysis for: " +
                       funcName);
    }
    return expr.getResolvedTypeArgs()[0];
  };

  // Handle generic intrinsics via switch
  switch (sun::codegen::intrinsics::getIntrinsic(funcName)) {
    case Intrinsic::Sizeof:
      return intrinsics().codegenSizeofIntrinsic(getFirstTypeArg());
    case Intrinsic::Init:
      return intrinsics().codegenInitIntrinsic(
          getFirstTypeArg(), expr.getArgs(), expr.getArgConversions(),
          expr.getTargetDeclarationId());
    case Intrinsic::Load:
      return intrinsics().codegenLoadIntrinsic(getFirstTypeArg(),
                                               expr.getArgs());
    case Intrinsic::Store:
      return intrinsics().codegenStoreIntrinsic(getFirstTypeArg(),
                                                expr.getArgs());
    case Intrinsic::PtrAsRaw:
      return intrinsics().codegenPtrAsRawIntrinsic(expr.getArgs());
    case Intrinsic::AddressOf:
      return intrinsics().codegenAddressOfIntrinsic(expr.getArgs());
    case Intrinsic::ToRef:
      return intrinsics().codegenToRefIntrinsic(expr.getArgs());
    case Intrinsic::Is:
      return intrinsics().codegenIsIntrinsic(getFirstTypeArg(), expr.getArgs());
    case Intrinsic::Deinit:
      return intrinsics().codegenDeinitIntrinsic(getFirstTypeArg(),
                                                 expr.getArgs());
    case Intrinsic::EnumFromInt:
      return intrinsics().codegenEnumFromIntIntrinsic(expr);
    case Intrinsic::Convert:
      return intrinsics().codegenConvertIntrinsic(getFirstTypeArg(),
                                                  expr.getArgs());
    case Intrinsic::Bitcast:
      return intrinsics().codegenBitcastIntrinsic(getFirstTypeArg(),
                                                  expr.getArgs());
    case Intrinsic::Spawn:
      return intrinsics().codegenSpawnIntrinsic(
          getFirstTypeArg(), expr.getResolvedType(), expr.getArgs(),
          expr.getArgConversions());
    case Intrinsic::ThreadJoin:
      return intrinsics().codegenThreadJoinIntrinsic(getFirstTypeArg(),
                                                     expr.getArgs(),
                                                     /*dropResult=*/false);
    case Intrinsic::ThreadJoinDrop:
      return intrinsics().codegenThreadJoinIntrinsic(getFirstTypeArg(),
                                                     expr.getArgs(),
                                                     /*dropResult=*/true);
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
    auto calleeType = expr.getResolvedCalleeType();
    if (!calleeType || !calleeType->isFunction())
      logAndThrowError("Generic call has no resolved callable signature");
    const auto& signature =
        static_cast<const sun::types::FunctionType&>(*calleeType);
    Function* specializedFunc =
        functions().lookupFunctionById(expr.getTargetDeclarationId());

    std::vector<Value*> argValues;
    if (AllocaInst* envPtr =
            scopes().findVariable(expr.getTargetDeclarationId())) {
      argValues.push_back(envPtr);
    }
    const auto& specParamTypes = signature.getParamTypes();
    bool canThrow = signature.canThrow();

    if (!emitCallArguments(expr.getArgs(), expr.getArgConversions(),
                           specParamTypes, specializedFunc->getFunctionType(),
                           argValues, funcName)) {
      logAndThrowError("Failed to generate argument for generic call");
      return nullptr;
    }

    Value* result = gen_.errorGenerator().emitPossiblyThrowingCall(
        specializedFunc->getFunctionType(), specializedFunc, argValues,
        canThrow, "generic.call");
    return scopes().trackCallTemporary(gen_.materializeStructReturn(result),
                                       expr.getResolvedType());
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
  if (auto* resolvedClass =
          sun::codegen::support::tryGetType<ClassType>(expr)) {
    auto classType = typeRegistry->getClass(resolvedClass->getDeclarationId());
    // Create a stack-allocated instance and call constructor
    llvm::StructType* structType = classType->getStructType(ctx.getContext());
    Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
    AllocaInst* alloca =
        createEntryBlockAlloca(currentFunc, "stack.obj", structType);

    // Zero-initialize
    const DataLayout& DL = module->getDataLayout();
    uint64_t structSize = DL.getTypeAllocSize(structType);
    llvm::FunctionCallee memsetFn = module->getOrInsertFunction(
        "memset",
        llvm::FunctionType::get(PointerType::getUnqual(ctx.getContext()),
                                {PointerType::getUnqual(ctx.getContext()),
                                 llvm::Type::getInt32Ty(ctx.getContext()),
                                 llvm::Type::getInt64Ty(ctx.getContext())},
                                false));
    ctx.builder->CreateCall(
        memsetFn,
        {alloca, ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0),
         ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()),
                          structSize)});

    // Call the constructor selected during semantic analysis.
    const auto* ctor = classType->getMethod(expr.getTargetDeclarationId());
    Function* ctorFunc =
        ctor ? functions().lookupFunctionById(ctor->declarationId) : nullptr;
    size_t argCount = expr.getArgs().size();

    if (ctorFunc) {
      const auto& paramTypes = ctor ? ctor->paramTypes : std::vector<TypePtr>{};

      std::vector<Value*> ctorArgs =
          generateCtorArgs(ctorFunc, alloca, expr.getArgs(),
                           expr.getArgConversions(), paramTypes);
      // See codegenStackClassInstance: a throwing constructor must be
      // invoked so its exception reaches the enclosing try's landing pad.
      bool ctorCanThrow =
          (ctor && ctor->canThrow) || ctorFunc->hasFnAttribute("sun.canthrow");
      gen_.errorGenerator().emitPossiblyThrowingCall(ctorFunc, ctorArgs,
                                                     ctorCanThrow, "");
    } else if (argCount > 0) {
      // Zeroed storage fully describes a class with no constructor, so an
      // argument-free miss is fine. Arguments that reach no constructor
      // would be dropped on the floor, which is a miscompile.
      logAndThrowError("No constructor to initialize " +
                           classType->getDisplayName() + " with " +
                           std::to_string(argCount) + " argument(s)",
                       expr.getLocation());
    }

    // Track the temporary for deinit ONLY if not moved (ownership
    // transferred)
    if (!expr.isMoved()) {
      auto classTypePtr = std::make_shared<ClassType>(*classType);
      scopes().trackClassAllocation(alloca, "stack.obj", classTypePtr);
    }

    return alloca;
  }

  logAndThrowError("Unknown generic call: " + funcName);
  return nullptr;
}

// -------------------------------------------------------------------
// Struct literal codegen: { field: value, ... }
// -------------------------------------------------------------------

// Builds a class instance field by field. Semantic analysis has already
// checked that the type is a class without an `init`, that every field is
// named exactly once, and that the values are assignable — so this only has
// to lay the bytes down. Returns the object's address, like other class-
// valued expressions.
Value* ClassGenerator::codegen(const sun::ast::StructLiteralAST& expr) {
  auto* classType =
      &sun::codegen::support::requireType<ClassType>(expr, "struct literal");
  llvm::StructType* structType = classType->getStructType(ctx.getContext());

  Function* parentFunc = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* alloca =
      createEntryBlockAlloca(parentFunc, "struct.lit", structType);

  // Every field is assigned below, so no zeroing pass is needed.
  for (size_t i = 0; i < expr.getFields().size(); ++i) {
    const auto& field = expr.getFields()[i];
    const ClassField* classField =
        classType->getField(expr.resolvedFields().at(i));
    if (!classField)
      logAndThrowError("Struct literal field target is not registered");

    Value* value = codegen(*field.value);
    if (!value) return nullptr;

    TypePtr valueType = field.value->getResolvedType();
    value = sun::codegen::support::widenNumericIfNeeded(
        *ctx.builder, typeResolver, value, classField->type, valueType);

    Value* fieldPtr = ctx.builder->CreateStructGEP(
        structType, alloca, classField->index, field.name + ".ptr");
    sun::codegen::support::storeIntoSlot(*ctx.builder, module->getDataLayout(),
                                         fieldPtr, value, classField->type,
                                         classType);
  }

  // Track for deinit at scope exit unless ownership moves to a destination.
  if (!expr.isMoved()) {
    auto classTypePtr = std::make_shared<ClassType>(*classType);
    scopes().trackClassAllocation(alloca, "struct.lit", classTypePtr);
  }

  return alloca;
}

}  // namespace sun::codegen::classes
