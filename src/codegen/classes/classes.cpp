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
#include "semantic_analysis/semantic_scope.h"

using namespace llvm;

namespace layout = sun::codegen::layout;
namespace ops = sun::codegen::ops;

namespace {

// True when a registered specialization still carries a type parameter among
// its type arguments — a shape produced by resolving a template's own
// signature, never a class with a layout.
bool specializationIsAbstract(
    const std::shared_ptr<sun::ClassType>& classType) {
  if (!classType) return false;
  for (const auto& typeArg : classType->getTypeArguments()) {
    if (typeArg && typeArg->isTypeParameter()) return true;
  }
  return false;
}

}  // namespace

// -------------------------------------------------------------------
// Precompiled class codegen (from linked bitcode)
// -------------------------------------------------------------------

Value* ClassGenerator::codegenPrecompiledClass(const ClassDefinitionAST& expr,
                                               const std::string& className) {
  // Still need to register the class type for type checking
  auto classType = typeRegistry->getClass(className);
  if (classType) {
    // Generate specializations for generic methods on this non-generic
    // precompiled class (e.g., HeapAllocator.create<T>)
    CodegenState::ReceiverGuard receiver(state_);
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

  }

  // If the class is also generic, store the AST for later instantiation
  if (expr.isGeneric()) {
    genericClassASTs[className] = &expr;

    // Generate specializations that were added during semantic analysis.
    // Some may be library specializations (already in bitcode) - skip those.
    // Others may be user specializations (e.g., Vec<MyUserClass>) - codegen
    // those.
    for (const auto& [mangledName, specializedAST] :
         expr.getSpecializations()) {
      if (!specializedAST || codegenedClasses.count(mangledName)) {
        continue;
      }

      // Check if this specialization already exists in the precompiled library.
      // Pre-declared specializations (e.g., Vec_i32, Matrix_f64) have their
      // methods declared from the bitcode metadata.
      // New user-triggered specializations (e.g., Vec<u32>, Vec<MyClass>) won't
      // have declarations and need codegen.
      std::string initMethodName = mangledName + "_init";
      if (isPrecompiledFunction(initMethodName)) {
        // Pre-declared library specialization - skip, bitcode will be linked
        continue;
      }

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
// Declare every method of a class (no bodies), so calls can be emitted
// before the class definition is reached
// -------------------------------------------------------------------

void ClassGenerator::declareClassMethods(
    const ClassDefinitionAST& expr,
    const std::shared_ptr<sun::ClassType>& classType) {
  if (!classType) return;

  for (const auto& methodDecl : expr.getMethods()) {
    const FunctionAST& methodFunc = *methodDecl.function;
    const PrototypeAST& proto = methodFunc.getProto();
    const std::string& methodName = proto.getName();

    // A generic method has no signature of its own — declare the
    // specializations semantic analysis created instead.
    if (proto.isGeneric()) {
      for (const auto& [specMangledName, specializedAST] :
           methodFunc.getSpecializations()) {
        if (specializedAST) {
          declareMethodFromAST(*specializedAST, specMangledName);
        }
      }
      continue;
    }

    // Get resolved parameter types for mangled name (overload disambiguation)
    std::vector<sun::TypePtr> paramTypes;
    if (proto.hasResolvedParamTypes()) {
      paramTypes = proto.getResolvedParamTypes();
    }

    // Create mangled method name with param types:
    // ClassName_methodName$type1$type2
    std::string mangledName =
        classType->getMangledMethodName(methodName, paramTypes);

    // Declare the non-generic method using the shared helper
    declareMethodFromAST(methodFunc, mangledName);
  }
}

// Declare the methods of every class a block defines, before any body is
// emitted, so a method may call one of a class declared further down the file
// (the same courtesy the function pre-pass extends to free functions).
void ClassGenerator::declareBlockClassMethods(const ClassDefinitionAST& expr) {
  if (expr.isPrecompiled() || expr.isPartial()) return;

  if (expr.isGeneric()) {
    for (const auto& [mangledName, specializedAST] :
         expr.getSpecializations()) {
      if (!specializedAST) continue;
      if (specializationIsAbstract(typeRegistry->getClass(mangledName)))
        continue;
      declareBlockClassMethods(*specializedAST);
    }
    return;
  }

  std::string className = expr.getName();
  if (auto* resolvedClass = sun::tryGetType<sun::ClassType>(expr)) {
    className = resolvedClass->getMangledName();
  }
  declareClassMethods(expr, typeRegistry->getClass(className));
}

// -------------------------------------------------------------------
// Class definition codegen
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const ClassDefinitionAST& expr) {
  // Get the class name - check if there's a qualified name via resolved type
  // The semantic analyzer may have qualified the name (e.g., sun_SliceRange)
  std::string className = expr.getName();
  if (auto* resolvedClass = sun::tryGetType<sun::ClassType>(expr)) {
    className = resolvedClass->getMangledName();
  }

  // Skip precompiled classes - they come from linked bitcode
  if (expr.isPrecompiled()) {
    return codegenPrecompiledClass(expr, className);
  }

  // Skip partial classes - their methods are merged into the primary class
  if (expr.isPartial()) {
    return ConstantFP::get(ctx.getContext(), APFloat(0.0));
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
      if (!specializedAST || codegenedClasses.count(mangledName)) continue;
      // Resolving a template's own signature instantiates the shape it names
      // — `ref Pair<T>` in `unwrap<T>` yields Pair<T>, whose T is still a
      // type parameter. That shape has no layout to emit; the class the code
      // actually uses is instantiated when unwrap<i32> is.
      if (specializationIsAbstract(typeRegistry->getClass(mangledName)))
        continue;
      codegen(*specializedAST);
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
  CodegenState::ReceiverGuard receiver(state_);
  currentClass = classType;

  // PASS 1: Declare all method functions first (so methods can call each other)
  declareClassMethods(expr, classType);

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
            functions().noteUserDefined(specMangledName);
          }
        }
      }
      continue;
    }

    // Get resolved parameter types for mangled name (overload disambiguation)
    std::vector<sun::TypePtr> paramTypes;
    if (proto.hasResolvedParamTypes()) {
      paramTypes = proto.getResolvedParamTypes();
    }

    // Create mangled method name with param types:
    // ClassName_methodName$type1$type2
    std::string mangledName =
        classType->getMangledMethodName(proto.getName(), paramTypes);
    generateMethodBody(methodFunc, mangledName);
    // Track user-defined methods for IR filtering
    if (isUserDefined) {
      functions().noteUserDefined(mangledName);
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
      // Include param types for overload disambiguation
      std::string mangledName = classType->getMangledMethodName(
          interfaceMethod.name, interfaceMethod.paramTypes);
      std::string defaultMangledName =
          interfaceType->getMangledDefaultMethodName(interfaceMethod.name);

      // Build parameter types (closure ptr as first parameter)
      std::vector<llvm::Type*> paramTypes;
      paramTypes.push_back(
          PointerType::getUnqual(ctx.getContext()));  // closure
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
      // No subprogram on this wrapper: it must carry no debug locations
      debugInfo.clearLocation(*ctx.builder);

      // Get the default implementation function
      Function* defaultFunc = module->getFunction(defaultMangledName);
      if (!defaultFunc) {
        logAndThrowError("Default implementation not found: " +
                         defaultMangledName);
        continue;
      }

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
    getOrCreateInterfaceVtable(classType.get(), interfaceType.get());
  }

  // Class definitions return void
  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}

