// variable_creation.cpp - Variable creation codegen methods

#include <llvm/Transforms/Utils/ModuleUtils.h>

#include "ast.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics.h"
#include "codegen/support/scalar_ops.h"
#include "codegen/support/struct_access.h"
#include "codegen/variables/variable_generator.h"
#include "semantic_analysis/globals.h"

using sun::types::ClassType;
using sun::types::ReferenceType;
using sun::types::TypePtr;

using sun::ast::ASTNodeType;
using sun::ast::ExprAST;
using sun::ast::VariableCreationAST;
using sun::support::logAndThrowError;

using namespace llvm;

/** Generates storage and access operations for Sun variables. */
namespace sun::codegen::variables {

/** Helpers private to variable creation. */
namespace {

/**
 * Deepest chain of imports a startup function can be ordered across. Startup
 * priorities above the default are not portable, so the range is carved out
 * just below it.
 */
constexpr uint32_t kMaxStaticInitOrder = 255;

/**
 * Startup priority of a bundle that imports nothing. The default priority,
 * which the top of the deepest allowed chain lands on, is the highest the
 * platform linkers order reliably.
 */
constexpr uint32_t kStaticInitBasePriority = 65535 - kMaxStaticInitOrder;

}  // namespace

// -------------------------------------------------------------------
// Global variable creation
// -------------------------------------------------------------------

GlobalVariable* VariableGenerator::createGlobalVariable(
    sun::semantic_analysis::DeclarationId id, const std::string& name,
    llvm::Type* type, llvm::Constant* initializer) {
  GlobalVariable* gv = new GlobalVariable(
      *module, type, false, GlobalValue::ExternalLinkage, initializer, name);
  return bindGlobal(id, gv);
}

// -------------------------------------------------------------------
// Variable creation codegen
// -------------------------------------------------------------------

void VariableGenerator::declareBlockGlobals(
    const sun::ast::BlockExprAST& block) {
  sun::semantic_analysis::forEachGlobalDeclaration(
      block, [&](const VariableCreationAST& variable) {
        // Inside a function, a variable with a value is an ordinary local,
        // emitted in its turn
        if (variable.isCExtern() || variable.isPrecompiled() ||
            (scopes().empty() && variable.getValue()))
          codegen(variable);
      });
}

Value* VariableGenerator::codegen(const VariableCreationAST& expr) {
  // A global's storage is created up front, with the rest of its block's
  // globals, so by its own turn there is nothing left to do
  if (scopes().empty()) {
    if (GlobalVariable* declared = findGlobal(expr.getDeclarationId()))
      return declared;
  }

  // Get the type from the resolved type set by semantic analyzer
  TypePtr varSunType = expr.getResolvedType();
  if (!varSunType) {
    logAndThrowError(
        "Variable declaration has no type (semantic analysis may have failed)");
  }
  if (varSunType->isTypeParameter()) {
    logAndThrowError(
        "Semantic analysis failed to substitute type parameter for variable: " +
        expr.getName());
  }

  // Use qualified name from semantic analysis
  std::string varName =
      (scopes().empty()
           ? state_.declarationSymbol(expr.getDeclarationId(), "global")
           : expr.getName());

  if (expr.isCExtern()) {
    return bindGlobal(expr.getDeclarationId(),
                      gen_.externCEmitter().declareGlobal(
                          expr, typeResolver.resolve(varSunType)));
  }

  // A global imported from a .moon is defined in the bundle's bitcode, which
  // is linked in. Declare it so references resolve; defining it here would
  // give the program a second, uninitialized copy.
  if (expr.isPrecompiled()) {
    if (GlobalVariable* existing = module->getGlobalVariable(varName)) {
      return bindGlobal(expr.getDeclarationId(), existing);
    }
    return bindGlobal(
        expr.getDeclarationId(),
        new GlobalVariable(*module, typeResolver.resolve(varSunType),
                           /*isConstant=*/false, GlobalValue::ExternalLinkage,
                           /*Initializer=*/nullptr, varName));
  }

  if (scopes().empty() && module->getGlobalVariable(varName)) {
    logAndThrowError("Cannot redeclare global variable: " + varName);
  }

  bool isLambdaType = varSunType->isLambda();

  // If the value is a lambda literal, generate it and store its pointer
  if (expr.getValue()->isLambda()) {
    if (!isLambdaType) {
      logAndThrowError(
          "Type mismatch: expected lambda type for lambda literal variable: " +
          varName);
    }
    return genFunctionVariable(expr);
  }

  // Resolve the LLVM type (handles function types -> ptr, lambda types ->
  // closure struct)
  llvm::Type* varType = typeResolver.resolve(varSunType);

  if (scopes().empty()) {
    if (sun::codegen::CodegenVisitor::isPayloadEnum(varSunType)) {
      logAndThrowError(
          "Global variables of payload-carrying enum types are not yet "
          "supported",
          expr.getLocation());
    }
    // A class value is built by its constructor, which runs at startup
    if (auto* classType =
            sun::codegen::support::tryGetType<ClassType>(varSunType)) {
      return emitStartupGlobal(expr,
                               classType->getStructType(ctx.getContext()));
    }
    // Analysis decided where every other file-scope variable gets its value
    const auto* decision = expr.getGlobalInit();
    if (!decision) {
      logAndThrowError(
          "Internal error: analysis made no decision about how "
          "global variable '" +
              expr.getName() + "' is initialized",
          expr.getLocation());
    }
    using sun::semantic_analysis::constants::GlobalInitKind;
    if (decision->kind == GlobalInitKind::Image && decision->value)
      return emitImageGlobal(expr, varType, *decision->value);
    return emitStartupGlobal(expr, varType);
  }

  return genLocalVar(expr, varType);
}

// -------------------------------------------------------------------
// Lambda variable creation
// -------------------------------------------------------------------

Value* VariableGenerator::genFunctionVariable(const VariableCreationAST& expr) {
  if (!expr.getValue()->isLambda()) {
    logAndThrowError("Expected lambda literal for lambda type variable: " +
                     expr.getName());
  }

  // Use qualified name from semantic analysis
  std::string varName =
      (scopes().empty()
           ? state_.declarationSymbol(expr.getDeclarationId(), "global")
           : expr.getName());

  // Generate the lambda
  auto& lambdaAst =
      static_cast<sun::ast::LambdaAST&>(const_cast<ExprAST&>(*expr.getValue()));
  llvm::Value* resultPtr = functionGen().codegenLambda(lambdaAst);

  if (!resultPtr) {
    logAndThrowError("Failed to generate lambda: " + varName);
  }

  // Lambda: resultPtr is an alloca containing the closure struct (or constant
  // for global)
  llvm::Type* varType = resultPtr->getType();

  if (scopes().empty()) {
    // Top-level: use global variable for closure struct
    createGlobalVariable(expr.getDeclarationId(), varName, varType,
                         llvm::cast<llvm::Constant>(resultPtr));
  } else {
    // Inside a function: resultPtr is already an alloca from createFatClosure
    // that holds the closure struct. Just register it in the scope.
    auto& scope = scopes().back().variables;
    if (auto* fatAlloca = llvm::dyn_cast<AllocaInst>(resultPtr)) {
      // resultPtr is already an alloca containing the closure struct - use it
      // directly
      fatAlloca->setName(varName);
      scope[expr.getDeclarationId()] = fatAlloca;
      debugDeclareLocal(fatAlloca, expr.getName(), expr.getResolvedType(),
                        expr.getLocation());
    } else {
      // Fallback: create a new alloca and store the value
      Function* currentFunc = ctx.builder->GetInsertBlock()->getParent();
      AllocaInst* alloca =
          createEntryBlockAlloca(currentFunc, varName, varType);
      ctx.builder->CreateStore(resultPtr, alloca);
      scope[expr.getDeclarationId()] = alloca;
      debugDeclareLocal(alloca, expr.getName(), expr.getResolvedType(),
                        expr.getLocation());
    }
  }
  return resultPtr;
}

// -------------------------------------------------------------------
// Local variable creation
// -------------------------------------------------------------------

llvm::Value* VariableGenerator::genLocalVar(const VariableCreationAST& expr,
                                            llvm::Type* varType) {
  // A reference variable binds the referent's address rather than reading
  // through it — that is what makes `var r = v.get(i); r = 5;` write into
  // the Vec. codegen() would read instead (see loadIfRef).
  TypePtr declaredType = expr.getResolvedType();
  Value* value = declaredType && declaredType->isReference()
                     ? codegenBorrowAddress(*expr.getValue())
                     : nullptr;
  if (!value) value = codegen(*expr.getValue());
  if (!value) return nullptr;
  if (declaredType && declaredType->isReference() &&
      sun::types::areDifferentInterfaces(expr.getValue()->getResolvedType(),
                                         declaredType)) {
    value = classes().createBorrowedInterfaceUpcast(
        value, expr.getValue()->getResolvedType(), declaredType);
  }

  // Inside a function: use local alloca
  auto& scope = scopes().back().variables;
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  TypePtr varSunType = expr.getResolvedType();

  // Payload enums: struct values handled by pointer. The variable OWNS its
  // storage: fresh temporaries are adopted, named sources are MOVED (never
  // implicitly copied), and the result is drop-tracked when payloads own
  // heap resources.
  if (varSunType && sun::codegen::CodegenVisitor::isPayloadEnum(varSunType)) {
    auto& enumType = static_cast<sun::types::EnumType&>(*varSunType);
    llvm::StructType* storageTy = typeResolver.getEnumStorageType(enumType);

    // A fresh temporary (construction, materialized call return) can be
    // adopted directly; a named source (variable, field) is moved out of
    ASTNodeType valueKind = expr.getValue()->getType();
    bool valueIsFreshTemp = valueKind != ASTNodeType::VARIABLE_REFERENCE &&
                            valueKind != ASTNodeType::MEMBER_ACCESS;
    if (valueIsFreshTemp) {
      if (auto* allocaValue = dyn_cast<AllocaInst>(value)) {
        allocaValue->setName(expr.getName());
        scope[expr.getDeclarationId()] = allocaValue;
        debugDeclareLocal(allocaValue, expr.getName(), varSunType,
                          expr.getLocation());
        scopes().trackClassAllocation(allocaValue, expr.getName(), varSunType);
        return allocaValue;
      }
    }

    AllocaInst* alloca =
        createEntryBlockAlloca(func, expr.getName(), storageTy);
    Value* structVal = value;
    if (value->getType()->isPointerTy()) {
      // Move: load the storage and poison the source tag (the new variable
      // owns the payload now)
      structVal = gen_.applyMoveSemantics(value, varSunType);
    }
    ctx.builder->CreateStore(structVal, alloca);
    scope[expr.getDeclarationId()] = alloca;
    debugDeclareLocal(alloca, expr.getName(), varSunType, expr.getLocation());
    scopes().trackClassAllocation(alloca, expr.getName(), varSunType);
    return alloca;
  }

  // Handle interface types
  if (auto* ifaceType =
          sun::codegen::support::tryGetType<sun::types::InterfaceType>(
              varSunType)) {
    // Unwrap reference if needed
    TypePtr valueSunType =
        sun::types::unwrapRef(expr.getValue()->getResolvedType());

    // A concrete value is moved into stable storage owned by the interface.
    if (auto* classType =
            sun::codegen::support::tryGetType<ClassType>(valueSunType)) {
      Value* fatPtr =
          classes().createOwnedInterfaceFatPointer(value, classType, ifaceType);
      if (!fatPtr) return nullptr;

      AllocaInst* alloca =
          createEntryBlockAlloca(func, expr.getName(), fatPtr->getType());
      ctx.builder->CreateStore(fatPtr, alloca);
      scope[expr.getDeclarationId()] = alloca;
      debugDeclareLocal(alloca, expr.getName(), varSunType, expr.getLocation());
      scopes().trackClassAllocation(alloca, expr.getName(), varSunType);
      return fatPtr;
    }

    // An interface source transfers its existing erased owner.
    if (valueSunType && valueSunType->isInterface()) {
      llvm::StructType* fatPtrType =
          sun::types::InterfaceType::getFatPointerType(ctx.getContext());
      Value* fatPtrVal = classes().upcastInterface(
          value, static_cast<sun::types::InterfaceType*>(valueSunType.get()),
          ifaceType, false);

      AllocaInst* alloca =
          createEntryBlockAlloca(func, expr.getName(), fatPtrType);
      ctx.builder->CreateStore(fatPtrVal, alloca);
      scope[expr.getDeclarationId()] = alloca;
      debugDeclareLocal(alloca, expr.getName(), varSunType, expr.getLocation());
      scopes().trackClassAllocation(alloca, expr.getName(), varSunType);
      return fatPtrVal;
    }
  }

  // Sized arrays own their inline storage. A fresh temporary (a literal, a
  // materialized call result) is adopted as the variable's storage; a named
  // source MOVES its elements into new storage, never aliasing it.
  if (auto* arrayType =
          sun::codegen::support::tryGetType<sun::types::ArrayType>(
              varSunType)) {
    if (!arrayType->isUnsized()) {
      ASTNodeType valueKind = expr.getValue()->getType();
      bool valueIsFreshTemp = valueKind == ASTNodeType::ARRAY_LITERAL ||
                              valueKind == ASTNodeType::CALL ||
                              valueKind == ASTNodeType::GENERIC_CALL;
      if (valueIsFreshTemp) {
        if (auto* allocaValue = dyn_cast<AllocaInst>(value)) {
          allocaValue->setName(expr.getName());
          scope[expr.getDeclarationId()] = allocaValue;
          debugDeclareLocal(allocaValue, expr.getName(), varSunType,
                            expr.getLocation());
          scopes().trackClassAllocation(allocaValue, expr.getName(),
                                        varSunType);
          return allocaValue;
        }
      }
      AllocaInst* alloca =
          createEntryBlockAlloca(func, expr.getName(), varType);
      gen_.emitArrayTransfer(alloca, value, *arrayType, /*move=*/true);
      scope[expr.getDeclarationId()] = alloca;
      debugDeclareLocal(alloca, expr.getName(), varSunType, expr.getLocation());
      scopes().trackClassAllocation(alloca, expr.getName(), varSunType);
      return alloca;
    }
  }

  // A `ref array<T>` local bound to a sized array: the view of its storage
  if (auto* refType =
          sun::codegen::support::tryGetType<ReferenceType>(varSunType)) {
    if (refType->isUnsizedArrayRef() && value->getType()->isPointerTy()) {
      TypePtr valueType =
          sun::types::unwrapRef(expr.getValue()->getResolvedType());
      if (auto* sized =
              sun::codegen::support::tryGetType<sun::types::ArrayType>(
                  valueType)) {
        if (!sized->isUnsized()) {
          value = gen_.emitArrayView(value, sized->getDimensions());
        } else {
          value = gen_.loadArrayView(value);
        }
      }
    }
  }

  // Fresh call results can be adopted; named sources must transfer ownership.
  if (varSunType && varSunType->isClass()) {
    auto valueKind = expr.getValue()->getType();
    auto* allocaValue = dyn_cast<AllocaInst>(value);
    if (allocaValue && (valueKind == ASTNodeType::CALL ||
                        valueKind == ASTNodeType::GENERIC_CALL)) {
      allocaValue->setName(expr.getName());
      scope[expr.getDeclarationId()] = allocaValue;
      debugDeclareLocal(allocaValue, expr.getName(), varSunType,
                        expr.getLocation());

      // Track class allocation for automatic deinit at scope exit
      if (auto classType =
              sun::codegen::support::tryGetTypePtr<ClassType>(varSunType)) {
        scopes().trackClassAllocation(allocaValue, expr.getName(), classType);
      }
      return allocaValue;
    }

    // For class types, if value is a struct (from array indexing),
    // create alloca of the STRUCT type and store the struct value
    if (varSunType && varSunType->isClass() && value->getType()->isStructTy()) {
      llvm::Type* structType = value->getType();
      AllocaInst* alloca =
          createEntryBlockAlloca(func, expr.getName(), structType);
      ctx.builder->CreateStore(value, alloca);
      scope[expr.getDeclarationId()] = alloca;
      debugDeclareLocal(alloca, expr.getName(), varSunType, expr.getLocation());

      // Track class allocation for automatic deinit at scope exit
      if (auto classType =
              sun::codegen::support::tryGetTypePtr<ClassType>(varSunType)) {
        scopes().trackClassAllocation(alloca, expr.getName(), classType);
      }
      return alloca;
    }

    // Transfer a local or field into independently owned storage.
    if (value->getType()->isPointerTy()) {
      if (auto classType =
              sun::codegen::support::tryGetTypePtr<ClassType>(varSunType)) {
        llvm::StructType* structType =
            classType->getStructType(ctx.getContext());
        Value* structVal = gen_.applyMoveSemantics(value, varSunType);
        // Create a new alloca for this variable (the move destination)
        AllocaInst* alloca =
            createEntryBlockAlloca(func, expr.getName(), structType);
        ctx.builder->CreateStore(structVal, alloca);
        scope[expr.getDeclarationId()] = alloca;
        debugDeclareLocal(alloca, expr.getName(), varSunType,
                          expr.getLocation());

        // Track the destination for deinit - it now owns the data
        scopes().trackClassAllocation(alloca, expr.getName(), classType);
        return alloca;
      }
    }
  }

  value = convertToVariableType(
      value, varType,
      expr.getValue() ? expr.getValue()->getResolvedType() : nullptr);

  AllocaInst* alloca = createEntryBlockAlloca(func, expr.getName(), varType);
  ctx.builder->CreateStore(value, alloca);
  scope[expr.getDeclarationId()] = alloca;
  debugDeclareLocal(alloca, expr.getName(), varSunType, expr.getLocation());

  return value;
}

llvm::Value* VariableGenerator::convertToVariableType(
    llvm::Value* value, llvm::Type* varType, const TypePtr& valueSunType) {
  llvm::Type* valueType = value->getType();
  if (valueType == varType) return value;

  if (valueType->isIntegerTy() && varType->isIntegerTy()) {
    unsigned valueBits = valueType->getIntegerBitWidth();
    unsigned varBits = varType->getIntegerBitWidth();
    if (valueBits < varBits)
      return sun::codegen::support::extendInt(*ctx.builder, value, varType,
                                              valueSunType);
    return ctx.builder->CreateTrunc(value, varType, "trunc");
  }
  if (valueType->isFloatTy() && varType->isDoubleTy())
    return ctx.builder->CreateFPExt(value, varType, "widen");
  if (valueType->isDoubleTy() && varType->isFloatTy())
    return ctx.builder->CreateFPTrunc(value, varType, "trunc");
  return value;
}

// -------------------------------------------------------------------
// File-scope variables whose value is known at compile time
// -------------------------------------------------------------------

llvm::Constant* VariableGenerator::buildLlvmConstant(
    const sun::semantic_analysis::constants::ConstantValue& value,
    llvm::Type* type, const sun::support::Position& location) {
  if (value.isInteger() && type->isIntegerTy()) {
    // Analysis stores an integer at the width generated code uses, so this
    // only changes the width if the two ever disagree.
    unsigned width = type->getIntegerBitWidth();
    const APInt& bits = value.getInteger();
    return ConstantInt::get(ctx.getContext(), value.isUnsigned()
                                                  ? bits.zextOrTrunc(width)
                                                  : bits.sextOrTrunc(width));
  }

  if (value.isFloat() && type->isFloatingPointTy()) {
    APFloat number = value.getFloat();
    bool losesInfo = false;
    number.convert(type->getFltSemantics(), APFloat::rmNearestTiesToEven,
                   &losesInfo);
    return ConstantFP::get(ctx.getContext(), number);
  }

  if (value.isString() && type == typeResolver.getStaticPtrType()) {
    // The text lives in its own read-only array, ending in a zero byte so C
    // code can read it; the variable holds its address and its length.
    llvm::Constant* text = llvm::ConstantDataArray::getString(
        ctx.getContext(), value.getString(), /*AddNull=*/true);
    auto* storage = new GlobalVariable(
        *module, text->getType(),
        /*isConstant=*/true, GlobalValue::PrivateLinkage, text, "str");
    storage->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    storage->setAlignment(llvm::Align(1));
    return llvm::ConstantStruct::get(
        typeResolver.getStaticPtrType(),
        {storage, ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()),
                                   value.getString().size())});
  }

  if (auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(type);
      arrayType && value.isArray() &&
      value.getElements().size() == arrayType->getNumElements()) {
    // One constant per element; a further dimension nests the same way
    std::vector<llvm::Constant*> elements;
    elements.reserve(value.getElements().size());
    for (const auto& element : value.getElements())
      elements.push_back(
          buildLlvmConstant(element, arrayType->getElementType(), location));
    return llvm::ConstantArray::get(arrayType, elements);
  }

  logAndThrowError("Internal error: the compile-time value " +
                       value.toDisplayString() +
                       " does not fit the storage of the global variable it "
                       "initializes",
                   location);
}

