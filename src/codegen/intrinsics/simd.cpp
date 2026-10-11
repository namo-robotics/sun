/** Lowers fixed-width float operations while keeping vectors internal to LLVM.
 */
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Metadata.h>
#include <llvm/TargetParser/Triple.h>

#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics_generator.h"
#include "support/error.h"

/** Implements compiler-provided operations. */
namespace sun::codegen::intrinsics {
llvm::Value* IntrinsicsGenerator::codegenSimdF32(const CallExprAST& expr,
                                                 unsigned lanes,
                                                 const std::string& operation) {
  const bool fused = operation == "fma" || operation == "mul_add";
  const bool binary =
      operation == "add" || operation == "sub" || operation == "mul" || fused;
  if (expr.getArgs().size() != (fused ? 4 : binary ? 3 : 2))
    sun::support::logAndThrowError("Invalid argument count for SIMD intrinsic");
  auto* output = codegen(*expr.getArgs()[0]);
  auto* input = codegen(*expr.getArgs()[1]);
  auto* right = binary ? codegen(*expr.getArgs()[2]) : nullptr;
  auto* addend = fused ? codegen(*expr.getArgs()[3]) : nullptr;
  llvm::IRBuilderBase::FastMathFlagGuard flags(*ctx.builder);
  ctx.builder->clearFastMathFlags();
  auto* vectorType = llvm::FixedVectorType::get(
      llvm::Type::getFloatTy(ctx.getContext()), lanes);
  llvm::Value* result;
  if (operation == "splat") {
    result = ctx.builder->CreateVectorSplat(lanes, input, "simd.splat");
  } else {
    auto* a = ctx.builder->CreateAlignedLoad(vectorType, input, llvm::Align(4),
                                             "simd.a");
    result = a;
    if (binary) {
      auto* b = ctx.builder->CreateAlignedLoad(vectorType, right,
                                               llvm::Align(4), "simd.b");
      if (fused) {
        auto* c = ctx.builder->CreateAlignedLoad(vectorType, addend,
                                                 llvm::Align(4), "simd.c");
        result = ctx.builder->CreateIntrinsic(
            (operation == "fma" ? llvm::Intrinsic::fma
                                : llvm::Intrinsic::fmuladd),
            {vectorType}, {a, b, c}, nullptr, "simd.fma");
      }
      if (operation == "add")
        result = ctx.builder->CreateFAdd(a, b, "simd.add");
      if (operation == "sub")
        result = ctx.builder->CreateFSub(a, b, "simd.sub");
      if (operation == "mul")
        result = ctx.builder->CreateFMul(a, b, "simd.mul");
    }
  }
  return ctx.builder->CreateAlignedStore(result, output, llvm::Align(4));
}
}  // namespace sun::codegen::intrinsics

