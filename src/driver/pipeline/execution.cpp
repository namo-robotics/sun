/** Runs borrow checking, code generation, optimization, and JIT execution. */
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "abi/abi_compatibility_checker.h"
#include "borrow_checker/borrow_checker.h"
#include "debug/ast_dot_generator.h"
#include "debug/scope_tree_generator.h"
#include "driver/driver.h"
#include "jit_runtime.h"
#include "moon_bundling/library_cache.h"
#include "moon_bundling/module_linker.h"
#include "support/display_path.h"
#include "support/error.h"
#include "support/stage_timer.h"
#include "support/terminal.h"

using sun::ast::BlockExprAST;
using sun::codegen::CodegenVisitor;
using sun::driver::SunValue;
using sun::driver::VoidValue;
using sun::moon_bundling::LibraryCache;
using sun::moon_bundling::MoonImport;
using sun::parsing::Parser;
using sun::semantic_analysis::AnalysisResults;
using sun::semantic_analysis::SemanticAnalyzer;
using sun::support::ScopedStage;
using sun::support::SunError;

/** Coordinates compilation, dependency loading, linking, and program execution.
 */
namespace sun::driver {

static llvm::ExitOnError ExitOnErr;
using llvm::orc::ThreadSafeModule;

/**
 * Strip library code the program never uses before optimizing an executable.
 * Both optimization and ORC compilation otherwise process unused definitions.
 * Internalize everything except entry points, then GlobalDCE drops anything
 * main cannot reach (references through vtables/globals are preserved).
 */
static void stripUnreachableForExecutable(llvm::Module& module) {
  for (auto& F : module) {
    // Global initializers need no exemption: they are internal already, and
    // llvm.global_ctors (appending linkage, never discarded) keeps them and
    // everything they call alive through GlobalDCE.
    if (!F.isDeclaration() && F.getName() != "main") {
      F.setLinkage(llvm::GlobalValue::InternalLinkage);
    }
  }
  for (auto& G : module.globals()) {
    if (!G.isDeclaration() && !G.getName().starts_with("llvm.")) {
      G.setLinkage(llvm::GlobalValue::InternalLinkage);
    }
  }

  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb;
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  llvm::ModulePassManager mpm;
  mpm.addPass(llvm::GlobalDCEPass());
  mpm.run(module, mam);
}

/**
 * Works out where the startup function of the code being compiled runs
 * relative to those of the bundles it imports: zero when it imports nothing,
 * otherwise one more than the highest value recorded by an import. Lower
 * values run first, so every import's globals are initialized before this
 * code's startup function reads them.
 */
static uint32_t computeStaticInitOrder(
    const std::vector<std::string>& importedModuleKeys,
    const std::vector<sun::moon_bundling::MoonImport>& moonImports) {
  uint32_t order = 0;
  for (const auto& moduleKey : importedModuleKeys) {
    if (const auto* metadata =
            sun::moon_bundling::LibraryCache::instance().getMetadata(
                moduleKey)) {
      order = std::max(order, metadata->static_init_order() + 1);
    }
  }
  for (const auto& moonImport : moonImports) {
    auto reader = sun::moon_bundling::MoonReader::open(moonImport.path);
    if (!reader) {
      continue;
    }
    sun::abi::AbiCompatibilityChecker::current().validate(*reader);
    for (const auto& moduleKey : reader->listModules()) {
      if (const auto* metadata = reader->getMetadata(moduleKey)) {
        order = std::max(order, metadata->static_init_order() + 1);
      }
    }
  }
  return order;
}

/**
 * Make a module's global initializers callable under the JIT. They are
 * internal functions registered in llvm.global_ctors — one per linked module,
 * uniquified by the IR linker — and the JIT resolves symbols by name, which
 * cannot reach an internal function. So wrap every ctor entry in a single
 * external runner for the driver to look up and call before main, in the
 * same order the AOT init_array would use. Returns false when the module has
 * no constructors and there is nothing to run.
 */
static bool wrapStaticCtorsForJIT(llvm::Module& module) {
  auto* ctors = module.getGlobalVariable("llvm.global_ctors");
  if (!ctors || !ctors->hasInitializer()) {
    return false;
  }
  auto* entries = llvm::dyn_cast<llvm::ConstantArray>(ctors->getInitializer());
  if (!entries) {
    return false;
  }

  // Each entry is { i32 priority, ptr function, ptr data }. Lower priority
  // runs first; entries with equal priority keep their link order.
  std::vector<std::pair<uint64_t, llvm::Function*>> fns;
  for (auto& op : entries->operands()) {
    auto* entry = llvm::cast<llvm::ConstantStruct>(op.get());
    auto* fn = llvm::dyn_cast<llvm::Function>(
        entry->getOperand(1)->stripPointerCasts());
    if (!fn || fn->isDeclaration()) {
      continue;
    }
    uint64_t priority =
        llvm::cast<llvm::ConstantInt>(entry->getOperand(0))->getZExtValue();
    fns.push_back({priority, fn});
  }
  if (fns.empty()) {
    return false;
  }
  std::stable_sort(fns.begin(), fns.end(), [](const auto& a, const auto& b) {
    return a.first < b.first;
  });

  auto* runnerType = llvm::FunctionType::get(
      llvm::Type::getVoidTy(module.getContext()), false);
  auto* runner =
      llvm::Function::Create(runnerType, llvm::GlobalValue::ExternalLinkage,
                             "__sun_run_static_ctors", module);
  auto* entry = llvm::BasicBlock::Create(module.getContext(), "entry", runner);
  llvm::IRBuilder<> builder(entry);
  for (const auto& [priority, fn] : fns) {
    builder.CreateCall(fn);
  }
  builder.CreateRetVoid();
  return true;
}

SunValue Driver::runPipeline(std::unique_ptr<BlockExprAST> program,
                             Parser& parser, bool execute, int argc,
                             char** argv) {
  SunValue result = VoidValue{};
  // Declaration records will point at this tree's nodes; keep it with them.
  analyzedTree_ = std::move(program);
  std::unique_ptr<BlockExprAST>& blockAst = analyzedTree_;

  if (!blockAst) {
    sun::support::messageStream("error") << "Failed to parse program.\n";
    return result;
  }

  // Strip or compile test functions before any analysis, so a production
  // build never even sema-analyzes a test (whose helpers may live in
  // test_files this build did not load).
  applyTestHandling(*blockAst);

  // Debug mode: generate AST DOT graph (pre-lowering, lossless parse tree)
  if (debugMode_ && !debugFolder_.empty()) {
    sun::debug::AstDotGenerator dotGen;
    std::string dot = dotGen.generate(blockAst.get());
    std::string dotPath = debugFolder_ + "/ast.dot";
    std::ofstream dotFile(dotPath);
    if (dotFile) {
      dotFile << dot;
      sun::support::logPathMessage("debug", "  Generated: ", dotPath);
    } else {
      sun::support::messageStream("warning")
          << "Could not write " << sun::support::displayPath(dotPath) << "\n";
    }
  }

  analyzeProgram(*blockAst, parser);

  // Debug mode: generate scope tree HTML after semantic analysis
  if (debugMode_ && !debugFolder_.empty()) {
    sun::debug::ScopeTreeGenerator scopeGen;
    std::string html = scopeGen.generateHtml(analyzer->getRootScope());
    std::string scopePath = debugFolder_ + "/scope_tree.html";
    std::ofstream scopeFile(scopePath);
    if (scopeFile) {
      scopeFile << html;
      sun::support::logPathMessage("debug", "  Generated: ", scopePath);
    } else {
      sun::support::messageStream("warning")
          << "Could not write " << sun::support::displayPath(scopePath) << "\n";
    }
  }

  // Run borrow checking on the unified AST
  // Uses compile-time settings from sun::support::Config
  sun::borrow_checker::BorrowChecker borrowChecker;
  std::vector<sun::borrow_checker::BorrowError> borrowErrors;
  {
    ScopedStage stage("borrow check");
    borrowErrors = borrowChecker.check(*blockAst);
  }
  if (!borrowErrors.empty()) {
    // One error carrying every borrow error and related borrow: whoever
    // catches it renders them all, each at its own place in the source
    throw sun::borrow_checker::buildBorrowCheckError(borrowErrors);
  }

  staticInitOrder_ =
      computeStaticInitOrder(parser.getPrecompiledImports(), moonImports_);

  if (metadataCallback_) {
    metadataCallback_(*blockAst, *analyzer);
  }

  // Register precompiled modules for lazy linking
  // This builds the symbol-to-module map without loading bitcode yet
  const auto& precompiledImports = parser.getPrecompiledImports();

  sun::moon_bundling::ModuleLinker linker(*ctx->mainModule);
  bool hasMoonImports = !precompiledImports.empty() || !moonImports_.empty();

  if (!precompiledImports.empty()) {
    linker.registerAvailableModules(precompiledImports);
  }
  for (const auto& moonImport : moonImports_) {
    linker.registerAvailableBundle(moonImport);
  }
  if (hasMoonImports) {
    // Create forward declarations for all functions from bitcode so codegen
    // can reference them before actual linking
    linker.declareAvailableFunctions();
  }

  // Snapshot precompiled function declarations before codegen starts
  // This lets codegen distinguish bitcode declarations from forward decls
  codegenVisitor->snapshotPrecompiledFunctions();

  // Generate code into single module
  {
    ScopedStage stage("codegen");
    codegenVisitor->codegen(*blockAst);
    // Emit static initialization function for globals that need runtime init
    codegenVisitor->emitStaticInitFunction(staticInitOrder_, ownBundleHash_);
    codegenVisitor->externCEmitter().finalizeDeclarations();
  }

  // Bundles retain complete dependencies for future generic specialization.
  // Executables only need implementations reachable from their own code.
  if (hasMoonImports) {
    ScopedStage stage("moon link");
    const bool linked = ownBundleHash_.empty()
                            ? linker.linkOnlyUsedSymbols(true)
                            : linker.linkModules(precompiledImports);
    if (!linked) {
      throw SunError(SunError::Kind::Semantic,
                     "Failed to link precompiled module: " + linker.getError());
    }
    // Strip imported debug info when the current build does not request it.
    if (!ctx->debugInfoEnabled()) {
      sun::codegen::DebugInfoBuilder::stripFromModule(*ctx->mainModule);
    }
  }
  // The archives to link: the manifest's own `archives:` plus those carried
  // by the bundles just linked (a bundle binding a C library carries that
  // library's static archives; they go on disk so the link, or the JIT, can
  // use them).
  collectNativeArchives(hasMoonImports ? linker.getLinkedModules()
                                       : std::set<std::string>{});

  // All codegen is done (including static init); emit the DI finalization
  // before any module verification.
  codegenVisitor->finalizeDebugInfo();

  // Discard unused library definitions before running expensive LLVM passes.
  // Library and object compilation must retain their externally visible API.
  if ((execute || executable_) && ctx->mainModule->getFunction("main")) {
    ScopedStage stage("prune unreachable");
    stripUnreachableForExecutable(*ctx->mainModule);
  }

  // Optimize only after codegen has finished using instruction pointers.
  if (ctx->optimizationEnabled()) {
    ScopedStage stage("optimize");
    // Run the standard pipeline after linking so it can optimize library calls.
    ctx->mpm->run(*ctx->mainModule, *ctx->mam);
  }

  // Debug mode: dump only user-defined IR after codegen (filters out stdlib /
  // moon imports)
  if (debugMode_ && !debugFolder_.empty()) {
    std::string irPath = debugFolder_ + "/ir.ll";
    writeUserDefinedIR(irPath);
    sun::support::logPathMessage("debug", "  Generated: ", irPath);
  }

  // If not executing, handle AOT compilation specifics
  if (!execute) {
    // Handle main function return type for compiled programs
    if (auto* mainFunc = ctx->mainModule->getFunction("main")) {
      llvm::Type* retType = mainFunc->getReturnType();
      if (retType->isVoidTy()) {
        // Wrap void main() to return i32 0
        mainFunc->setName("__sun_main_void");
        // The user-defined set is keyed by name, so the program's own body
        // would drop out of an IR dump under its new name — leaving the dump
        // showing only the wrapper, calling a function that isn't there.
        codegenVisitor->noteUserDefinedFunction("__sun_main_void");
        llvm::FunctionType* wrapperType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(ctx->mainModule->getContext()),
            mainFunc->getFunctionType()->params(), false);
        llvm::Function* wrapper =
            llvm::Function::Create(wrapperType, llvm::Function::ExternalLinkage,
                                   "main", ctx->mainModule.get());
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(
            ctx->mainModule->getContext(), "entry", wrapper);
        llvm::IRBuilder<> builder(bb);
        std::vector<llvm::Value*> args;
        for (auto& arg : wrapper->args()) {
          args.push_back(&arg);
        }
        builder.CreateCall(mainFunc, args);
        builder.CreateRet(llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(ctx->mainModule->getContext()), 0));
      } else if (!retType->isIntegerTy(32)) {
        std::string typeName;
        if (retType->isIntegerTy()) {
          typeName = "i" + std::to_string(retType->getIntegerBitWidth());
        } else if (retType->isFloatTy()) {
          typeName = "f32";
        } else if (retType->isDoubleTy()) {
          typeName = "f64";
        } else if (retType->isPointerTy()) {
          typeName = "pointer (string)";
        } else {
          typeName = "unknown";
        }
        throw SunError(SunError::Kind::Type,
                       "For compiled programs, 'main' must return i32 or void, "
                       "but found return type '" +
                           typeName + "'");
      }
    }