llvm::GlobalVariable* VariableGenerator::emitImageGlobal(
    const VariableCreationAST& expr, llvm::Type* varType,
    const sun::semantic_analysis::constants::ConstantValue& value) {
  assert(scopes().empty() &&
         "emitImageGlobal should only be called at top-level");
  return createGlobalVariable(
      expr.getDeclarationId(),
      state_.declarationSymbol(expr.getDeclarationId(), "global"), varType,
      buildLlvmConstant(value, varType, expr.getLocation()));
}

// -------------------------------------------------------------------
// Reference creation codegen
// -------------------------------------------------------------------

Value* VariableGenerator::codegen(const sun::ast::ReferenceCreationAST& expr) {
  TypePtr refSunType = expr.getResolvedType();
  sun::codegen::support::requireType<ReferenceType>(expr, "reference creation");

  // A reference stores a pointer to the target's storage. Any addressable
  // lvalue qualifies: variables, fields (obj.f), and array elements (arr[i]).
  // For ref-typed targets codegenAddress flattens to the referent's address,
  // so rebinding aliases the original storage.
  llvm::Value* targetPtr = codegenBorrowAddress(*expr.getTarget());
  if (!targetPtr) {
    logAndThrowError(
        "Reference target is not addressable (expected a variable, field, or "
        "array element)",
        expr.getLocation());
  }

  // Create an alloca that holds a pointer to the target
  std::string refName =
      (scopes().empty()
           ? state_.declarationSymbol(expr.getDeclarationId(), "global")
           : expr.getName());
  llvm::Type* ptrType = llvm::PointerType::getUnqual(ctx.getContext());
  Function* func = ctx.builder->GetInsertBlock()->getParent();
  AllocaInst* refAlloca = createEntryBlockAlloca(func, refName, ptrType);
  ctx.builder->CreateStore(targetPtr, refAlloca);

  if (!scopes().empty()) {
    scopes().back().variables[expr.getDeclarationId()] = refAlloca;
    debugDeclareLocal(refAlloca, refName, refSunType, expr.getLocation());
  }
  return refAlloca;
}