/** Implements runtime dispatch and isolated vector kernels with scalar ABIs. */
namespace sun::codegen::intrinsics {
llvm::Value* IntrinsicsGenerator::codegenCpuAvx2Fma(const CallExprAST& expr) {
  if (!expr.getArgs().empty())
    sun::support::logAndThrowError("_cpu_avx2_fma expects no arguments");
  if (llvm::Triple(module->getTargetTriple()).getArch() != llvm::Triple::x86_64)
    return ctx.builder->getFalse();
  auto* fn = module->getFunction("__sun_cpu_avx2_fma");
  if (!fn) {
    auto& context = ctx.getContext();
    llvm::IRBuilder<> b(context);
    auto* type = llvm::FunctionType::get(b.getInt1Ty(), false);
    fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                "__sun_cpu_avx2_fma", module);
    fn->addFnAttr(llvm::Attribute::NoInline);
    fn->addFnAttr("target-cpu", "x86-64");
    fn->addFnAttr("target-features", "-avx,-avx2,-fma,-avx512f");
    auto* entry = llvm::BasicBlock::Create(context, "entry", fn);
    auto* features = llvm::BasicBlock::Create(context, "features", fn);
    auto* state = llvm::BasicBlock::Create(context, "os.state", fn);
    auto* avx2 = llvm::BasicBlock::Create(context, "avx2", fn);
    auto* no = llvm::BasicBlock::Create(context, "unsupported", fn);
    auto* regs = llvm::StructType::get(
        context,
        {b.getInt32Ty(), b.getInt32Ty(), b.getInt32Ty(), b.getInt32Ty()});
    auto* cpuidType =
        llvm::FunctionType::get(regs, {b.getInt32Ty(), b.getInt32Ty()}, false);
    auto* cpuid = llvm::InlineAsm::get(cpuidType, "cpuid",
                                       "={ax},={bx},={cx},={dx},0,2", true);
    b.SetInsertPoint(entry);
    auto* limits = b.CreateCall(cpuid, {b.getInt32(0), b.getInt32(0)});
    b.CreateCondBr(
        b.CreateICmpUGE(b.CreateExtractValue(limits, 0), b.getInt32(7)),
        features, no);
    b.SetInsertPoint(features);
    auto* leaf1 = b.CreateCall(cpuid, {b.getInt32(1), b.getInt32(0)});
    const uint32_t mask = (1u << 12) | (1u << 26) | (1u << 27) | (1u << 28);
    auto* ecx = b.CreateExtractValue(leaf1, 2);
    b.CreateCondBr(
        b.CreateICmpEQ(b.CreateAnd(ecx, b.getInt32(mask)), b.getInt32(mask)),
        state, no);
    b.SetInsertPoint(state);
    auto* xcrType = llvm::FunctionType::get(
        llvm::StructType::get(context, {b.getInt32Ty(), b.getInt32Ty()}),
        {b.getInt32Ty()}, false);
    auto* xgetbv =
        llvm::InlineAsm::get(xcrType, "xgetbv", "={ax},={dx},{cx}", true);
    auto* xcr = b.CreateExtractValue(b.CreateCall(xgetbv, {b.getInt32(0)}), 0);
    // XGETBV is reached only after OSXSAVE; both XMM and YMM state must be
    // enabled.
    b.CreateCondBr(
        b.CreateICmpEQ(b.CreateAnd(xcr, b.getInt32(6)), b.getInt32(6)), avx2,
        no);
    b.SetInsertPoint(avx2);
    auto* leaf7 = b.CreateCall(cpuid, {b.getInt32(7), b.getInt32(0)});
    auto* ebx = b.CreateExtractValue(leaf7, 1);
    b.CreateRet(
        b.CreateICmpNE(b.CreateAnd(ebx, b.getInt32(1u << 5)), b.getInt32(0)));
    b.SetInsertPoint(no);
    b.CreateRet(b.getFalse());
  }
  return ctx.builder->CreateCall(fn);
}

