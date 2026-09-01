// extern_c.cpp — The `extern "C"` boundary. See extern_c.h.

#include "codegen/abi/extern_c.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/TargetParser/Host.h>

#include "ast.h"
#include "support/error.h"

namespace sun::cabi {

namespace {

llvm::Triple targetTriple(const llvm::Module* module) {
  std::string triple = module->getTargetTriple();
  if (triple.empty()) triple = llvm::sys::getDefaultTargetTriple();
  return llvm::Triple(triple);
}

// Which of the signature's integers are signed, read off the Sun-level types
// the analyzer resolved. Darwin arm64 needs this to pick between signext and
// zeroext; targets that never extend ignore it.
abi::SignednessInfo signednessOf(const PrototypeAST& proto) {
  abi::SignednessInfo signs;
  if (const sun::TypePtr& ret = proto.getResolvedReturnType()) {
    signs.retSigned = ret->isIntegral() && ret->isSigned();
  }
  for (const sun::TypePtr& param : proto.getResolvedParamTypes()) {
    signs.paramSigned.push_back(param && param->isIntegral() &&
                                param->isSigned());
  }
  return signs;
}

// Attach the attributes a lowered signature requires — sret, byval and their
// alignments, HFA alignstack, and integer extension. Applied to both the
// declaration and every call site: LLVM lowers a call from the call site's
// attributes, so a declaration-only sret or signext would silently vanish.
// `Target` is llvm::Function or llvm::CallBase; both spell the setters the
// same way.
template <typename Target>
void attachLoweringAttributes(Target* target,
                              const abi::SignatureLowering& lowering,
                              llvm::LLVMContext& llvmCtx) {
  unsigned idx = 0;

  auto addAlign = [&](unsigned at, uint64_t align) {
    target->addParamAttr(at, llvm::Attribute::getWithAlignment(
                                 llvmCtx, llvm::Align(align ? align : 1)));
  };
  auto extension = [](abi::Extend extend) {
    return extend == abi::Extend::Sign ? llvm::Attribute::SExt
                                       : llvm::Attribute::ZExt;
  };

  if (lowering.usesSret()) {
    target->addParamAttr(idx, llvm::Attribute::getWithStructRetType(
                                  llvmCtx, lowering.ret.type));
    addAlign(idx, lowering.ret.align);
    ++idx;
  } else if (lowering.ret.extend != abi::Extend::None) {
    target->addRetAttr(extension(lowering.ret.extend));
  }

  for (const auto& param : lowering.params) {
    if (param.isIndirect()) {
      // SysV spells "in memory" as a byval pointer; AAPCS64 passes a plain
      // pointer to a caller-made copy, so no attribute there.
      if (param.indirectByval) {
        target->addParamAttr(
            idx, llvm::Attribute::getWithByValType(llvmCtx, param.type));
        addAlign(idx, param.align);
      }
      ++idx;
    } else if (param.isCoerced()) {
      for (size_t i = 0; i < param.pieces.size(); ++i, ++idx) {
        if (param.stackAlign) {
          target->addParamAttr(idx,
                               llvm::Attribute::getWithStackAlignment(
                                   llvmCtx, llvm::Align(param.stackAlign)));
        }
      }
    } else {
      if (param.extend != abi::Extend::None) {
        target->addParamAttr(idx, extension(param.extend));
      }
      ++idx;
    }
  }
}

}  // namespace

llvm::Function* ExternCEmitter::declare(
    const PrototypeAST& proto, llvm::Type* returnType,
    llvm::ArrayRef<llvm::Type*> paramTypes) {
  // Record the rename before anything else, so call sites resolve even when
  // the declaration itself was already emitted and we return early below.
  if (proto.hasLinkName()) {
    symbolNames_[proto.getName()] = proto.getLinkName();
  }

  const std::string& symbol = proto.getLinkName();
  if (llvm::GlobalValue* named = module_->getNamedValue(symbol);
      named && !llvm::isa<llvm::Function>(named)) {
    logAndThrowError("C symbol '" + symbol +
                     "' was already declared as a global variable");
  }
  if (llvm::Function* existing = module_->getFunction(symbol)) {
    // The function may have been created by another path (linked .moon
    // bitcode, a prior declaration). Without a registered lowering,
    // needsMarshalling() would silently answer no and calls would skip the
    // ABI rewriting the signature was built with.
    if (!lowerings_.count(symbol)) {
      abi::SignednessInfo signs = signednessOf(proto);
      auto lowering =
          abi::lowerCSignature(targetTriple(module_), returnType, paramTypes,
                               module_->getDataLayout(), &signs);
      llvm::FunctionType* expected = abi::buildLoweredFunctionType(
          lowering, ctx_.getContext(), proto.isCVariadic());
      if (expected != existing->getFunctionType()) {
        logAndThrowError("extern \"C\" declaration of '" + symbol +
                         "' does not match the signature it was previously "
                         "declared or compiled with");
      }
      lowerings_[symbol] = lowering;
      applyAttributes(existing, lowering);
    }
    // A C extern's name *is* its ABI; tag it so .moon bundling leaves the
    // symbol alone.
    existing->addFnAttr("sun.cabi");
    return existing;
  }

  // Apply the C ABI to the signature. Scalars and pointers come back
  // unchanged; aggregates are coerced into register-sized pieces or passed
  // through memory. LLVM does none of this on its own.
  abi::SignednessInfo signs = signednessOf(proto);
  auto lowering =
      abi::lowerCSignature(targetTriple(module_), returnType, paramTypes,
                           module_->getDataLayout(), &signs);
  llvm::FunctionType* funcType = abi::buildLoweredFunctionType(
      lowering, ctx_.getContext(), proto.isCVariadic());

  llvm::Function* func = llvm::Function::Create(
      funcType, llvm::Function::ExternalLinkage, symbol, module_);

  // Key the plan by the emitted symbol so any path that finds the Function
  // can find the marshalling it was built with.
  lowerings_[func->getName().str()] = lowering;
  applyAttributes(func, lowering);
  func->addFnAttr("sun.cabi");

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

llvm::GlobalVariable* ExternCEmitter::declareGlobal(
    const VariableCreationAST& variable, llvm::Type* valueType) {
  const std::string& symbol = variable.getLinkName();
  mapSunName(variable.getMangledName(), symbol);

  if (llvm::GlobalValue* named = module_->getNamedValue(symbol)) {
    auto* existing = llvm::dyn_cast<llvm::GlobalVariable>(named);
    if (!existing) {
      logAndThrowError("C symbol '" + symbol +
                       "' was already declared as a function");
    }
    if (existing->getValueType() != valueType) {
      logAndThrowError("extern C global '" + symbol +
                       "' has conflicting declared types");
    }
    if (existing->hasInitializer()) {
      logAndThrowError("extern C global '" + symbol +
                       "' collides with a global definition");
    }
    existing->setMetadata(
        "sun.cabi", llvm::MDNode::get(ctx_.getContext(), {}));
    return existing;
  }

  auto* global = new llvm::GlobalVariable(
      *module_, valueType, /*isConstant=*/false,
      llvm::GlobalValue::ExternalLinkage, /*Initializer=*/nullptr, symbol);
  global->setMetadata("sun.cabi",
                      llvm::MDNode::get(ctx_.getContext(), {}));
  return global;
}

void ExternCEmitter::mapSunName(const std::string& sunName,
                                const std::string& symbol) {
  symbolNames_[sunName] = symbol;
}

const std::string& ExternCEmitter::symbolFor(const std::string& sunName) const {
  auto it = symbolNames_.find(sunName);
  return it == symbolNames_.end() ? sunName : it->second;
}

const abi::SignatureLowering* ExternCEmitter::loweringFor(
    const llvm::Function* func) const {
  if (!func) return nullptr;
  auto it = lowerings_.find(func->getName().str());
  return it == lowerings_.end() ? nullptr : &it->second;
}

bool ExternCEmitter::needsMarshalling(const llvm::Function* func) const {
  const abi::SignatureLowering* lowering = loweringFor(func);
  return lowering && !lowering->isTrivial();
}

void ExternCEmitter::applyAttributes(
    llvm::Function* func, const abi::SignatureLowering& lowering) const {
  attachLoweringAttributes(func, lowering, ctx_.getContext());
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
    bool isUnsigned = sunType && sunType->isIntegral() && !sunType->isSigned();
    return isUnsigned ? ctx_.builder->CreateZExt(value, i32Ty, "vararg.zext")
                      : ctx_.builder->CreateSExt(value, i32Ty, "vararg.sext");
  }

  return value;
}

llvm::Value* ExternCEmitter::loadPiece(llvm::Value* aggregateAddr,
                                       llvm::Type* pieceType, uint64_t offset,
                                       uint64_t aggregateSize) const {
  const llvm::DataLayout& dl = module_->getDataLayout();
  llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx_.getContext());