// -------------------------------------------------------------------
// File-scope variables initialized at startup
// -------------------------------------------------------------------

GlobalVariable* VariableGenerator::emitStartupGlobal(
    const VariableCreationAST& expr, llvm::Type* varType) {
  assert(scopes().empty() &&
         "emitStartupGlobal should only be called at top-level");

  // Zeroed until the startup function stores the real value
  GlobalVariable* gv = new GlobalVariable(
      *module, varType,
      /*isConstant=*/false, GlobalValue::ExternalLinkage,
      Constant::getNullValue(varType),
      state_.declarationSymbol(expr.getDeclarationId(), "global"));

  StaticInitInfo info;
  info.globalVar = gv;
  info.varName = expr.getName();
  info.varType = expr.getResolvedType();
  info.classType = sun::codegen::support::tryGetTypePtr<ClassType>(expr);
  info.initExpr = expr.getValue();
  info.location = expr.getLocation();
  staticInits.push_back(std::move(info));

  return bindGlobal(expr.getDeclarationId(), gv);
}

// -------------------------------------------------------------------
// Emit static initialization function
// -------------------------------------------------------------------

/**
 * The initializer of a global class variable constructs it in place when it
 * names the class: `Class(args)` is a call whose callee resolves to the class,
 * and `Class<T>(args)` a generic call that resolves to it. Intrinsics and
 * generic functions merely return a class, so they are not constructions.
 * Returns the constructor arguments, or null when the initializer is anything
 * else.
 */
