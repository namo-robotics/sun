#pragma once

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
class PrototypeAST;
}

#include <map>
#include <stdexcept>

#include "driver/sun_jit.h"
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
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

/** Declares LLVM types referenced by the compiler interfaces. */
using namespace llvm;

/** Translates analyzed Sun programs into LLVM instructions. */
namespace sun::codegen {}  // namespace sun::codegen

/** Translates analyzed Sun programs into LLVM instructions. */
namespace sun::codegen {

/** Creates the LLVM generation context for the requested module and target. */
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
  std::shared_ptr<sun::driver::SunJIT>
      jit;  // shared JIT instance, preserved across reinitializations

 private:
  std::string moduleName;
  bool ownsContext = true;  // Whether this context owns its LLVMContext
  // AOT cross-compilation target; empty means the host (JIT ignores it —
  // it can only ever execute on the host).
  std::string targetTriple_;
  // Emit DWARF debug info (-g)
  bool debugInfo_ = false;
  bool optimize_ = true;

 public:
  /** Creates the LLVM generation context for the requested module and target. */
  explicit CodegenContext(std::string moduleName,
                          const std::shared_ptr<sun::driver::SunJIT>& jit,
                          LLVMContext* existingContext = nullptr,
                          std::string targetTriple = "", bool debugInfo = false,
                          bool optimize = true)
      : moduleName(std::move(moduleName)),
        jit(jit),
        targetTriple_(std::move(targetTriple)),
        debugInfo_(debugInfo),
        optimize_(optimize) {
    ownsContext = existingContext == nullptr;
    if (existingContext) {
      initializeModule(*existingContext);
    } else {
      createNewModule();
    }
  }

  /** Starts code generation in a new or supplied LLVM module. */
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
  /** Sets up the LLVM module and its target-dependent state. */
  void initializeModule(LLVMContext& ctx) {
    mainModule = std::make_unique<Module>(moduleName, ctx);
    builder = std::make_unique<IRBuilder<>>(ctx);
    initializePasses(ctx);
  }

  /** Configures the LLVM passes used to optimize generated code. */
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
    if (!targetTriple_.empty()) {
      // Cross-compiling: the requested backend is not the native one.
      InitializeAllTargetInfos();
      InitializeAllTargets();
      InitializeAllTargetMCs();
      InitializeAllAsmPrinters();
      InitializeAllAsmParsers();
    }

    si->registerCallbacks(*pic, mam.get());

    fpm->addPass(PromotePass());
    fpm->addPass(InstCombinePass());
    fpm->addPass(ReassociatePass());
    fpm->addPass(GVNPass());
    fpm->addPass(SimplifyCFGPass());

    PassBuilder PB;
    PB.registerModuleAnalyses(*mam);
    PB.registerFunctionAnalyses(*fam);
    PB.registerLoopAnalyses(*lam);
    PB.registerCGSCCAnalyses(*cgam);
    PB.crossRegisterProxies(*lam, *fam, *cgam, *mam);

    if (jit) {
      mainModule->setDataLayout(jit->getDataLayout());
      // C ABI classification dispatches on the triple, not just the layout.
      mainModule->setTargetTriple(jit->getTargetTriple().str());
    } else {
      // AOT: emitObjectFile sets the real layout, but only after codegen has
      // run. C ABI classification needs true field offsets while emitting,
      // and LLVM's default layout aligns i64 to 32 bits — which would make
      // `{i32, i64}` 12 bytes instead of 16 and misclassify it. Establish the
      // target's layout up front (host unless cross-compiling). Targets were
      // initialized just above.
      auto triple = targetTriple_.empty() ? llvm::sys::getDefaultTargetTriple()
                                          : targetTriple_;
      std::string err;
      const auto* target = llvm::TargetRegistry::lookupTarget(triple, err);
      if (!target && !targetTriple_.empty()) {
        // Silently falling back to the host layout would miscompile the
        // requested target; a bad --target is a hard error.
        throw std::runtime_error("unknown target '" + triple + "': " + err);
      }
      if (target) {
        llvm::TargetOptions opt;
        if (auto* tm = target->createTargetMachine(triple, "generic", "", opt,
                                                   llvm::Reloc::PIC_)) {
          mainModule->setTargetTriple(triple);
          mainModule->setDataLayout(tm->createDataLayout());
          delete tm;
        }
      }
    }
  }

 public:
  /**
   * Always use this to get the LLVM context - handles both owned and borrowed
   * cases
   */
  LLVMContext& getContext() const { return mainModule->getContext(); }

  /** Reports whether source-level debug metadata should be emitted. */
  bool debugInfoEnabled() const { return debugInfo_; }

  /**
   * Whether to run IR optimization passes. Independent of debug info.
   */
  bool optimizationEnabled() const { return optimize_; }

  /** Destroys this object and releases its owned members. */
  ~CodegenContext() = default;

  /**
   * Delete copy operations (LLVMContext cannot be shared/copied this way)
   */
  CodegenContext(const CodegenContext&) = delete;
  /** Disallows assignment so ownership and object identity cannot be duplicated. */
  CodegenContext& operator=(const CodegenContext&) = delete;

  /**
   * Allow move semantics if needed
   */
  CodegenContext(CodegenContext&&) = default;
  /** Transfers the stored state from another instance during move assignment. */
  CodegenContext& operator=(CodegenContext&&) = default;

  /**
   * Helper for temporary expression evaluation
   */
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
}  // namespace sun::codegen