  llvm::Value* src = aggregateAddr;
  if (offset != 0) {
    src = ctx_.builder->CreateConstInBoundsGEP1_64(i8Ty, aggregateAddr, offset,
                                                   "cabi.piece.addr");
  }

  // The coerced type can overhang the aggregate — AAPCS64 reads a 12-byte
  // struct as [2 x i64]. Bounce through a slot of the piece's full size so
  // the load stays in bounds; the callee ignores the padding bytes.
  uint64_t pieceSize = dl.getTypeStoreSize(pieceType);
  if (offset + pieceSize > aggregateSize) {
    llvm::AllocaInst* bounce = entryAlloca(pieceType, "cabi.piece.pad");
    ctx_.builder->CreateMemCpy(bounce, llvm::MaybeAlign(), src,
                               llvm::MaybeAlign(), aggregateSize - offset);
    src = bounce;
  }
  return ctx_.builder->CreateLoad(pieceType, src, "cabi.piece");
}

void ExternCEmitter::storePiece(llvm::Value* piece, llvm::Value* aggregateAddr,
                                uint64_t offset, uint64_t aggregateSize) const {
  const llvm::DataLayout& dl = module_->getDataLayout();
  llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx_.getContext());

  llvm::Value* dest = aggregateAddr;
  if (offset != 0) {
    dest = ctx_.builder->CreateConstInBoundsGEP1_64(i8Ty, aggregateAddr, offset,
                                                    "cabi.ret.addr");
  }

  // Mirror of loadPiece: never write the coerced type's padding over memory
  // past the aggregate's end.
  uint64_t pieceSize = dl.getTypeStoreSize(piece->getType());
  if (offset + pieceSize > aggregateSize) {
    llvm::AllocaInst* bounce = entryAlloca(piece->getType(), "cabi.ret.pad");
    ctx_.builder->CreateStore(piece, bounce);
    ctx_.builder->CreateMemCpy(dest, llvm::MaybeAlign(), bounce,
                               llvm::MaybeAlign(), aggregateSize - offset);
    return;
  }
  ctx_.builder->CreateStore(piece, dest);
}