static const std::vector<std::unique_ptr<ExprAST>>* constructorArgsForGlobal(
    const ExprAST& initExpr) {
  if (initExpr.getType() == ASTNodeType::CALL) {
    const auto& call = static_cast<const sun::ast::CallExprAST&>(initExpr);
    if (sun::codegen::support::tryGetType<ClassType>(*call.getCallee()))
      return &call.getArgs();
    return nullptr;
  }
  if (initExpr.getType() == ASTNodeType::GENERIC_CALL) {
    const auto& call = static_cast<const sun::ast::GenericCallAST&>(initExpr);
    if (call.getGenericFunctionAST()) return nullptr;
    if (sun::codegen::intrinsics::isIntrinsic(call.getFunctionName()))
      return nullptr;
    if (sun::codegen::support::tryGetType<ClassType>(call))
      return &call.getArgs();
  }
  return nullptr;
}

void VariableGenerator::emitStaticInitFunction(uint32_t initOrder,
                                               const std::string& bundleHash) {
  if (staticInits.empty()) return;
  if (initOrder > kMaxStaticInitOrder) {
    logAndThrowError(
        "Too many levels of imported libraries: a library may be "
        "at most " +
        std::to_string(kMaxStaticInitOrder) +
        " imports away from a library that imports nothing");
  }

  // Create the initialization function: void __sun_static_init()
  // Internal linkage, on purpose: a .moon bundle carries its own copy of this
  // function, and linking it into a program that also has one must keep both.
  // With internal linkage the IR linker renames one instead of silently
  // replacing the other; llvm.global_ctors (below) is the shared merge point
  // that runs every copy.
  llvm::FunctionType* initFuncType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(ctx.getContext()), false);
  Function* initFunc = Function::Create(initFuncType, Function::InternalLinkage,
                                        "__sun_static_init", module);

  BasicBlock* entryBB = BasicBlock::Create(ctx.getContext(), "entry", initFunc);
  ctx.builder->SetInsertPoint(entryBB);
  // No subprogram on the init function: it must carry no debug locations
  debugInfo.clearLocation(*ctx.builder);

  // A bundle's code is embedded in every bundle that imports it, so a program
  // can receive several copies of this function. They share one flag, named
  // after the bundle, and only the first copy to run does the work: the
  // globals themselves exist once, and constructing them twice would repeat
  // their side effects and leak the first values.
  if (!bundleHash.empty()) {
    llvm::Type* flagType = llvm::Type::getInt1Ty(ctx.getContext());
    const std::string flagName = "_SUN1_static_init_done_" + bundleHash;
    GlobalVariable* doneFlag = module->getGlobalVariable(flagName);
    if (!doneFlag) {
      doneFlag = new GlobalVariable(
          *module, flagType, /*isConstant=*/false, GlobalValue::WeakAnyLinkage,
          ConstantInt::getFalse(ctx.getContext()), flagName);
    }
    BasicBlock* alreadyBB =
        BasicBlock::Create(ctx.getContext(), "already.initialized", initFunc);
    BasicBlock* runBB = BasicBlock::Create(ctx.getContext(), "run", initFunc);
    Value* done = ctx.builder->CreateLoad(flagType, doneFlag, "done");
    ctx.builder->CreateCondBr(done, alreadyBB, runBB);
    ctx.builder->SetInsertPoint(alreadyBB);
    ctx.builder->CreateRetVoid();
    ctx.builder->SetInsertPoint(runBB);
    ctx.builder->CreateStore(ConstantInt::getTrue(ctx.getContext()), doneFlag);
  }

  // Push a scope for any temporaries needed during init
  scopes().push().isFunctionBoundary = true;

  // Generate initialization code for each global variable
  for (const auto& init : staticInits) {
    GlobalVariable* gv = init.globalVar;

    if (init.classType) {
      // Class type: call constructor. The storage is already zeroed: the
      // global is defined that way and this function's work runs once.
      ClassType* classType = init.classType.get();
      llvm::StructType* structType = classType->getStructType(ctx.getContext());

      // A struct literal names its fields, so store them straight into the
      // global rather than looking for a constructor.
      if (init.initExpr->getType() == ASTNodeType::STRUCT_LITERAL) {
        const auto& literal =
            *static_cast<const sun::ast::StructLiteralAST*>(init.initExpr);
        for (size_t i = 0; i < literal.getFields().size(); ++i) {
          const auto& field = literal.getFields()[i];
          const sun::types::ClassField* classField =
              classType->getField(literal.resolvedFields().at(i));
          if (!classField)
            logAndThrowError(
                "Global initializer field target is not registered");

          Value* value = codegen(*field.value);
          if (!value) {
            logAndThrowError("Failed to generate field '" + field.name +
                             "' for global: " + init.varName);
          }
          value = sun::codegen::support::widenNumericIfNeeded(
              *ctx.builder, typeResolver, value, classField->type,
              field.value->getResolvedType());

          Value* fieldPtr = ctx.builder->CreateStructGEP(
              structType, gv, classField->index, field.name + ".ptr");
          sun::codegen::support::storeIntoSlot(
              *ctx.builder, module->getDataLayout(), fieldPtr, value,
              classField->type, classType);
        }
        continue;
      }

      // `Class(args)` and `Class<T>(args)` construct the global in place;
      // anything else produces a value elsewhere and moves it in.
      const std::vector<std::unique_ptr<ExprAST>>* ctorArgs =
          constructorArgsForGlobal(*init.initExpr);
      if (!ctorArgs) {
        Value* value = codegen(*init.initExpr);
        if (!value) {
          logAndThrowError(
              "Failed to generate initializer for global variable: " +
              init.varName);
        }
        // Moving, not copying: the source is invalidated so only the global
        // drops the value.
        ctx.builder->CreateStore(gen_.applyMoveSemantics(value, init.varType),
                                 gv);
        continue;
      }

      const auto* ctor =
          classType->getMethod(init.initExpr->getTargetDeclarationId());
      Function* ctorFunc =
          ctor ? functions().lookupFunctionById(ctor->declarationId) : nullptr;

      // A class with no constructor at all is fully described by the zeroed
      // storage; anything else must reach its constructor, so a lookup that
      // comes up empty is a miscompile rather than a silent no-op.
      if (!ctorFunc || ctorFunc->arg_size() != ctorArgs->size() + 1) {
        if (!ctorArgs->empty() || ctorFunc) {
          logAndThrowError("No constructor to initialize global variable '" +
                               init.varName + "' of type " +
                               classType->getDisplayName(),
                           init.location);
        }
        continue;
      }

      const auto& paramTypes = ctor->paramTypes;

      std::vector<Value*> ctorArgValues;
      // Method closure; the receiver is the global variable
      ctorArgValues.push_back(
          gen_.materializeMethodClosure(ctorFunc, gv, "init.closure"));

      size_t argIdx = 0;
      for (const auto& arg : *ctorArgs) {
        TypePtr paramType =
            argIdx < paramTypes.size() ? paramTypes[argIdx] : nullptr;
        bool isRefParam = paramType && paramType->isReference();

        Value* argVal = codegen(*arg);
        if (!argVal) {
          logAndThrowError(
              "Failed to generate argument for global class constructor: " +
              init.varName);
        }

        if (isRefParam) {
          // Reference parameters take the argument's address; a sized array
          // handed to a `ref array<T>` parameter is viewed with its rank
          // erased
          auto* paramRef = static_cast<const ReferenceType*>(paramType.get());
          auto* sizedArg =
              sun::codegen::support::tryGetType<sun::types::ArrayType>(
                  sun::types::unwrapRef(arg->getResolvedType()));
          if (paramRef->isUnsizedArrayRef() && sizedArg &&
              !sizedArg->isUnsized()) {
            argVal = gen_.emitArrayView(argVal, sizedArg->getDimensions());
          } else if (paramRef->isUnsizedArrayRef()) {
            argVal = gen_.loadArrayView(argVal);
          } else if (!argVal->getType()->isPointerTy()) {
            AllocaInst* tempAlloca =
                createEntryBlockAlloca(initFunc, "ref.temp", argVal->getType());
            ctx.builder->CreateStore(argVal, tempAlloca);
            argVal = tempAlloca;
          }
        } else {
          // By-value compound arguments move into the constructor
          argVal = gen_.applyMoveSemantics(argVal, arg->getResolvedType());
          argVal = sun::codegen::support::widenNumericIfNeeded(
              *ctx.builder, typeResolver, argVal, paramType,
              arg->getResolvedType());
        }

        ctorArgValues.push_back(argVal);
        ++argIdx;
      }

      ctx.builder->CreateCall(ctorFunc, ctorArgValues);
    } else {
      // Non-class type: evaluate expression and store
      Value* initVal = codegen(*init.initExpr);
      if (!initVal) {
        logAndThrowError(
            "Failed to generate initializer for global variable: " +
            init.varName);
      }

      // A sized array owns its elements inline: they move into the global
      auto* arrayType =
          sun::codegen::support::tryGetType<sun::types::ArrayType>(
              init.varType);
      if (arrayType && !arrayType->isUnsized()) {
        gen_.emitArrayTransfer(gv, initVal, *arrayType, /*move=*/true);
        continue;
      }

      initVal = convertToVariableType(initVal, gv->getValueType(),
                                      init.initExpr->getResolvedType());
      if (initVal->getType() != gv->getValueType()) {
        logAndThrowError("Global variables of type '" +
                             init.varType->toString() +
                             "' cannot be initialized with this expression yet",
                         init.location);
      }
      ctx.builder->CreateStore(initVal, gv);
    }
  }

  scopes().pop();
  ctx.builder->CreateRetVoid();

  // Register the init function in llvm.global_ctors. Lower priorities run
  // first, so a library's globals are ready before the globals of whatever
  // imports it.
  llvm::appendToGlobalCtors(*module, initFunc,
                            kStaticInitBasePriority + initOrder);

  // Clear the queue
  staticInits.clear();
}

}  // namespace sun::codegen::variables
