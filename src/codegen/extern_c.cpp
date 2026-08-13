// extern_c.cpp — The `extern "C"` boundary. See extern_c.h.

#include "extern_c.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>

#include "ast.h"

namespace sun::cabi {

llvm::Function* ExternCEmitter::declare(
    const PrototypeAST& proto, llvm::Type* returnType,
    llvm::ArrayRef<llvm::Type*> paramTypes) {
  // Record the rename before anything else, so call sites resolve even when
  // the declaration itself was already emitted and we return early below.
  if (proto.hasLinkName()) {
    symbolNames_[proto.getName()] = proto.getLinkName();
  }

  const std::string& symbol = proto.getLinkName();
  if (llvm::Function* existing = module_->getFunction(symbol)) {
    return existing;
  }

  // Apply the C ABI to the signature. Scalars and pointers come back
  // unchanged; aggregates are coerced into register-sized pieces or passed
  // through memory. LLVM does none of this on its own.
  auto lowering =
      sysv::lowerCSignature(returnType, paramTypes, module_->getDataLayout());
  llvm::FunctionType* funcType = sysv::buildLoweredFunctionType(
      lowering, ctx_.getContext(), proto.isCVariadic());

  llvm::Function* func = llvm::Function::Create(
      funcType, llvm::Function::ExternalLinkage, symbol, module_);

  // Key the plan by the emitted symbol so any path that finds the Function
  // can find the marshalling it was built with.
  lowerings_[func->getName().str()] = lowering;
  applyAttributes(func, lowering);

  // Parameter names are only meaningful where a parameter survived one-to-one;
  // coerced and indirect ones no longer correspond to a single source name.
  if (lowering.isTrivial()) {
    unsigned idx = 0;
    for (auto& arg : func->args()) {
      if (idx < proto.getArgs().size()) arg.setName(proto.getArgs()[idx].first);
      ++idx;
    }
  }

  return func;
}

const std::string& ExternCEmitter::symbolFor(const std::string& sunName) const {
  auto it = symbolNames_.find(sunName);
  return it == symbolNames_.end() ? sunName : it->second;
}

const sysv::SignatureLowering* ExternCEmitter::loweringFor(
    const llvm::Function* func) const {
  if (!func) return nullptr;
  auto it = lowerings_.find(func->getName().str());
  return it == lowerings_.end() ? nullptr : &it->second;
}

bool ExternCEmitter::needsMarshalling(const llvm::Function* func) const {
  const sysv::SignatureLowering* lowering = loweringFor(func);
  return lowering && !lowering->isTrivial();
}

void ExternCEmitter::applyAttributes(
    llvm::Function* func, const sysv::SignatureLowering& lowering) const {
  llvm::LLVMContext& llvmCtx = ctx_.getContext();
  unsigned idx = 0;

  auto addAlign = [&](unsigned at, uint64_t align) {
    func->addParamAttr(at, llvm::Attribute::getWithAlignment(
                               llvmCtx, llvm::Align(align ? align : 1)));
  };

  if (lowering.usesSret()) {
    func->addParamAttr(idx, llvm::Attribute::getWithStructRetType(
                                llvmCtx, lowering.ret.type));
    addAlign(idx, lowering.ret.align);
    ++idx;
  }

  for (const auto& param : lowering.params) {
    if (param.isIndirect()) {
      func->addParamAttr(
          idx, llvm::Attribute::getWithByValType(llvmCtx, param.type));
      addAlign(idx, param.align);
      ++idx;
    } else if (param.isCoerced()) {
      idx += param.pieces.size();
    } else {
      ++idx;
    }
  }
}

llvm::AllocaInst* ExternCEmitter::entryAlloca(llvm::Type* type,
                                              const char* name) const {
  llvm::Function* parent = ctx_.builder->GetInsertBlock()->getParent();
  llvm::IRBuilder<> entry(&parent->getEntryBlock(),
                          parent->getEntryBlock().begin());
  return entry.CreateAlloca(type, nullptr, name);
}

llvm::Value* ExternCEmitter::addressOf(llvm::Value* value,
                                       llvm::Type* aggregateType) const {
  if (value->getType()->isPointerTy()) return value;
  llvm::AllocaInst* slot = entryAlloca(aggregateType, "cabi.arg");
  ctx_.builder->CreateStore(value, slot);
  return slot;
}

llvm::Value* ExternCEmitter::promoteVararg(llvm::Value* value,
                                           const sun::TypePtr& sunType) const {
  if (!value) return value;
  llvm::LLVMContext& llvmCtx = ctx_.getContext();

  // static_ptr<T> is a fat { ptr, i64 }; C expects the bare data pointer.
  // This is what makes printf("%s", "literal") work.
  if (sunType && sunType->isStaticPointer() && value->getType()->isStructTy()) {
    return ctx_.builder->CreateExtractValue(value, 0, "vararg.str.data");
  }

  if (value->getType()->isFloatTy()) {
    return ctx_.builder->CreateFPExt(value, llvm::Type::getDoubleTy(llvmCtx),
                                     "vararg.fpext");
  }

  if (value->getType()->isIntegerTy() &&
      value->getType()->getIntegerBitWidth() < 32) {
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
    bool isUnsigned =
        sunType && sunType->isIntegral() && !sunType->isSigned();
    return isUnsigned ? ctx_.builder->CreateZExt(value, i32Ty, "vararg.zext")
                      : ctx_.builder->CreateSExt(value, i32Ty, "vararg.sext");
  }

  return value;
}

llvm::Value* ExternCEmitter::emitCall(llvm::Function* func,
                                      llvm::ArrayRef<PreparedArg> args,
                                      CallEmitter emitCallInsn) {
  const sysv::SignatureLowering* lowering = loweringFor(func);
  if (!lowering) return nullptr;

  llvm::LLVMContext& llvmCtx = ctx_.getContext();
  llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);
  const llvm::DataLayout& dl = module_->getDataLayout();