llvm::Value* ExternCEmitter::emitCall(llvm::Function* func,
                                      llvm::ArrayRef<PreparedArg> args,
                                      CallEmitter emitCallInsn) {
  const abi::SignatureLowering* lowering = loweringFor(func);
  if (!lowering) return nullptr;

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
    const abi::ArgLowering* plan =
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
      // The callee owns a private copy — byval or not — so give it a fresh
      // one rather than the caller's live object.
      llvm::AllocaInst* copy = entryAlloca(plan->type, "cabi.byval");
      ctx_.builder->CreateMemCpy(copy, llvm::MaybeAlign(plan->align), addr,
                                 llvm::MaybeAlign(plan->align),
                                 dl.getTypeAllocSize(plan->type));
      loweredArgs.push_back(copy);
      continue;
    }

    // Coerced: reload the aggregate's bytes as the register piece(s) the ABI
    // wants, one LLVM argument each.
    uint64_t aggregateSize = dl.getTypeAllocSize(plan->type);
    for (size_t piece = 0; piece < plan->pieces.size(); ++piece) {
      loweredArgs.push_back(loadPiece(
          addr, plan->pieces[piece], plan->pieceOffsets[piece], aggregateSize));
    }
  }

  llvm::Value* result =
      emitCallInsn(func->getFunctionType(), func, loweredArgs);

  // The backend lowers a call from the call site's attributes, not the
  // declaration's, so the sret/byval/extension story must be retold here.
  if (auto* callSite = llvm::dyn_cast_or_null<llvm::CallBase>(result)) {
    attachLoweringAttributes(callSite, *lowering, ctx_.getContext());
  }

  // sret already wrote into our buffer; its address is the Sun-level result.
  if (lowering->usesSret()) return sretSlot;

  // A coerced return arrives as register pieces. Write them back over a
  // buffer of the real struct type and hand back its address, which is how
  // Sun represents a struct value.
  if (lowering->ret.isCoerced() && !lowering->ret.pieces.empty()) {
    llvm::AllocaInst* slot = entryAlloca(lowering->ret.type, "cabi.ret");
    uint64_t aggregateSize = dl.getTypeAllocSize(lowering->ret.type);
    if (lowering->ret.pieces.size() == 1) {
      storePiece(result, slot, lowering->ret.pieceOffsets[0], aggregateSize);
    } else {
      for (unsigned piece = 0; piece < lowering->ret.pieces.size(); ++piece) {
        llvm::Value* part =
            ctx_.builder->CreateExtractValue(result, piece, "cabi.ret.part");
        storePiece(part, slot, lowering->ret.pieceOffsets[piece],
                   aggregateSize);
      }
    }
    return slot;
  }

  // A Direct aggregate return (AAPCS64 HFAs stay the literal struct type)
  // still needs an address to act as a Sun struct value.
  if (lowering->ret.isDirect() && result && result->getType()->isStructTy()) {
    llvm::AllocaInst* slot = entryAlloca(lowering->ret.type, "cabi.ret");
    ctx_.builder->CreateStore(result, slot);
    return slot;
  }

  return result;
}

}  // namespace sun::cabi
