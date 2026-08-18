// lvalues.cpp - Shared lvalue (address) computation for assignable
// expressions. codegenAddress is the single owner of "give me a pointer to
// this place"; assignment codegens, compound assignment, reference creation,
// and ref arguments all build on it.

#include "ast.h"
#include "codegen.h"
#include "codegen_visitor.h"
#include "packed_layout.h"

using namespace llvm;

// -------------------------------------------------------------------
// Field pointer helper
// -------------------------------------------------------------------

Value* CodegenVisitor::getFieldPtr(sun::ClassType* classType, Value* objectPtr,
                                   const sun::ClassField& field,
                                   const std::string& name) {
  llvm::StructType* structType = classType->getStructType(ctx.getContext());
  return ctx.builder->CreateStructGEP(structType, objectPtr, field.index, name);
}

// -------------------------------------------------------------------
// Field access alignment
// -------------------------------------------------------------------

llvm::Align CodegenVisitor::fieldAlign(const sun::ClassType* owner,
                                       llvm::Type* fieldTy) {
  return sun::packed::fieldAlign(owner, fieldTy, module->getDataLayout());
}

// Write a value into a storage slot.
//
// codegen of a class-valued expression yields the object's ADDRESS, not the
// struct itself. Storing that address would write a pointer over the object's
// leading bytes — and it fits silently, because a two-word class is exactly
// pointer-sized, so the mistake surfaces as corrupted fields rather than as a
// verifier error. Every site that writes a class into storage goes through
// here so the copy cannot be forgotten again.
//
// `owner` is the class the slot belongs to when the slot is one of its
// fields; a packed owner drops the alignment to 1. Pass nullptr for a
// standalone slot such as a local variable.
void CodegenVisitor::storeIntoSlot(llvm::Value* dest, llvm::Value* value,
                                   const sun::TypePtr& slotType,
                                   const sun::ClassType* owner) {
  if (slotType && slotType->isClass() && value->getType()->isPointerTy()) {
    const auto* classType = static_cast<const sun::ClassType*>(slotType.get());
    llvm::StructType* structTy = classType->getStructType(ctx.getContext());
    llvm::Align align = fieldAlign(owner, structTy);
    ctx.builder->CreateMemCpy(
        dest, align, value, align,
        module->getDataLayout().getTypeAllocSize(structTy));
    return;
  }
  ctx.builder->CreateAlignedStore(value, dest,
                                  fieldAlign(owner, value->getType()));
}

llvm::Align CodegenVisitor::lvalueAlign(const ExprAST& target,
                                        llvm::Type* slotTy) {
  return sun::packed::lvalueAlign(target, slotTy, module->getDataLayout());
}

// -------------------------------------------------------------------
// Object pointer resolution for member access/assignment
// -------------------------------------------------------------------

// Codegen a member-access object expression down to (objectPtr, ClassType*).
// Applies the generic-`this` fixup and unwraps raw_ptr/static_ptr/ref to
// class. Returns {nullptr, nullptr} when the object is not class-shaped
// (caller decides whether that is an error).
std::pair<Value*, sun::ClassType*> CodegenVisitor::codegenObjectPtr(
    const ExprAST& object) {
  Value* objectPtr = codegen(object);
  if (!objectPtr) return {nullptr, nullptr};

  sun::TypePtr objectType = object.getResolvedType();

  // For generic method bodies, 'this' may have a type parameter type; use the
  // specialized currentClass instead
  if (dynamic_cast<const ThisExprAST*>(&object) && currentClass) {
    objectType = currentClass;
  }

  // Pointer-to-class: the pointer value is already the object pointer
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

  // Reference-to-class: the reference value is already the object pointer
  if (objectType && objectType->isReference()) {
    sun::TypePtr referencedType =
        static_cast<sun::ReferenceType*>(objectType.get())->getReferencedType();
    if (referencedType && referencedType->isClass()) {
      objectType = referencedType;
    }
  }

  if (!objectType || !objectType->isClass()) return {objectPtr, nullptr};

  return {objectPtr, static_cast<sun::ClassType*>(objectType.get())};
}

// -------------------------------------------------------------------
// Lvalue address computation
// -------------------------------------------------------------------

