/** Materializes runtime fixtures before allocation-denial tests begin. */
#include "exception_jit_fixture.h"

#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>

#include "runtime/exceptions.h"

/** Shares JIT setup without coupling the standalone probe to a test framework.
 */
namespace sun::tests::exceptions {
/** Creates a JIT and binds transport services without executing generated code.
 */
std::unique_ptr<sun::driver::SunJIT> createExceptionJIT(bool optimize) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  auto jit = llvm::cantFail(sun::driver::SunJIT::Create(optimize));
  llvm::orc::SymbolMap symbols;
  /** Makes a transport service visible to generated fixture code. */
  auto bind = [&](const char* name, auto* address) {
    symbols[jit->mangle(name)] =
        llvm::orc::ExecutorSymbolDef(llvm::orc::ExecutorAddr::fromPtr(address),
                                     llvm::JITSymbolFlags::Exported);
  };
  bind("sun_eh_raise", &sun_eh_raise);
  bind("sun_eh_throw", &sun_eh_throw);
  bind("sun_eh_report", &sun_eh_report);
  bind("sun_eh_terminate_exception", &sun_eh_terminate_exception);
  bind("sun_eh_take", &sun_eh_take);
  bind("sun_eh_push", &sun_eh_push);
  bind("sun_eh_pop", &sun_eh_pop);
  bind("sun_eh_terminate", &sun_eh_terminate);
  bind("sun_eh_personality", &sun_eh_personality);
  llvm::cantFail(jit->getMainJITDylib().define(
      llvm::orc::absoluteSymbols(std::move(symbols))));
  return jit;
}

/** Materializes a separately named module and eagerly indexes all of its
 * frames. */
llvm::orc::ResourceTrackerSP loadExceptionFixture(sun::driver::SunJIT& jit,
                                                  const std::string& prefix) {
  auto context = std::make_unique<llvm::LLVMContext>();
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(SUN_EXCEPTION_FIXTURE, diagnostic, *context);
  if (!module) {
    diagnostic.print("allocation fixture", llvm::errs());
    std::abort();
  }
  for (auto& function : *module) {
    if (!function.isDeclaration()) {
      function.setName(prefix + function.getName());
    }
  }
  module->setDataLayout(jit.getDataLayout());
  module->setTargetTriple(jit.getTargetTriple().str());
  auto tracker = jit.getMainJITDylib().createResourceTracker();
  llvm::cantFail(jit.addModule(
      llvm::orc::ThreadSafeModule(std::move(module), std::move(context)),
      tracker));
  return tracker;
}
}  // namespace sun::tests::exceptions