// -------------------------------------------------------------------
// Declare a method function from a specialized AST (no body generated)
// -------------------------------------------------------------------

Function* ClassGenerator::declareMethodFromAST(
    const FunctionAST& specializedAST, const std::string& mangledName) {
  // Skip if already declared
  if (Function* existing = module->getFunction(mangledName)) {
    return existing;
  }

  const PrototypeAST& proto = specializedAST.getProto();

  // Build parameter types: closure ptr first ({ func, env } with the
  // receiver in env), then regular params
  std::vector<llvm::Type*> paramTypes;
  paramTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // closure

  if (!proto.hasResolvedParamTypes()) {
    logAndThrowError(
        "Method parameter types not resolved by semantic analysis: " +
        mangledName);
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
    returnType = Type::getVoidTy(ctx.getContext());
  } else {
    logAndThrowError("Method return type not resolved by semantic analysis: " +
                     mangledName);
    return nullptr;
  }

  // With native exceptions a throwing method ('T, IError') returns plain T; the
  // marker only means it may unwind.

  // Create the function declaration
  FunctionType* funcType = FunctionType::get(returnType, paramTypes, false);
  Function* func = Function::Create(funcType, Function::ExternalLinkage,
                                    mangledName, module);
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

  // Register 'this' in the current scope so the body can find it
  scopes().back().variables["this"] = thisAlloca;
  debugInfo.declareThisParameter(*ctx.builder, thisAlloca, currentClass);
}

