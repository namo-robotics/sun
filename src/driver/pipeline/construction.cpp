/** Constructs compiler drivers and manages JIT exit handlers. */
#include <cstdlib>
#include <mutex>
#include <vector>

#include "abi/abi_compatibility_checker.h"
#include "driver/driver.h"
#include "jit_runtime.h"
#include "moon_bundling/library_cache.h"

using sun::codegen::CodegenVisitor;
using sun::moon_bundling::LibraryCache;
using sun::semantic_analysis::AnalysisResults;
using sun::semantic_analysis::SemanticAnalyzer;

/** Constructs drivers and manages the lifetime of JIT exit handlers. */
namespace sun::driver {

/**
 * ---------------------------------------------------------------------------
 * Exit handlers registered by JIT-compiled code
 *
 * A program run under the JIT lives inside the compiler process. C code it
 * brings along (an archive carried by a bundle) may call atexit, as OpenSSL
 * does for its cleanup. The real atexit would run that handler when the
 * compiler exits, long after the JIT's code memory is gone, and crash. So the
 * JIT resolves atexit to a shim that keeps the handlers here, and they run
 * when the program's main returns (its exit, as far as it can tell) while
 * its code is still mapped. A program that calls exit() itself ends the
 * whole process from inside the JIT'd code; a real exit handler drains the
 * same list then, with the memory still there.
 * ---------------------------------------------------------------------------
 */
namespace {

/** Provides the mutex protecting registered JIT exit handlers. */
std::mutex& jitExitMutex() {
  static std::mutex mutex;
  return mutex;
}

/** Provides the exit handlers registered by JIT-compiled code. */
std::vector<void (*)()>& jitExitHandlers() {
  static std::vector<void (*)()> handlers;
  return handlers;
}

/**
 * Reverse registration order, like atexit; a handler may register another
 */
void runJITExitHandlers() {
  for (;;) {
    void (*handler)() = nullptr;
    {
      std::lock_guard<std::mutex> lock(jitExitMutex());
      if (jitExitHandlers().empty()) {
        return;
      }
      handler = jitExitHandlers().back();
      jitExitHandlers().pop_back();
    }
    handler();
  }
}

/**
 * Stands in for atexit and at_quick_exit inside the JIT
 */
int jitAtExit(void (*handler)()) {
  {
    std::lock_guard<std::mutex> lock(jitExitMutex());
    jitExitHandlers().push_back(handler);
  }
  // Registered after the list exists, so at process exit it runs before the
  // list is destroyed
  static std::once_flag drainAtProcessExit;
  std::call_once(drainAtProcessExit, [] { std::atexit(runJITExitHandlers); });
  return 0;
}

/**
 * Fork handlers registered by JIT'd code would outlive it the same way and
 * fire on the compiler's own later forks. A program run this way does not
 * fork, so they are accepted and dropped.
 */
int jitAtFork(void (*)(), void (*)(), void (*)()) { return 0; }

}  // namespace

JITExitScope::~JITExitScope() { runJITExitHandlers(); }

// Factory method for JIT execution
std::unique_ptr<Driver> Driver::createForJIT(const std::string& moduleName,
                                             bool debugInfo, bool optimize) {
  ensureLLVMInitialized();

  // JIT always runs on the host; .moon bundle selection must match.
  LibraryCache::instance().setTargetTriple("", false);
  abi::AbiCompatibilityChecker::prepareCompilation(
      "", abi::NativeExecution::JIT, debugInfo, optimize);

  auto jit = SunJIT::Create(optimize);
  if (!jit) {
    sun::support::messageStream("error")
        << "Failed to create SunJIT: " << toString(jit.takeError()) << "\n";
    std::abort();
  }

  auto jitShared = std::shared_ptr<SunJIT>(std::move(jit.get()));
  auto ctx = std::make_unique<sun::codegen::CodegenContext>(
      moduleName, jitShared,
      /*existingContext=*/nullptr,
      /*targetTriple=*/"", debugInfo, optimize);

  // Register runtime symbols for JIT
  auto& mainDylib = ctx->jit->getMainJITDylib();
  llvm::orc::SymbolMap runtimeSymbols;
  auto provide = [&](const char* name, void* address) {
    runtimeSymbols[ctx->jit->mangle(name)] = ExecutorSymbolDef(
        ExecutorAddr::fromPtr(address), JITSymbolFlags::Exported);
  };
  provide("putchard", reinterpret_cast<void*>(&sun::parsing::putchard));
  // Process-lifetime registrations must not reach the real libc from code
  // that lives only as long as this driver; see the JIT exit handlers above.
  // (On glibc these also live in libc_nonshared.a, out of dlsym's sight.)
  provide("atexit", reinterpret_cast<void*>(&jitAtExit));
  provide("at_quick_exit", reinterpret_cast<void*>(&jitAtExit));
  provide("pthread_atfork", reinterpret_cast<void*>(&jitAtFork));
  cantFail(mainDylib.define(llvm::orc::absoluteSymbols(runtimeSymbols)));

  auto analysisResults = std::make_shared<AnalysisResults>();
  auto codegenVisitor = std::make_unique<CodegenVisitor>(*ctx, analysisResults);
  auto analyzer = std::make_unique<SemanticAnalyzer>(analysisResults);

  return std::unique_ptr<Driver>(new Driver(std::move(ctx), analysisResults,
                                            std::move(codegenVisitor),
                                            std::move(analyzer)));
}

// Factory method for AOT compilation
std::unique_ptr<Driver> Driver::createForAOT(const std::string& moduleName,
                                             const std::string& targetTriple,
                                             bool debugInfo, bool optimize) {
  ensureLLVMInitialized();

  // Both the parser's bundle resolution and the linker's bundle selection
  // key off this; setting it here keeps API users consistent with the CLI.
  LibraryCache::instance().setTargetTriple(targetTriple, false);
  abi::AbiCompatibilityChecker::prepareCompilation(
      targetTriple, abi::NativeExecution::NativeLink, debugInfo, optimize);

  auto ctx = std::make_unique<sun::codegen::CodegenContext>(
      moduleName, nullptr,
      /*existingContext=*/nullptr, targetTriple, debugInfo, optimize);
  auto analysisResults = std::make_shared<AnalysisResults>();
  auto codegenVisitor = std::make_unique<CodegenVisitor>(*ctx, analysisResults);
  auto analyzer = std::make_unique<SemanticAnalyzer>(analysisResults);

  return std::unique_ptr<Driver>(new Driver(std::move(ctx), analysisResults,
                                            std::move(codegenVisitor),
                                            std::move(analyzer)));
}

}  // namespace sun::driver