llvm::Value* IntrinsicsGenerator::codegenGemmAvx2Fma(
    const CallExprAST& expr, bool block) {
  if (expr.getArgs().size() != (block ? 8 : 6))
    sun::support::logAndThrowError("Invalid packed GEMM argument count");
  llvm::SmallVector<llvm::Value*, 8> args;
  for (const auto& arg : expr.getArgs()) args.push_back(codegen(*arg));
  const auto* name =
      block ? "__sun_gemm_block_avx2_fma" : "__sun_gemm_tile_avx2_fma";
  auto* fn = module->getFunction(name);
  if (!fn) {
    auto& context = ctx.getContext();
    llvm::IRBuilder<> b(context);
    auto* f32 = b.getFloatTy();
    auto* vec = llvm::FixedVectorType::get(f32, 8);
    auto* ptr = llvm::PointerType::getUnqual(context);
    llvm::SmallVector<llvm::Type*, 8> parameters = {
        ptr, ptr, ptr, b.getInt64Ty(), b.getInt64Ty(), f32};
    if (block) {
      parameters.push_back(b.getInt64Ty());
      parameters.push_back(b.getInt64Ty());
    }
    auto* type = llvm::FunctionType::get(b.getVoidTy(), parameters, false);
    fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name,
                                module);
    // Prevent AVX instructions from escaping into a baseline caller through
    // inlining.
    fn->addFnAttr(llvm::Attribute::NoInline);
    const bool x86 = llvm::Triple(module->getTargetTriple()).getArch() ==
                     llvm::Triple::x86_64;
    if (x86) {
      fn->addFnAttr("target-cpu", "x86-64");
      fn->addFnAttr("target-features", "+avx,+avx2,+fma,-avx512f");
    }
    llvm::Value* a = fn->getArg(0);
    llvm::Value* inputB = fn->getArg(1);
    llvm::Value* c = fn->getArg(2);
    auto* n = fn->getArg(3);
    auto* k = fn->getArg(4);
    auto* beta = fn->getArg(5);
    auto* entry = llvm::BasicBlock::Create(context, "entry", fn);
    auto* load = llvm::BasicBlock::Create(context, "load.output", fn);
    auto* scale = llvm::BasicBlock::Create(context, "scale.output", fn);
    auto* start = llvm::BasicBlock::Create(context, "start", fn);
    auto* loop = llvm::BasicBlock::Create(context, "loop", fn);
    auto* body = llvm::BasicBlock::Create(context, "body", fn);
    auto* done = llvm::BasicBlock::Create(context, "done", fn);
    llvm::Value* output[8];
    llvm::Value* initialized[8];
    llvm::Value* scaled[8];
    llvm::PHINode* initial[8];
    llvm::PHINode* sums[8];
    llvm::BasicBlock* columns = nullptr;
    llvm::PHINode* column = nullptr;
    if (block) {
      // Keep all tiles within one targeted function so no baseline/AVX
      // transition is needed between tiles in the packed block.
      auto* begin = llvm::BasicBlock::Create(context, "block.entry", fn, entry);
      auto* rows = llvm::BasicBlock::Create(context, "rows", fn);
      auto* rowBody = llvm::BasicBlock::Create(context, "row.body", fn);
      auto* nextRow = llvm::BasicBlock::Create(context, "row.next", fn);
      columns = llvm::BasicBlock::Create(context, "columns", fn);
      auto* exit = llvm::BasicBlock::Create(context, "exit", fn);
      b.SetInsertPoint(begin);
      b.CreateBr(rows);
      b.SetInsertPoint(rows);
      auto* row = b.CreatePHI(b.getInt64Ty(), 2);
      row->addIncoming(b.getInt64(0), begin);
      b.CreateCondBr(b.CreateICmpSLT(row, fn->getArg(6)), rowBody, exit);
      b.SetInsertPoint(rowBody);
      b.CreateBr(columns);
      b.SetInsertPoint(columns);
      column = b.CreatePHI(b.getInt64Ty(), 2);
      column->addIncoming(b.getInt64(0), rowBody);
      b.CreateCondBr(b.CreateICmpSLT(column, fn->getArg(7)), entry, nextRow);
      b.SetInsertPoint(nextRow);
      row->addIncoming(b.CreateAdd(row, b.getInt64(4)), nextRow);
      b.CreateBr(rows);
      b.SetInsertPoint(exit);
      b.CreateRetVoid();
      b.SetInsertPoint(entry);
      a = b.CreateGEP(f32, a, b.CreateMul(row, k));
      inputB = b.CreateGEP(f32, inputB, b.CreateMul(column, k));
      c = b.CreateGEP(f32, c, b.CreateAdd(b.CreateMul(row, n), column));
    } else {
      b.SetInsertPoint(entry);
    }
    for (unsigned i = 0; i < 8; ++i) {
      auto* offset = b.CreateAdd(b.CreateMul(n, b.getInt64(i / 2)),
                                 b.getInt64((i % 2) * 8));
      output[i] = b.CreateGEP(f32, c, offset);
    }
    b.CreateCondBr(b.CreateFCmpOEQ(beta, llvm::ConstantFP::get(f32, 0)), start,
                   load);
    b.SetInsertPoint(load);
    for (unsigned i = 0; i < 8; ++i)
      initialized[i] = b.CreateAlignedLoad(vec, output[i], llvm::Align(4));
    b.CreateCondBr(b.CreateFCmpOEQ(beta, llvm::ConstantFP::get(f32, 1)), start,
                   scale);
    b.SetInsertPoint(scale);
    auto* factor = b.CreateVectorSplat(8, beta);
    for (unsigned i = 0; i < 8; ++i)
      scaled[i] = b.CreateFMul(factor, initialized[i]);
    b.CreateBr(start);
    b.SetInsertPoint(start);
    for (unsigned i = 0; i < 8; ++i) {
      initial[i] = b.CreatePHI(vec, 3);
      initial[i]->addIncoming(llvm::Constant::getNullValue(vec), entry);
      initial[i]->addIncoming(initialized[i], load);
      initial[i]->addIncoming(scaled[i], scale);
    }
    b.CreateBr(loop);
    b.SetInsertPoint(loop);
    auto* p = b.CreatePHI(b.getInt64Ty(), 2);
    p->addIncoming(b.getInt64(0), start);
    for (unsigned i = 0; i < 8; ++i) {
      sums[i] = b.CreatePHI(vec, 2);
      sums[i]->addIncoming(initial[i], start);
    }
    b.CreateCondBr(b.CreateICmpSLT(p, k), body, done);
    b.SetInsertPoint(body);
    llvm::Value* bv[2];
    for (unsigned j = 0; j < 2; ++j) {
      auto* offset = b.CreateMul(b.CreateAdd(p, b.CreateMul(k, b.getInt64(j))),
                                 b.getInt64(8));
      bv[j] = b.CreateAlignedLoad(vec, b.CreateGEP(f32, inputB, offset),
                                  llvm::Align(4));
    }
    for (unsigned row = 0; row < 4; ++row) {
      auto* offset =
          b.CreateAdd(b.CreateMul(p, b.getInt64(4)), b.getInt64(row));
      auto* av = b.CreateVectorSplat(
          8, b.CreateAlignedLoad(f32, b.CreateGEP(f32, a, offset),
                                 llvm::Align(4)));
      for (unsigned j = 0; j < 2; ++j) {
        auto* sum = b.CreateIntrinsic(
            x86 ? llvm::Intrinsic::fma : llvm::Intrinsic::fmuladd, {vec},
            {av, bv[j], sums[row * 2 + j]});
        sums[row * 2 + j]->addIncoming(sum, body);
      }
    }
    p->addIncoming(b.CreateAdd(p, b.getInt64(1)), body);
    auto* backedge = b.CreateBr(loop);
    if (x86) {
      // Expose adjacent loads to the scheduler without changing the order of
      // each sum. LLVM retains a cleanup iteration for odd inner dimensions.
      auto* count = llvm::MDNode::get(
          context, {llvm::MDString::get(context, "llvm.loop.unroll.count"),
                    llvm::ConstantAsMetadata::get(b.getInt32(2))});
      auto* metadata = llvm::MDNode::getDistinct(context, {nullptr, count});
      metadata->replaceOperandWith(0, metadata);
      backedge->setMetadata(llvm::LLVMContext::MD_loop, metadata);
    }
    b.SetInsertPoint(done);
    for (unsigned i = 0; i < 8; ++i)
      b.CreateAlignedStore(sums[i], output[i], llvm::Align(4));
    if (block) {
      column->addIncoming(b.CreateAdd(column, b.getInt64(16)), done);
      b.CreateBr(columns);
    } else {
      b.CreateRetVoid();
    }
  }
  return ctx.builder->CreateCall(fn, args);
}
}  // namespace sun::codegen::intrinsics