// -------------------------------------------------------------------
// Generate a method body for an already-declared function
// -------------------------------------------------------------------

void ClassGenerator::generateMethodBody(const FunctionAST& methodFunc,
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

  // Save and set error handling context. With native exceptions a throwing
  // method returns plain T, so the value type is just the return type.
  CodegenState::ReturnGuard returns(state_);
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
  const std::vector<sun::TypePtr> paramTypes = proto.getAllParamTypes();
  if (!proto.hasResolvedParamTypes() ||
      paramTypes.size() != paramNames.size()) {
    logAndThrowError(
        "Method parameter types not resolved by semantic analysis: " +
        mangledName);
    return;
  }
  const size_t fixedCount = proto.getArgs().size();

  for (size_t i = 0; i < paramNames.size(); ++i) {
    const std::string& argName = paramNames[i];
    llvm::Type* argLLVMType = typeResolver.resolve(paramTypes[i]);

    AllocaInst* alloca =
        ctx.builder->CreateAlloca(argLLVMType, nullptr, argName);
    ctx.builder->CreateStore(&*argIt, alloca);
    scopes().back().variables[argName] = alloca;
    // A pack element has no annotation in the source to point a debug entry at
    if (i < fixedCount) {
      debugDeclareParam(alloca, argName, proto, static_cast<unsigned>(i),
                        /*argNoBase=*/2);
    }
    scopes().trackOwnedParam(alloca, argName, paramTypes[i]);
    ++argIt;
  }

  // Generate the method body
  codegen(methodFunc.getBody());

  // Add implicit return if no explicit return. A non-void body whose last
  // statement always returns/throws (e.g. a match with terminating arms)
  // leaves an unreachable tail block.
  if (!ctx.builder->GetInsertBlock()->getTerminator()) {
    if (returnType->isVoidTy()) {
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

Value* ClassGenerator::codegen(const ThisExprAST& expr) {
  if (!thisPtr) {
    logAndThrowError("'this' used outside of a class method");
    return nullptr;
  }
  return thisPtr;
}

// -------------------------------------------------------------------
// Module member helpers
// -------------------------------------------------------------------

// The global backing `mod.name`, or null when the object is not a module or
// the member names something other than a global variable. `symbol` is the
// name semantic analysis took from the member's own declaration; codegen never
// rebuilds it from the module path, since only the declaration knows the
// library-hash scope the symbol was emitted under.
GlobalVariable* ClassGenerator::moduleMemberGlobal(const ExprAST& object,
                                                   const std::string& symbol) {
  sun::TypePtr objectType = object.getResolvedType();
  if (!objectType || !objectType->isModule() || symbol.empty()) return nullptr;
  return module->getGlobalVariable(symbol);
}

// -------------------------------------------------------------------
// Member access codegen (field read)
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const MemberAccessAST& expr) {
  const std::string& memberName = expr.getMemberName();

  // Handle module member access: mod_x.mod_y or mod_x.var
  sun::TypePtr objectType = expr.getObject()->getResolvedType();
  if (auto* moduleType = sun::tryGetType<sun::ModuleType>(objectType)) {
    // Check if the result type is also a module (nested module access)
    if (sun::tryGetType<sun::ModuleType>(expr)) {
      // Return null sentinel - next member access will handle it
      return llvm::ConstantPointerNull::get(
          llvm::PointerType::getUnqual(ctx.getContext()));
    }

    // Semantic analysis took this from the member's own declaration, so it
    // names the symbol that declaration emitted, library-hash scope included
    const std::string qualifiedName = expr.getQualifiedName().mangled();

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

  // Check for enum variant access: EnumName.VariantName. Prefer the
  // sema-resolved object type (handles generic specializations like
  // Option.None, whose object resolves to Option_i32); fall back to the
  // registry for the plain-name path, without auto-creating entries.
  if (expr.getObject()->getType() == ASTNodeType::VARIABLE_REFERENCE) {
    const auto& varRef =
        static_cast<const VariableReferenceAST&>(*expr.getObject());
    std::shared_ptr<sun::EnumType> enumType =
        sun::tryGetTypePtr<sun::EnumType>(*expr.getObject());
    if (!enumType && typeRegistry->hasEnum(varRef.getName())) {
      enumType = typeRegistry->getEnum(varRef.getName());
    }
    if (enumType && enumType->getNumVariants() > 0) {
      const auto* variant = enumType->getVariant(memberName);
      if (variant) {
        return codegenEnumVariantAccess(*enumType, *variant);
      }
    }
  }

  // Refresh objectType in case it was not set from module handling above
  if (!objectType) {
    objectType = expr.getObject()->getResolvedType();
  }

  // Handle array.shape() - returns a 1D array of dimension sizes
  if (memberName == "shape" &&
      sun::tryGetType<sun::ArrayType>(sun::unwrapRef(objectType))) {
    return codegenArrayShape(expr);
  }

  // Resolve the object down to (pointer, class type)
  auto [objectPtr, classType] = codegenObjectPtr(*expr.getObject());
  if (!objectPtr) return nullptr;
  if (!classType) {
    logAndThrowError("Member access on non-class type");
    return nullptr;
  }

  // Check if it's a field access
  const sun::ClassField* field = classType->getField(memberName);
  if (field) {
    Value* fieldPtr = layout::fieldPtr(*ctx.builder, classType, objectPtr, *field, memberName);

    // For class-typed and payload-enum fields (embedded structs), return the
    // pointer to the embedded struct (struct values in Sun are pointers)
    if (field->type->isClass() || CodegenVisitor::isPayloadEnum(field->type)) {
      return fieldPtr;
    }

    // Load the field value
    llvm::Type* fieldLLVMType = field->type->toLLVMType(ctx.getContext());
    return ctx.builder->CreateAlignedLoad(fieldLLVMType, fieldPtr,
                                          layout::fieldAlign(classType, fieldLLVMType, module->getDataLayout()),
                                          memberName + ".val");
  }

  // Bound method reference: a method in value position materializes the
  // closure value { methodFn, objectPtr }
  if (expr.isBoundMethodRef()) {
    return codegenBoundMethodReference(expr, objectPtr, classType);
  }

  // It's a method - just return the object pointer
  // The actual method call will be handled in CallExprAST
  return objectPtr;
}

// -------------------------------------------------------------------
// Bound method reference codegen: obj.method in value position
// Produces a closure struct VALUE { methodFn, objectPtr } (lambda ABI)
// -------------------------------------------------------------------

Value* ClassGenerator::codegenBoundMethodReference(const MemberAccessAST& expr,
                                                   Value* objectPtr,
                                                   sun::ClassType* classType) {
  auto& lambdaType =
      sun::requireType<sun::LambdaType>(expr, "bound method reference");
  const std::string& methodName = expr.getMemberName();

  // Semantic analysis picked the exact overload; its param types are the
  // lambda's param types.
  const sun::ClassMethod* method =
      classType->getMethodForArgs(methodName, lambdaType.getParamTypes());
  if (!method) {
    logAndThrowError("Unknown method: " + methodName + " on class " +
                     classType->getDisplayName());
    return nullptr;
  }

  Function* methodFunc = functions().getOrDeclareMethodFunction(
      classType->getMangledMethodName(methodName, method->paramTypes),
      method->paramTypes, method->returnType, method->canThrow);

  return materializeMethodClosureValue(methodFunc, objectPtr);
}

// -------------------------------------------------------------------
// Stack-allocated class instance codegen: ClassName(args...)
// -------------------------------------------------------------------

Value* ClassGenerator::codegenStackClassInstance(const CallExprAST& expr,
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
  ConstructorLookup ctor = lookupConstructor(&classType, expr.getArgs());

  Function* ctorFunc = nullptr;
  size_t argCount = expr.getArgs().size();

  // Find the constructor; declare an external if the init method exists but
  // isn't in the module yet (precompiled classes linked later)
  Function* candidate =
      ctor.method ? functions().getOrDeclareMethodFunction(
                        ctor.mangledName, ctor.method->paramTypes,
                        ctor.method->returnType, ctor.method->canThrow)
                  : module->getFunction(ctor.mangledName);

  if (candidate && candidate->arg_size() == argCount + 1) {
    ctorFunc = candidate;
  }

  if (ctorFunc) {
    const auto& paramTypes =
        ctor.method ? ctor.method->paramTypes : std::vector<sun::TypePtr>{};

    std::vector<Value*> ctorArgs = generateCtorArgs(
        ctorFunc, alloca, expr.getArgs(), expr.getArgConversions(), paramTypes);
    ctx.builder->CreateCall(ctorFunc, ctorArgs);
  }

  // Track the temporary for deinit ONLY if not moved (ownership
  // transferred). The borrow checker marks temporaries as moved when
  // assigned to a variable or field. Moved temporaries are owned by the
  // destination, which will call deinit. Non-moved temporaries must be
  // deinited here.
  if (!expr.isMoved()) {
    auto classTypePtr = std::make_shared<sun::ClassType>(classType);
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
    const std::vector<sun::ArgConversion>& conversions,
    const std::vector<sun::TypePtr>& paramTypes) {
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
// Constructor lookup helpers
// -------------------------------------------------------------------

ClassGenerator::ConstructorLookup ClassGenerator::lookupConstructor(
    sun::ClassType* classType,
    const std::vector<std::unique_ptr<ExprAST>>& args) {
  // Collect argument types from AST nodes
  std::vector<sun::TypePtr> argTypes;
  argTypes.reserve(args.size());
  for (const auto& arg : args) {
    argTypes.push_back(arg->getResolvedType());
  }
  return lookupConstructor(classType, argTypes);
}

ClassGenerator::ConstructorLookup ClassGenerator::lookupConstructor(
    sun::ClassType* classType, const std::vector<sun::TypePtr>& argTypes) {
  ConstructorLookup result;

  // Look up the init method that matches the argument types
  const sun::ClassMethod* initMethod =
      classType->getMethodForArgs("init", argTypes);

  if (initMethod) {
    result.method = initMethod;
    result.mangledName =
        classType->getMangledMethodName("init", initMethod->paramTypes);
  } else {
    // No matching overload - use default mangled name (no params)
    result.mangledName = classType->getMangledMethodName("init");
  }

  return result;
}

// -------------------------------------------------------------------
// Member assignment codegen (field write)
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const MemberAssignmentAST& expr) {
  // mod.global = value: the module is compile-time only, so this writes the
  // global directly
  if (GlobalVariable* gv = moduleMemberGlobal(
          *expr.getObject(), expr.getQualifiedName().mangled())) {
    Value* value = codegen(*expr.getValue());
    if (!value) return nullptr;
    assignToVariableSlot(gv, value,
                         sun::unwrapRef(expr.getValue()->getResolvedType()),
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
  if (auto* refType = sun::tryGetType<sun::ReferenceType>(valueSunType)) {
    if (field->type->isArray() && refType->getReferencedType()->isArray()) {
      // value is a pointer to the fat struct, load it
      llvm::StructType* fatType =
          sun::ArrayType::getArrayStructType(ctx.getContext());
      value = ctx.builder->CreateAlignedLoad(
          fatType, value, module->getDataLayout().getABITypeAlign(fatType),
          "arr.fat.load");
      valueType = value->getType();
    }
  }

  // Generate GEP to access the field
  Value* fieldPtr =
      layout::fieldPtr(*ctx.builder, classType, objectPtr, *field, memberName + ".ptr");

  // Payload-enum fields: the value arrives as a storage pointer. Drop the
  // overwritten field first (a no-op on freshly zeroed storage: tag 0 with
  // zeroed payloads drops cleanly), then MOVE the source in — never an
  // implicit copy.
  if (CodegenVisitor::isPayloadEnum(field->type)) {
    Value* structVal = value;
    if (value->getType()->isPointerTy()) {
      scopes().emitDropInPlace(field->type, fieldPtr, memberName);
      structVal = gen_.applyMoveSemantics(value, field->type);
    }
    ctx.builder->CreateStore(structVal, fieldPtr);
    return structVal;
  }

  // Handle class-typed fields: the source instance MOVES into the field.
  // The overwritten field value is dropped first (a no-op on freshly
  // zeroed storage), then the source is copied in and invalidated.
  if (auto* fieldClassType = sun::tryGetType<sun::ClassType>(field->type)) {
    llvm::StructType* fieldStructType =
        fieldClassType->getStructType(ctx.getContext());
    const DataLayout& DL = module->getDataLayout();
    uint64_t structSize = DL.getTypeAllocSize(fieldStructType);

    // Drop whatever the field currently holds
    scopes().emitDropInPlace(field->type, fieldPtr, memberName);

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
    ctx.builder->CreateMemCpy(fieldPtr, layout::fieldAlign(classType, fieldStructType, module->getDataLayout()),
                              value, srcAlign, structSize);
    if (sourceIsAddressable) {
      // Move: the field owns the payload now. Release the source's tracking
      // entry and zero it so its own drop is a no-op.
      scopes().markClassAllocationAsDeinited(value);
      ctx.builder->CreateMemSet(
          value, ConstantInt::get(Type::getInt8Ty(ctx.getContext()), 0),
          structSize, srcAlign);
    }
    return value;
  }

  // Handle implicit widening for literals assigned to wider types
  if (valueType != fieldLLVMType) {
    ASTNodeType valueKind = expr.getValue()->getType();
    bool valueIsLiteral = valueKind == ASTNodeType::NUMBER ||
                          valueKind == ASTNodeType::CHAR_LITERAL;

    if (valueIsLiteral) {
      // Integer widening
      if (valueType->isIntegerTy() && fieldLLVMType->isIntegerTy()) {
        unsigned valueBits = valueType->getIntegerBitWidth();
        unsigned fieldBits = fieldLLVMType->getIntegerBitWidth();
        if (valueBits < fieldBits) {
          value = ops::extendInt(*ctx.builder, value, fieldLLVMType,
                            expr.getValue()->getResolvedType());
        }
      }
      // Float widening
      else if (valueType->isFloatTy() && fieldLLVMType->isDoubleTy()) {
        value = ctx.builder->CreateFPExt(value, fieldLLVMType, "widen");
      }
      // Float literal into an f32 field: literals default to f64
      else if (valueType->isDoubleTy() && fieldLLVMType->isFloatTy()) {
        value = ctx.builder->CreateFPTrunc(value, fieldLLVMType, "narrow");
      }
    }
  }

  // Store the value
  ctx.builder->CreateAlignedStore(value, fieldPtr,
                                  layout::fieldAlign(classType, fieldLLVMType, module->getDataLayout()));

  // Return the value (like C assignment)
  return value;
}

// -------------------------------------------------------------------
// Interface definition codegen
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const InterfaceDefinitionAST& expr) {
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

    // Build the method parameter types (closure ptr first - the receiver
    // lives in the closure's env slot)
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(PointerType::getUnqual(ctx.getContext()));  // closure

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
    argIt->setName("closure");
    ++argIt;

    for (const auto& [argName, argType] : proto.getArgs()) {
      argIt->setName(argName);
      ++argIt;
    }

    // Create entry basic block
    BasicBlock* BB = BasicBlock::Create(ctx.getContext(), "entry", func);
    ctx.builder->SetInsertPoint(BB);

    debugInfo.enterFunction(*ctx.builder, func, proto.getName(),
                            proto.getLocation());

    // Create a new scope for the method
    scopes().push().isFunctionBoundary = true;

    emitMethodPrologueThis(func);

    // Store other parameters
    argIt = func->arg_begin();
    ++argIt;  // Skip closure

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
      scopes().back().variables[argName] = alloca;
      debugDeclareParam(alloca, argName, proto, static_cast<unsigned>(paramIdx),
                        /*argNoBase=*/2);
      ++argIt;
      ++paramIdx;
    }

    // Generate the method body
    codegen(methodFunc.getBody());

    // Add implicit return if no explicit return (see codegenMethod)
    if (!ctx.builder->GetInsertBlock()->getTerminator()) {
      if (returnType->isVoidTy()) {
        ctx.builder->CreateRetVoid();
      } else {
        ctx.builder->CreateUnreachable();
      }
    }

    scopes().pop();
    thisPtr = nullptr;

    debugInfo.exitFunction(func);

    // Verify the function
    verifyFunction(*func);
  }

  // Interface definitions return void
  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}

// -------------------------------------------------------------------
// Enum definition codegen
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const EnumDefinitionAST& expr) {
  // Enum definitions are already fully registered by the semantic analyzer
  // in the TypeRegistry. Payload-free enums are represented as i32 constants
  // emitted inline when variants are referenced.

  // Generic templates generate no code themselves; walk the specializations
  // recorded by the semantic analyzer (mirrors generic classes) and build
  // their storage structs.
  if (expr.isGeneric()) {
    for (const auto& [mangledName, specialized] : expr.getSpecializations()) {
      if (specialized && specialized->hasPayload()) {
        typeResolver.getEnumStorageType(*specialized);
      }
    }
    return ConstantFP::get(ctx.getContext(), APFloat(0.0));
  }

  // Payload enums: eagerly build the storage struct so any later
  // ClassType::getStructType embedding an enum field (which cannot reach the
  // resolver) can serve it from the EnumType cache.
  if (expr.hasAnyPayload()) {
    if (auto enumType = typeRegistry->getEnum(expr.getName())) {
      typeResolver.getEnumStorageType(*enumType);
    }
  }

  return ConstantFP::get(ctx.getContext(), APFloat(0.0));
}

// -------------------------------------------------------------------
// Generic call codegen: name<Type>(args...)
// For standalone generic functions (not methods)
// -------------------------------------------------------------------

Value* ClassGenerator::codegen(const GenericCallAST& expr) {
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
      return intrinsics().codegenSizeofIntrinsic(getFirstTypeArg());
    case sun::Intrinsic::Init:
      return intrinsics().codegenInitIntrinsic(getFirstTypeArg(), expr.getArgs());
    case sun::Intrinsic::Load:
      return intrinsics().codegenLoadIntrinsic(getFirstTypeArg(), expr.getArgs());
    case sun::Intrinsic::Store:
      return intrinsics().codegenStoreIntrinsic(getFirstTypeArg(), expr.getArgs());
    case sun::Intrinsic::PtrAsRaw:
      return intrinsics().codegenPtrAsRawIntrinsic(expr.getArgs());
    case sun::Intrinsic::AddressOf:
      return intrinsics().codegenAddressOfIntrinsic(expr.getArgs());
    case sun::Intrinsic::ToRef:
      return intrinsics().codegenToRefIntrinsic(expr.getArgs());
    case sun::Intrinsic::Is:
      // _is<T> uses the type name for type trait checks (e.g., "_Integer")
      return intrinsics().codegenIsIntrinsic(typeArgs[0]->baseName, expr.getArgs());
    case sun::Intrinsic::Deinit:
      return intrinsics().codegenDeinitIntrinsic(getFirstTypeArg(), expr.getArgs());
    case sun::Intrinsic::Convert:
      return intrinsics().codegenConvertIntrinsic(getFirstTypeArg(), expr.getArgs());
    case sun::Intrinsic::Bitcast:
      return intrinsics().codegenBitcastIntrinsic(getFirstTypeArg(), expr.getArgs());
    case sun::Intrinsic::Spawn:
      return intrinsics().codegenSpawnIntrinsic(getFirstTypeArg(), expr.getResolvedType(),
                                   expr.getArgs(), expr.getArgConversions());
    case sun::Intrinsic::ThreadJoin:
      return intrinsics().codegenThreadJoinIntrinsic(getFirstTypeArg(), expr.getArgs(),
                                        /*dropResult=*/false);
    case sun::Intrinsic::ThreadJoinDrop:
      return intrinsics().codegenThreadJoinIntrinsic(getFirstTypeArg(), expr.getArgs(),
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
    // Call exactly what semantic analysis instantiated. Rebuilding the name
    // here would mean reproducing the template's own mangled name (which
    // carries enclosing function context, e.g. outer_i32_inner) plus any pack
    // suffix, and any drift makes the call reach for a missing symbol.
    if (!expr.hasSpecializationName()) {
      logAndThrowError(
          "Generic call specialization not recorded by semantic analysis: " +
          funcName);
      return nullptr;
    }
    std::string mangledName = expr.getSpecializationName().mangled();

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
    if (AllocaInst* envPtr = scopes().findVariable(mangledName)) {
      argValues.push_back(envPtr);
    }

    // Sun-level signature of the specialization. A `ref T` parameter wants the
    // referent's address, a by-value compound moves — the same coercions any
    // other direct call applies, so the argument build is shared.
    std::vector<sun::TypePtr> specParamTypes;
    bool canThrow = specializedFunc->hasFnAttribute("sun.canthrow");
    if (auto specialization = genericFuncAST->getSpecialization(mangledName)) {
      const PrototypeAST& specProto = specialization->getProto();
      // A pack's elements are parameters too, after the fixed ones
      specParamTypes = specProto.getAllParamTypes();
      // The specialization may still be a forward declaration here, in which
      // case it carries no attribute yet — its prototype is the authority.
      canThrow = canThrow || (specProto.hasReturnType() &&
                              specProto.getReturnType()->canError);
    }

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
  const std::vector<sun::TypePtr>& resolvedTypeArgs =
      expr.getResolvedTypeArgs();

  // Get the mangled class name - use resolved type if available to handle
  // qualified names from using imports (e.g., Unique -> sun_Unique)
  std::string baseName = funcName;
  if (auto* resolvedClass = sun::tryGetType<sun::ClassType>(expr)) {
    baseName = resolvedClass->getMangledName();
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

      // Call the constructor the arguments select. Resolving on the name
      // alone would always pick the first `init`, so a class with several
      // of them would construct through the wrong one — or, when the arity
      // did not match, through none at all.
      ConstructorLookup ctor =
          lookupConstructor(classType.get(), expr.getArgs());

      Function* ctorFunc = nullptr;
      size_t argCount = expr.getArgs().size();

      // Find the constructor; declare an external if the init method exists
      // but isn't in the module yet (class codegen hasn't run)
      Function* candidate =
          ctor.method ? functions().getOrDeclareMethodFunction(
                            ctor.mangledName, ctor.method->paramTypes,
                            ctor.method->returnType, ctor.method->canThrow)
                      : module->getFunction(ctor.mangledName);

      if (candidate && candidate->arg_size() == argCount + 1) {
        ctorFunc = candidate;
      }

      if (ctorFunc) {
        const auto& paramTypes =
            ctor.method ? ctor.method->paramTypes : std::vector<sun::TypePtr>{};

        std::vector<Value*> ctorArgs =
            generateCtorArgs(ctorFunc, alloca, expr.getArgs(),
                             expr.getArgConversions(), paramTypes);
        ctx.builder->CreateCall(ctorFunc, ctorArgs);
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
        auto classTypePtr = std::make_shared<sun::ClassType>(*classType);
        scopes().trackClassAllocation(alloca, "stack.obj", classTypePtr);
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
    // Resolve on the argument types, not the name alone — see the matching
    // comment above.
    ConstructorLookup ctor =
        lookupConstructor(fallbackClassType.get(), expr.getArgs());

    Function* ctorFunc = nullptr;
    size_t argCount = expr.getArgs().size();

    // Find the constructor; declare an external if the init method exists
    // but isn't in the module yet
    Function* candidate =
        ctor.method ? functions().getOrDeclareMethodFunction(
                          ctor.mangledName, ctor.method->paramTypes,
                          ctor.method->returnType, ctor.method->canThrow)
                    : module->getFunction(ctor.mangledName);

    if (candidate && candidate->arg_size() == argCount + 1) {
      ctorFunc = candidate;
    }

    if (ctorFunc) {
      const auto& paramTypes =
          ctor.method ? ctor.method->paramTypes : std::vector<sun::TypePtr>{};

      std::vector<Value*> ctorArgs =
          generateCtorArgs(ctorFunc, alloca, expr.getArgs(),
                           expr.getArgConversions(), paramTypes);
      ctx.builder->CreateCall(ctorFunc, ctorArgs);
    } else if (argCount > 0) {
      logAndThrowError("No constructor to initialize " +
                           fallbackClassType->getDisplayName() + " with " +
                           std::to_string(argCount) + " argument(s)",
                       expr.getLocation());
    }

    // Track the temporary for deinit ONLY if not moved (ownership
    // transferred)
    if (!expr.isMoved()) {
      auto classTypePtr = std::make_shared<sun::ClassType>(*fallbackClassType);
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
Value* ClassGenerator::codegen(const StructLiteralAST& expr) {
  auto* classType = &sun::requireType<sun::ClassType>(expr, "struct literal");
  llvm::StructType* structType = classType->getStructType(ctx.getContext());

  Function* parentFunc = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* alloca =
      createEntryBlockAlloca(parentFunc, "struct.lit", structType);

  // Every field is assigned below, so no zeroing pass is needed.
  for (const auto& field : expr.getFields()) {
    const sun::ClassField* classField = classType->getField(field.name);
    if (!classField) continue;  // rejected in semantic analysis

    Value* value = codegen(*field.value);
    if (!value) return nullptr;

    sun::TypePtr valueType = field.value->getResolvedType();
    value = ops::widenNumericIfNeeded(*ctx.builder, typeResolver, value, classField->type, valueType);

    Value* fieldPtr = ctx.builder->CreateStructGEP(
        structType, alloca, classField->index, field.name + ".ptr");
    layout::storeIntoSlot(*ctx.builder, module->getDataLayout(), fieldPtr, value, classField->type, classType);
  }

  // Track for deinit at scope exit unless ownership moves to a destination.
  if (!expr.isMoved()) {
    auto classTypePtr = std::make_shared<sun::ClassType>(*classType);
    scopes().trackClassAllocation(alloca, "struct.lit", classTypePtr);
  }

  return alloca;
}