  std::vector<llvm::Value*> loweredArgs;

  // sret: allocate the destination up front and pass its address first.
  llvm::AllocaInst* sretSlot = nullptr;
  if (lowering->usesSret()) {
    sretSlot = entryAlloca(lowering->ret.type, "cabi.ret");
    loweredArgs.push_back(sretSlot);
  }

  for (size_t i = 0; i < args.size(); ++i) {
    const PreparedArg& arg = args[i];
    const sysv::ArgLowering* plan =
        i < lowering->params.size() ? &lowering->params[i] : nullptr;

    // Past the declared parameters: a `...` tail, where C's promotions apply.
    if (!plan) {
      loweredArgs.push_back(func->getFunctionType()->isVarArg()
                                ? promoteVararg(arg.value, arg.sunType)
                                : arg.value);
      continue;
    }

    if (plan->isDirect()) {
      loweredArgs.push_back(arg.value);
      continue;
    }

    // Aggregate: work from its address so the bytes can be reinterpreted.
    llvm::Value* addr = addressOf(arg.value, plan->type);

    if (plan->isIndirect()) {
      // byval hands the callee a pointer, but the callee owns a private copy
      // — so give it a fresh one rather than the caller's live object.
      llvm::AllocaInst* copy = entryAlloca(plan->type, "cabi.byval");
      ctx_.builder->CreateMemCpy(copy, llvm::MaybeAlign(plan->align), addr,
                                 llvm::MaybeAlign(plan->align),
                                 dl.getTypeAllocSize(plan->type));
      loweredArgs.push_back(copy);
      continue;
    }

    // Coerced: reload the aggregate's bytes as the eightbyte pieces the ABI
    // wants, one LLVM argument each.
    for (size_t piece = 0; piece < plan->pieces.size(); ++piece) {
      llvm::Value* slot = ctx_.builder->CreateConstInBoundsGEP1_64(
          i8Ty, addr, piece * 8, "cabi.piece.addr");
      loweredArgs.push_back(
          ctx_.builder->CreateLoad(plan->pieces[piece], slot, "cabi.piece"));
    }
  }

  llvm::Value* result =
      emitCallInsn(func->getFunctionType(), func, loweredArgs);

  // sret already wrote into our buffer; its address is the Sun-level result.
  if (lowering->usesSret()) return sretSlot;

  // A coerced return arrives as register pieces. Write them back over a
  // buffer of the real struct type and hand back its address, which is how
  // Sun represents a struct value.
  if (lowering->ret.isCoerced() && !lowering->ret.pieces.empty()) {
    llvm::AllocaInst* slot = entryAlloca(lowering->ret.type, "cabi.ret");
    if (lowering->ret.pieces.size() == 1) {
      ctx_.builder->CreateStore(result, slot);
    } else {
      for (unsigned piece = 0; piece < lowering->ret.pieces.size(); ++piece) {
        llvm::Value* part =
            ctx_.builder->CreateExtractValue(result, piece, "cabi.ret.part");
        llvm::Value* dest = ctx_.builder->CreateConstInBoundsGEP1_64(
            i8Ty, slot, piece * 8, "cabi.ret.addr");
        ctx_.builder->CreateStore(part, dest);
      }
    }
    return slot;
  }

  return result;
}

}  // namespace sun::cabi
