// lvalues.cpp - Shared lvalue (address) computation for assignable
// expressions. codegenAddress is the single owner of "give me a pointer to
// this place"; assignment codegens, compound assignment, reference creation,
// and ref arguments all build on it.

#include "ast.h"
#include "codegen/codegen.h"
#include "codegen/codegen_visitor.h"
#include "codegen/variable_generator.h"
#include "codegen/scalar_ops.h"
#include "codegen/struct_access.h"
#include "semantic_analysis/packed_layout.h"

using namespace llvm;

namespace layout = sun::codegen::layout;
namespace ops = sun::codegen::ops;

// -------------------------------------------------------------------
// Field pointer helper
// -------------------------------------------------------------------
// Object pointer resolution for member access/assignment
// -------------------------------------------------------------------

// Codegen a member-access object expression down to (objectPtr, ClassType*).
// Applies the generic-`this` fixup and unwraps raw_ptr/static_ptr/ref to
// class. Returns {nullptr, nullptr} when the object is not class-shaped
// (caller decides whether that is an error).
std::pair<Value*, sun::ClassType*> VariableGenerator::codegenObjectPtr(
    const ExprAST& object) {
  Value* objectPtr = codegen(object);
  if (!objectPtr) return {nullptr, nullptr};

  sun::TypePtr objectType = object.getResolvedType();

  // For generic method bodies, 'this' may have a type parameter type; use the
  // specialized state_.frame.currentClass instead
  if (dynamic_cast<const ThisExprAST*>(&object) && state_.frame.currentClass) {
    objectType = state_.frame.currentClass;
  }

  // Pointer-to-class: the pointer value is already the object pointer
  sun::TypePtr pointeeType = sun::getPointeeType(objectType);
  if (pointeeType && pointeeType->isClass()) {
    objectType = pointeeType;
  }

  // Reference-to-class: the reference value is already the object pointer
  if (auto* refType = sun::tryGetType<sun::ReferenceType>(objectType)) {
    if (refType->getReferencedType() &&
        refType->getReferencedType()->isClass()) {
      objectType = refType->getReferencedType();
    }
  }

  return {objectPtr, sun::tryGetType<sun::ClassType>(objectType)};
}

// -------------------------------------------------------------------
// Lvalue address computation
// -------------------------------------------------------------------

// Compute the address of an assignable expression, or nullptr when the
// expression has no addressable slot (class __index__ targets, slices,
// closure captures, temporaries, modules). Never spills values to temp
// allocas - every returned pointer is the genuine storage location.
Value* VariableGenerator::tryCodegenAddress(const ExprAST& expr) {
  // Expressions that are already the referent's address: a call or index that
  // borrows, and the wrappers a body puts around one. codegenExpression skips
  // the read-through that codegen() would otherwise apply.
  sun::TypePtr exprType = expr.getResolvedType();
  if (exprType && exprType->isReference()) {
    switch (expr.getType()) {
      case ASTNodeType::CALL:
      case ASTNodeType::INDEX:
      case ASTNodeType::GENERIC_CALL:  // _to_ref<T>(ptr)
        return gen_.codegenExpression(expr);
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

      AllocaInst* alloca = scopes().findVariable(varRef.getName());
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
        return scopes().compoundStorageAddress(varRef.getName());
      }

      // [ref x] captures have a genuine storage address (the pointer stored
      // in the env slot), and an owned capture's slot IS the value's storage.
      // An implicit by-value capture stays non-addressable - handing out the
      // copy's address would let callers mutate it.
      {
        bool byRef = false;
        bool owned = false;
        Value* slotAddr =
            functionGen().createCaptureSlotAddress(varRef.getName(), nullptr, &byRef, &owned);
        if (slotAddr && (byRef || owned)) return slotAddr;
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

      // mod.global: the module is compile-time only, so the storage is the
      // global the member's declaration emitted
      if (llvm::GlobalVariable* gv =
              classes().moduleMemberGlobal(*memberAccess.getObject(),
                                 memberAccess.getQualifiedName().mangled())) {
        return gv;
      }

      auto [objectPtr, classType] = codegenObjectPtr(*memberAccess.getObject());
      if (!objectPtr || !classType) return nullptr;

      const sun::ClassField* field =
          classType->getField(memberAccess.getMemberName());
      if (!field) return nullptr;

      return layout::fieldPtr(*ctx.builder, classType, objectPtr, *field,
                         memberAccess.getMemberName() + ".addr");
    }

    case ASTNodeType::INDEX: {
      const auto& indexExpr = static_cast<const IndexAST&>(expr);
      auto baseType = sun::unwrapRef(indexExpr.getTarget()->getResolvedType());
      // Class __index__/__setindex__ targets have no address - callers
      // dispatch to the method protocol instead
      if (baseType && baseType->isClass()) return nullptr;
      if (indexExpr.hasSlices()) return nullptr;
      return gen_.codegenIndexElementPtr(indexExpr);
    }

    case ASTNodeType::THIS: {
      AllocaInst* thisAlloca = scopes().findVariable("this");
      if (!thisAlloca) return nullptr;
      return ctx.builder->CreateLoad(
          llvm::PointerType::getUnqual(ctx.getContext()), thisAlloca, "this");
    }

    default:
      return nullptr;
  }
}