// Compute the address of an assignable expression, or nullptr when the
// expression has no addressable slot (class __index__ targets, slices,
// closure captures, temporaries, modules). Never spills values to temp
// allocas - every returned pointer is the genuine storage location.
Value* CodegenVisitor::tryCodegenAddress(const ExprAST& expr) {
  // Expressions that are already the referent's address: a call or index that
  // borrows, and the wrappers a body puts around one. codegenExpression skips
  // the read-through that codegen() would otherwise apply.
  sun::TypePtr exprType = expr.getResolvedType();
  if (exprType && exprType->isReference()) {
    switch (expr.getType()) {
      case ASTNodeType::CALL:
      case ASTNodeType::INDEX:
      case ASTNodeType::GENERIC_CALL:  // _to_ref<T>(ptr)
        return codegenExpression(expr);
      case ASTNodeType::PAREN_EXPR:
        return tryCodegenAddress(
            *static_cast<const ParenExprAST&>(expr).getInner());
      case ASTNodeType::UNSAFE_BLOCK: {
        const auto& body =
            static_cast<const UnsafeBlockAST&>(expr).getBody().getBody();
        if (!body.empty()) return tryCodegenAddress(*body.back());
        break;
      }
      default:
        break;
    }
  }

  switch (expr.getType()) {
    case ASTNodeType::VARIABLE_REFERENCE: {
      const auto& varRef = static_cast<const VariableReferenceAST&>(expr);
      sun::TypePtr varType = varRef.getResolvedType();

      // Module references read as a null-pointer sentinel - never hand that
      // out as a storage address
      if (varType && varType->isModule()) return nullptr;

      AllocaInst* alloca = findVariable(varRef.getName());
      if (alloca) {
        if (varType && varType->isReference()) {
          // Ref variable: the referent's address. Mirrors
          // createLoadForRef/createStoreForRef's isDirectAlias decision.
          const auto* refType =
              static_cast<const sun::ReferenceType*>(varType.get());
          llvm::Type* referencedLLVMType =
              typeResolver.resolve(refType->getReferencedType());
          if (alloca->getAllocatedType() == referencedLLVMType) {
            return alloca;
          }
          return ctx.builder->CreateLoad(
              llvm::PointerType::getUnqual(ctx.getContext()), alloca,
              varRef.getName() + ".ptr");
        }
        // Compound match-payload bindings hold the borrowed slot's address
        return compoundStorageAddress(varRef.getName());
      }

      // [ref x] captures have a genuine storage address (the pointer stored
      // in the env slot). By-value captures stay non-addressable - handing
      // out the copy's address would let callers mutate it.
      {
        bool byRef = false;
        Value* slotAddr =
            createCaptureSlotAddress(varRef.getName(), nullptr, &byRef);
        if (slotAddr && byRef) return slotAddr;
      }

      // Globals: mangled name first (module-qualified), then plain
      if (GlobalVariable* gv =
              module->getGlobalVariable(varRef.getMangledName())) {
        return gv;
      }
      if (GlobalVariable* gv = module->getGlobalVariable(varRef.getName())) {
        return gv;
      }
      return nullptr;
    }

    case ASTNodeType::MEMBER_ACCESS: {
      const auto& memberAccess = static_cast<const MemberAccessAST&>(expr);
      auto [objectPtr, classType] =
          codegenObjectPtr(*memberAccess.getObject());
      if (!objectPtr || !classType) return nullptr;

      const sun::ClassField* field =
          classType->getField(memberAccess.getMemberName());
      if (!field) return nullptr;

      return getFieldPtr(classType, objectPtr, *field,
                         memberAccess.getMemberName() + ".addr");
    }

    case ASTNodeType::INDEX: {
      const auto& indexExpr = static_cast<const IndexAST&>(expr);
      auto baseType = sun::unwrapRef(indexExpr.getTarget()->getResolvedType());
      // Class __index__/__setindex__ targets have no address - callers
      // dispatch to the method protocol instead
      if (baseType && baseType->isClass()) return nullptr;
      if (indexExpr.hasSlices()) return nullptr;
      return codegenIndexElementPtr(indexExpr);
    }

    case ASTNodeType::THIS: {
      AllocaInst* thisAlloca = findVariable("this");
      if (!thisAlloca) return nullptr;
      return ctx.builder->CreateLoad(
          llvm::PointerType::getUnqual(ctx.getContext()), thisAlloca, "this");
    }

    default:
      return nullptr;
  }
}