    // Verify the module - invalid IR is a hard compile failure
    {
      ScopedStage stage("verify");
      if (llvm::verifyModule(*ctx->mainModule, &llvm::errs())) {
        throw SunError(SunError::Kind::Compile, "Module verification failed");
      }
    }
    return result;
  }

  // JIT execution mode - print IR if requested
  if (dumpIR) {
    if (dumpReachable) {
      printReachableIR();
    } else {
      printUserDefinedIR();
    }
  }

  // Verify before executing - never run invalid IR
  {
    ScopedStage stage("verify");
    if (llvm::verifyModule(*ctx->mainModule, &llvm::errs())) {
      throw SunError(SunError::Kind::Compile, "Module verification failed");
    }
  }

  llvm::Function* func = ctx->mainModule->getFunction("main");
  if (!func) {
    sun::support::messageStream("error")
        << "Could not find 'main' function in module.\n";
    return result;
  }

  // Get the return type and argument count before we clone the module
  llvm::Type* returnType = func->getReturnType();
  size_t mainArgCount = func->arg_size();

  // Create a NEW context for the JIT module
  auto jitStage = std::make_unique<ScopedStage>("jit compile");
  auto anonContext = std::make_unique<llvm::LLVMContext>();

  // Clone the module into the new context
  auto moduleClone = llvm::CloneModule(*ctx->mainModule);
  bool hasStaticCtors = wrapStaticCtorsForJIT(*moduleClone);

  // Add the cloned module to JIT with its own context
  // Archives carried by imported bundles resolve like linked libraries, and
  // the exit handlers their code registers run when this run ends
  registerArchivesWithJIT();
  JITExitScope exitHandlers;

  auto RT = ctx->jit->getMainJITDylib().createResourceTracker();
  ExitOnErr(ctx->jit->addModule(
      ThreadSafeModule(std::move(moduleClone), std::move(anonContext)), RT));

  // Run global initializers (the program's and every linked bundle's) before
  // main, the way the AOT init_array would
  if (hasStaticCtors) {
    auto initSym = ExitOnErr(ctx->jit->lookup("__sun_run_static_ctors"));
    void (*initFP)() = initSym.getAddress().toPtr<void (*)()>();
    initFP();
  }

  // Lookup and execute with appropriate type
  auto ExprSymbol = ExitOnErr(ctx->jit->lookup("main"));

  jitStage.reset();

  // Check if main takes arguments (argc, argv)
  bool mainHasArgs = (mainArgCount == 2);

  // Handle all primitive return types
  if (returnType->isVoidTy()) {
    if (mainHasArgs) {
      void (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<void (*)(int, char**)>();
      FP(argc, argv);
    } else {
      void (*FP)() = ExprSymbol.getAddress().toPtr<void (*)()>();
      FP();
    }
    result = VoidValue{};
  } else if (returnType->isIntegerTy()) {
    unsigned bitWidth = returnType->getIntegerBitWidth();
    if (bitWidth == 1) {
      // bool (i1)
      bool boolResult;
      if (mainHasArgs) {
        bool (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<bool (*)(int, char**)>();
        boolResult = FP(argc, argv);
      } else {
        bool (*FP)() = ExprSymbol.getAddress().toPtr<bool (*)()>();
        boolResult = FP();
      }
      result = boolResult;
    } else if (bitWidth == 8) {
      int8_t intResult;
      if (mainHasArgs) {
        int8_t (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<int8_t (*)(int, char**)>();
        intResult = FP(argc, argv);
      } else {
        int8_t (*FP)() = ExprSymbol.getAddress().toPtr<int8_t (*)()>();
        intResult = FP();
      }
      result = intResult;
    } else if (bitWidth == 16) {
      int16_t intResult;
      if (mainHasArgs) {
        int16_t (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<int16_t (*)(int, char**)>();
        intResult = FP(argc, argv);
      } else {
        int16_t (*FP)() = ExprSymbol.getAddress().toPtr<int16_t (*)()>();
        intResult = FP();
      }
      result = intResult;
    } else if (bitWidth <= 32) {
      int32_t intResult;
      if (mainHasArgs) {
        int32_t (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<int32_t (*)(int, char**)>();
        intResult = FP(argc, argv);
      } else {
        int32_t (*FP)() = ExprSymbol.getAddress().toPtr<int32_t (*)()>();
        intResult = FP();
      }
      result = intResult;
    } else {
      int64_t intResult;
      if (mainHasArgs) {
        int64_t (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<int64_t (*)(int, char**)>();
        intResult = FP(argc, argv);
      } else {
        int64_t (*FP)() = ExprSymbol.getAddress().toPtr<int64_t (*)()>();
        intResult = FP();
      }
      result = intResult;
    }
  } else if (returnType->isFloatTy()) {
    float floatResult;
    if (mainHasArgs) {
      float (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<float (*)(int, char**)>();
      floatResult = FP(argc, argv);
    } else {
      float (*FP)() = ExprSymbol.getAddress().toPtr<float (*)()>();
      floatResult = FP();
    }
    result = floatResult;
  } else if (returnType->isDoubleTy()) {
    double doubleResult;
    if (mainHasArgs) {
      double (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<double (*)(int, char**)>();
      doubleResult = FP(argc, argv);
    } else {
      double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
      doubleResult = FP();
    }
    result = doubleResult;
  } else if (returnType->isPointerTy()) {
    // Assume string (i8*) for pointer types
    const char* strResult;
    if (mainHasArgs) {
      const char* (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<const char* (*)(int, char**)>();
      strResult = FP(argc, argv);
    } else {
      const char* (*FP)() = ExprSymbol.getAddress().toPtr<const char* (*)()>();
      strResult = FP();
    }
    result = std::string(strResult ? strResult : "");
  } else if (returnType->isStructTy()) {
    // static_ptr<u8> is struct { ptr, i64 } - extract the pointer as a string
    auto* structType = llvm::cast<llvm::StructType>(returnType);
    if (structType->getNumElements() == 2 &&
        structType->getElementType(0)->isPointerTy() &&
        structType->getElementType(1)->isIntegerTy(64)) {
      /** The pointer-and-length result returned by a JIT-compiled entrypoint.
       */
      struct StaticPtr {
        const char* data;
        int64_t len;
      };
      StaticPtr spResult;
      if (mainHasArgs) {
        StaticPtr (*FP)(int, char**) =
            ExprSymbol.getAddress().toPtr<StaticPtr (*)(int, char**)>();
        spResult = FP(argc, argv);
      } else {
        StaticPtr (*FP)() = ExprSymbol.getAddress().toPtr<StaticPtr (*)()>();
        spResult = FP();
      }
      result =
          std::string(spResult.data ? spResult.data : "",
                      spResult.data ? static_cast<size_t>(spResult.len) : 0);
    }
  } else {
    // Unknown type - default to double for backward compatibility
    double doubleResult;
    if (mainHasArgs) {
      double (*FP)(int, char**) =
          ExprSymbol.getAddress().toPtr<double (*)(int, char**)>();
      doubleResult = FP(argc, argv);
    } else {
      double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
      doubleResult = FP();
    }
    result = doubleResult;
  }

  // Remove the anonymous module
  ExitOnErr(RT->remove());

  return result;
}

}  // namespace sun::driver
