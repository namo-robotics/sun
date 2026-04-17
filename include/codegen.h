#pragma once

#include <map>

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "sun_jit.h"

using namespace llvm;

class PrototypeAST;  // Forward declaration

class CodegenContext {
 public:
  std::unique_ptr<LLVMContext> context;
  std::unique_ptr<IRBuilder<>> builder;
  std::unique_ptr<Module> mainModule;

  // Pass and analysis managers
  std::unique_ptr<FunctionPassManager> fpm;
  std::unique_ptr<LoopAnalysisManager> lam;
  std::unique_ptr<FunctionAnalysisManager> fam;
  std::unique_ptr<CGSCCAnalysisManager> cgam;
  std::unique_ptr<ModuleAnalysisManager> mam;
  std::unique_ptr<PassInstrumentationCallbacks> pic;
  std::unique_ptr<StandardInstrumentations> si;
  std::shared_ptr<SunJIT>
      jit;  // shared JIT instance, preserved across reinitializations

 private:
  std::string moduleName;
  bool ownsContext = true;  // Whether this context owns its LLVMContext

 public:
  explicit CodegenContext(std::string moduleName,
                          const std::shared_ptr<SunJIT>& jit,
                          LLVMContext* existingContext = nullptr)
      : moduleName(std::move(moduleName)),
        jit(jit),
        ownsContext(existingContext == nullptr) {
    if (existingContext) {
      initializeModule(*existingContext);
    } else {
      createNewModule();
    }
  }

  void createNewModule(std::unique_ptr<Module> existingModule = nullptr) {
    context = std::make_unique<LLVMContext>();
    if (existingModule) {
      mainModule = std::move(existingModule);
      builder = std::make_unique<IRBuilder<>>(*context);
    } else {
      initializeModule(*context);
      return;
    }
    // Only reach here if existingModule was provided
    initializePasses(*context);
  }

 private:
  void initializeModule(LLVMContext& ctx) {
    mainModule = std::make_unique<Module>(moduleName, ctx);
    builder = std::make_unique<IRBuilder<>>(ctx);
    initializePasses(ctx);
  }

  void initializePasses(LLVMContext& ctx) {
    fpm = std::make_unique<FunctionPassManager>();
    lam = std::make_unique<LoopAnalysisManager>();
    fam = std::make_unique<FunctionAnalysisManager>();
    cgam = std::make_unique<CGSCCAnalysisManager>();
    mam = std::make_unique<ModuleAnalysisManager>();
    pic = std::make_unique<PassInstrumentationCallbacks>();
    si = std::make_unique<StandardInstrumentations>(ctx, /*DebugLogging*/ true);

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    si->registerCallbacks(*pic, mam.get());

    fpm->addPass(PromotePass());
    fpm->addPass(InstCombinePass());
    fpm->addPass(ReassociatePass());
    fpm->addPass(GVNPass());
    fpm->addPass(SimplifyCFGPass());

    PassBuilder PB;
    PB.registerModuleAnalyses(*mam);
    PB.registerFunctionAnalyses(*fam);
    PB.crossRegisterProxies(*lam, *fam, *cgam, *mam);

    if (jit) {
      mainModule->setDataLayout(jit->getDataLayout());
    }
  }

 public:
  // Always use this to get the LLVM context - handles both owned and borrowed
  // cases
  LLVMContext& getContext() const { return mainModule->getContext(); }

  ~CodegenContext() = default;

  // Delete copy operations (LLVMContext cannot be shared/copied this way)
  CodegenContext(const CodegenContext&) = delete;
  CodegenContext& operator=(const CodegenContext&) = delete;

  // Allow move semantics if needed
  CodegenContext(CodegenContext&&) = default;
  CodegenContext& operator=(CodegenContext&&) = default;

  // Helper for temporary expression evaluation
  std::unique_ptr<llvm::orc::ThreadSafeModule> createTempExpressionModule() {
    auto tempCtx = std::make_unique<LLVMContext>();
    auto tempMod = std::make_unique<Module>("__anon_expr", *tempCtx);

    if (jit) tempMod->setDataLayout(jit->getDataLayout());

    // Important: copy function declarations from main module
    // so we can call previously defined functions from expressions
    for (const Function& F : *mainModule) {
      if (F.isIntrinsic()) continue;

      // Usually copy only externally visible functions
      if (F.hasExternalLinkage() || F.hasWeakLinkage()) {
        tempMod->getOrInsertFunction(F.getName(), F.getFunctionType(),
                                     F.getAttributes());
      }
    }

    return std::make_unique<llvm::orc::ThreadSafeModule>(std::move(tempMod),
                                                         std::move(tempCtx));
  }
};