// A borrow may bind a conditional lvalue: `ref r = c ? a.x : b.y` picks one of
// two storage slots at runtime, so the address is a phi of the branches'
// addresses. Only the borrow-binding paths ask for this; every other lvalue
// use goes through tryCodegenAddress, which leaves a conditional alone.
Value* VariableGenerator::codegenBorrowAddress(const ExprAST& expr) {
  if (expr.getType() != ASTNodeType::TERNARY) return tryCodegenAddress(expr);

  const auto& ternary = static_cast<const TernaryExprAST&>(expr);
  Value* cond = codegen(*ternary.getCond());
  if (!cond) return nullptr;
  cond = coerceCondToBool(ctx, cond);

  Function* func = ctx.builder->GetInsertBlock()->getParent();
  BasicBlock* thenBB =
      BasicBlock::Create(ctx.getContext(), "borrow.then", func);
  BasicBlock* elseBB =
      BasicBlock::Create(ctx.getContext(), "borrow.else", func);
  BasicBlock* mergeBB =
      BasicBlock::Create(ctx.getContext(), "borrow.cont", func);
  ctx.builder->CreateCondBr(cond, thenBB, elseBB);

  // Blocks are already emitted, so a branch without an address is a hard
  // error rather than a nullptr the caller could recover from. Semantic
  // analysis rejects those shapes before codegen sees them.
  auto branchAddress = [&](const ExprAST& branch) {
    Value* addr = codegenBorrowAddress(branch);
    if (!addr) {
      logAndThrowError(
          "Conditional reference branch is not addressable (expected a "
          "variable, field, or array element)",
          branch.getLocation());
    }
    ctx.builder->CreateBr(mergeBB);
    return addr;
  };

  ctx.builder->SetInsertPoint(thenBB);
  Value* thenAddr = branchAddress(*ternary.getThen());
  thenBB = ctx.builder->GetInsertBlock();

  ctx.builder->SetInsertPoint(elseBB);
  Value* elseAddr = branchAddress(*ternary.getElse());
  elseBB = ctx.builder->GetInsertBlock();

  ctx.builder->SetInsertPoint(mergeBB);
  PHINode* phi = ctx.builder->CreatePHI(
      PointerType::getUnqual(ctx.getContext()), 2, "borrow.addr");
  phi->addIncoming(thenAddr, thenBB);
  phi->addIncoming(elseAddr, elseBB);
  return phi;
}

Value* VariableGenerator::codegenAddress(const ExprAST& expr) {
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
Value* VariableGenerator::emitCompoundOpValue(const CompoundAssignmentAST& expr,
                                           Value* cur, llvm::Type* slotTy,
                                           const sun::TypePtr& slotSunType) {
  Value* rhs = codegen(*expr.getValue());
  if (!rhs) return nullptr;

  const Position& loc = expr.getLocation();
  bool unsignedOp = slotSunType && slotSunType->isUnsigned();

  gen_.unifyBinaryOperands(cur, rhs, slotSunType, expr.getValue()->getResolvedType(),
                      loc);
  Value* result = gen_.emitBinaryOp(expr.binaryOpKind(), cur, rhs, unsignedOp, loc);
  if (!result) return nullptr;

  if (result->getType() != slotTy) {
    if (result->getType()->isIntegerTy() && slotTy->isIntegerTy()) {
      result =
          result->getType()->getIntegerBitWidth() > slotTy->getIntegerBitWidth()
              ? ctx.builder->CreateTrunc(result, slotTy, "compound.trunc")
              : ops::extendInt(*ctx.builder, result, slotTy, slotSunType);
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

Value* VariableGenerator::codegen(const CompoundAssignmentAST& expr) {
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
      llvm::AllocaInst* idxArr = gen_.boxIndicesToArrayRef(indexExpr);
      if (!idxArr) return nullptr;

      Value* cur = gen_.emitClassIndexCall(objPtr, idxArr, classType);
      if (!cur) return nullptr;
      Value* result = emitCompoundOpValue(expr, cur, slotTy, slotSunType);
      if (!result) return nullptr;
      return gen_.emitClassSetIndexCall(objPtr, idxArr, result, classType);
    }
  }

  // Addressable targets: address once -> load -> op -> store
  if (Value* addr = tryCodegenAddress(target)) {
    llvm::Align align =
        layout::lvalueAlign(target, slotTy, module->getDataLayout());
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