Value* CodegenVisitor::codegenAddress(const ExprAST& expr) {
  Value* addr = tryCodegenAddress(expr);
  if (!addr) {
    logAndThrowError(
        "Expression is not addressable (expected a variable, field, or array "
        "element)",
        expr.getLocation());
  }
  return addr;
}

// -------------------------------------------------------------------
// Compound assignment lowering: target op= value
// -------------------------------------------------------------------

// Apply `cur op rhs` for a compound assignment and coerce the result back to
// the slot's type if operand unification widened it
Value* CodegenVisitor::emitCompoundOpValue(const CompoundAssignmentAST& expr,
                                           Value* cur, llvm::Type* slotTy,
                                           const sun::TypePtr& slotSunType) {
  Value* rhs = codegen(*expr.getValue());
  if (!rhs) return nullptr;

  const Position& loc = expr.getLocation();
  bool unsignedOp = slotSunType && slotSunType->isUnsigned();

  unifyBinaryOperands(cur, rhs, slotSunType,
                      expr.getValue()->getResolvedType(), loc);
  Value* result = emitBinaryOp(expr.binaryOpKind(), cur, rhs, unsignedOp, loc);
  if (!result) return nullptr;

  if (result->getType() != slotTy) {
    if (result->getType()->isIntegerTy() && slotTy->isIntegerTy()) {
      result = result->getType()->getIntegerBitWidth() >
                       slotTy->getIntegerBitWidth()
                   ? ctx.builder->CreateTrunc(result, slotTy, "compound.trunc")
                   : extendInt(result, slotTy, slotSunType);
    } else if (result->getType()->isDoubleTy() && slotTy->isFloatTy()) {
      result = ctx.builder->CreateFPTrunc(result, slotTy, "compound.trunc");
    } else if (result->getType()->isFloatTy() && slotTy->isDoubleTy()) {
      result = ctx.builder->CreateFPExt(result, slotTy, "compound.widen");
    } else {
      logAndThrowError("Compound assignment result type mismatch", loc);
    }
  }
  return result;
}

Value* CodegenVisitor::codegen(const CompoundAssignmentAST& expr) {
  const ExprAST& target = *expr.getTarget();
  const Position& loc = expr.getLocation();

  sun::TypePtr slotSunType = sun::unwrapRef(target.getResolvedType());
  llvm::Type* slotTy = typeResolver.resolve(slotSunType);

  // Class __index__/__setindex__ targets have no address: lower as
  // get-op-set with the receiver and index array computed exactly once
  if (target.getType() == ASTNodeType::INDEX) {
    const auto& indexExpr = static_cast<const IndexAST&>(target);
    auto baseType = sun::unwrapRef(indexExpr.getTarget()->getResolvedType());
    if (baseType && baseType->isClass()) {
      auto* classType = static_cast<sun::ClassType*>(baseType.get());
      Value* objPtr = codegen(*indexExpr.getTarget());
      if (!objPtr) return nullptr;
      llvm::AllocaInst* idxArr = boxIndicesToArrayRef(indexExpr);
      if (!idxArr) return nullptr;

      Value* cur = emitClassIndexCall(objPtr, idxArr, classType);
      if (!cur) return nullptr;
      Value* result = emitCompoundOpValue(expr, cur, slotTy, slotSunType);
      if (!result) return nullptr;
      return emitClassSetIndexCall(objPtr, idxArr, result, classType);
    }
  }

  // Addressable targets: address once -> load -> op -> store
  if (Value* addr = tryCodegenAddress(target)) {
    llvm::Align align = lvalueAlign(target, slotTy);
    Value* cur =
        ctx.builder->CreateAlignedLoad(slotTy, addr, align, "compound.cur");
    Value* result = emitCompoundOpValue(expr, cur, slotTy, slotSunType);
    if (!result) return nullptr;
    ctx.builder->CreateAlignedStore(result, addr, align);
    return result;
  }

  // Note: [ref x] captures are addressable via tryCodegenAddress above;
  // by-value captures are rejected by semantic analysis before codegen
  logAndThrowError("Compound assignment target is not assignable", loc);
  return nullptr;
}
