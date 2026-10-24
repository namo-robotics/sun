#include "abi/cpu_requirements.h"

#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>

/** Describes CPU requirements shared by lowering and bundle validation. */
namespace sun::abi {
llvm::Function* createAvx2FmaProbe(llvm::Module& module) {
  auto* fn = module.getFunction("__sun_cpu_avx2_fma");
  if (!fn) {
    auto& context = module.getContext();
    llvm::IRBuilder<> b(context);
    auto* type = llvm::FunctionType::get(b.getInt1Ty(), false);
    fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                "__sun_cpu_avx2_fma", &module);
    fn->addFnAttr(llvm::Attribute::NoInline);
    fn->addFnAttr(llvm::Attribute::OptimizeNone);
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
  return fn;
}

/** Keeps independent probe comparison and control-flow checks private. */
namespace {
/** Prints only the body, excluding linker-renamed function symbols. */
std::string probeBody(const llvm::Function& function) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  for (const auto& block : function) {
    block.print(stream);
  }
  return text;
}

/** Checks the baseline probe's actual body rather than trusting its name. */
bool isAvx2FmaProbe(const llvm::Function& function) {
  if (function.isDeclaration() || !function.hasLocalLinkage() ||
      !function.hasFnAttribute(llvm::Attribute::NoInline) ||
      !function.hasFnAttribute(llvm::Attribute::OptimizeNone) ||
      !function.hasFnAttribute("target-cpu") ||
      function.getFnAttribute("target-cpu").getValueAsString() != "x86-64" ||
      !function.hasFnAttribute("target-features") ||
      function.getFnAttribute("target-features").getValueAsString() !=
          "-avx,-avx2,-fma,-avx512f" ||
      !function.arg_empty() || !function.getReturnType()->isIntegerTy(1)) {
    return false;
  }
  llvm::LLVMContext context;
  llvm::Module reference("cpu.probe", context);
  return probeBody(function) == probeBody(*createAvx2FmaProbe(reference));
}

/** Requires the successful probe edge to dominate this particular call. */
bool isGuardedCall(const llvm::CallBase& call) {
  llvm::DominatorTree dominators(
      *const_cast<llvm::Function*>(call.getFunction()));
  for (const auto& block : *call.getFunction()) {
    const auto* branch =
        llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
    if (!branch || !branch->isConditional()) {
      continue;
    }
    const auto* probe = llvm::dyn_cast<llvm::CallInst>(branch->getCondition());
    if (probe && probe->getCalledFunction() &&
        isAvx2FmaProbe(*probe->getCalledFunction()) &&
        dominators.dominates(
            llvm::BasicBlockEdge(&block, branch->getSuccessor(0)),
            call.getParent())) {
      return true;
    }
  }
  return false;
}
}  // namespace

bool hasGuardedCPURequirements(const llvm::Function& function) {
  if (function.isDeclaration() || !function.hasLocalLinkage() ||
      function.use_empty() ||
      !function.hasFnAttribute(llvm::Attribute::NoInline) ||
      llvm::Triple(function.getParent()->getTargetTriple()).getArch() !=
          llvm::Triple::x86_64 ||
      !function.hasFnAttribute("target-cpu") ||
      function.getFnAttribute("target-cpu").getValueAsString() != "x86-64") {
    return false;
  }
  const auto attribute = function.getFnAttribute("target-features");
  if (!attribute.isValid()) {
    return false;
  }
  llvm::SubtargetFeatures features(attribute.getValueAsString());
  for (const auto& feature : features.getFeatures()) {
    if (feature.starts_with("+") && feature != "+avx" && feature != "+avx2" &&
        feature != "+fma") {
      return false;
    }
  }
  for (const auto& use : function.uses()) {
    const auto* call = llvm::dyn_cast<llvm::CallBase>(use.getUser());
    if (!call || !call->isCallee(&use) ||
        call->getCalledFunction() != &function || !isGuardedCall(*call)) {
      return false;
    }
  }
  return true;
}
}  // namespace sun::abi
