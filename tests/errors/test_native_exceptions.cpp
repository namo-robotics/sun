/** Verifies native and JIT propagation without the C++ exception runtime. */
#include <gtest/gtest.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>

#include <atomic>
#include <thread>
#include <vector>

#include "driver/sun_jit.h"
#include "runtime/exceptions.h"

extern "C" {
/** Returns the sum of payloads preserved through nested native cleanup. */
int sun_fixture_nested();
/** Returns A's payload after catching its parent, handling C, and rethrowing A. */
int sun_fixture_rethrow();
/** Attempts to escape the boundary protecting stack-based exception storage. */
void sun_fixture_escape();
/** Propagates through recursive frames and skips an unrelated catch handler. */
int sun_fixture_deep(int depth);
}

/** Keeps JIT setup helpers local to these transport tests. */
namespace {
/** Materializes fixtures before entering the runtime under test. */
std::unique_ptr<sun::driver::SunJIT> fixtureJIT(bool optimize) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  auto jit = llvm::cantFail(sun::driver::SunJIT::Create(optimize));
  llvm::orc::SymbolMap symbols;
  /** Exposes exactly one host transport entrypoint to generated code. */
  auto bind = [&](const char* name, auto* address) {
    symbols[jit->mangle(name)] = llvm::orc::ExecutorSymbolDef(
        llvm::orc::ExecutorAddr::fromPtr(address), llvm::JITSymbolFlags::Exported);
  };
  bind("sun_eh_raise", &sun_eh_raise);
  bind("sun_eh_take", &sun_eh_take);
  bind("sun_eh_push", &sun_eh_push);
  bind("sun_eh_pop", &sun_eh_pop);
  bind("sun_eh_terminate", &sun_eh_terminate);
  bind("sun_eh_personality", &sun_eh_personality);
  llvm::cantFail(jit->getMainJITDylib().define(
      llvm::orc::absoluteSymbols(std::move(symbols))));
  auto context = std::make_unique<llvm::LLVMContext>();
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(SUN_EXCEPTION_FIXTURE, diagnostic, *context);
  if (!module) {
    diagnostic.print("exception fixture", llvm::errs());
    return nullptr;
  }
  module->setDataLayout(jit->getDataLayout());
  module->setTargetTriple(jit->getTargetTriple().str());
  llvm::cantFail(jit->addModule(llvm::orc::ThreadSafeModule(
      std::move(module), std::move(context))));
  return jit;
}

/** Runs the same immutable code with independent exception state on each thread. */
int concurrentFailures(int (*run)()) {
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    /** Keeps all exceptions on this worker's storage and stack. */
    threads.emplace_back([&] {
      sun_eh_initialize();
      for (int j = 0; j < 32; ++j)
        if (run() != 60) ++failures;
    });
  }
  for (auto& thread : threads) thread.join();
  return failures.load();
}

/** Exercises the same runtime behavior with both JIT backend optimization modes. */
class Errors_JITExceptions : public testing::TestWithParam<bool> {
 protected:
  std::unique_ptr<sun::driver::SunJIT> jit;

  /** Materializes a fresh code lifetime before each independent scenario. */
  void SetUp() override {
    jit = fixtureJIT(GetParam());
    ASSERT_NE(jit, nullptr);
    sun_eh_initialize();
  }

  /** Resolves a generated fixture using its exact C function-pointer type. */
  template <typename Function>
  Function lookup(const char* name) {
    return llvm::cantFail(jit->lookup(name)).getAddress().toPtr<Function>();
  }
};
}  // namespace

/** A and B retain their native headers while their respective cleanups handle C. */
TEST(Errors_NativeExceptions, nested_cleanup_preserves_all_payloads) {
  sun_eh_initialize();
  EXPECT_EQ(sun_fixture_nested(), 60);
  EXPECT_EQ(sun_fixture_nested(), 60);
}

/** Catching a parent never slices the concrete identity used by a later rethrow. */
TEST(Errors_NativeExceptions, rethrow_after_handled_error) {
  sun_eh_initialize();
  EXPECT_EQ(sun_fixture_rethrow(), 10);
}

/** Deep stack traversal must skip a catch clause for a different concrete type. */
TEST(Errors_NativeExceptions, deep_propagation_skips_unrelated_handler) {
  sun_eh_initialize();
  EXPECT_EQ(sun_fixture_deep(128), 10);
}

/** Equivalent descriptors from separately compiled modules match by identity. */
TEST(Errors_NativeExceptions, descriptor_identity_is_not_pointer_identity) {
  using sun::runtime::ExceptionTypeDescriptor;
  const ExceptionTypeDescriptor parent{"library:Parent:layout1", nullptr, 4, 4};
  const ExceptionTypeDescriptor imported{"library:Parent:layout1", nullptr, 4, 4};
  const ExceptionTypeDescriptor other{"library:Parent:layout2", nullptr, 8, 8};
  const ExceptionTypeDescriptor child{"library:Child:layout1", &parent, 8, 4};
  EXPECT_TRUE(sun_eh_matches(&child, &imported));
  EXPECT_FALSE(sun_eh_matches(&child, &other));
}

/** An unhandled cleanup error terminates before its record's frame disappears. */
TEST(Errors_NativeExceptions, escaping_cleanup_terminates) {
  EXPECT_DEATH({ sun_eh_initialize(); sun_fixture_escape(); },
               "Sun exception escaped");
}

/** Each thread retains separate base storage throughout nested unwinding. */
TEST(Errors_NativeExceptions, simultaneous_threads) {
  EXPECT_EQ(concurrentFailures(&sun_fixture_nested), 0);
}

/** JIT cleanup preserves both the suspended payloads and their native headers. */
TEST_P(Errors_JITExceptions, nested_cleanup_preserves_all_payloads) {
  EXPECT_EQ(lookup<int (*)()>("sun_fixture_nested")(), 60);
}

/** A parent-caught JIT exception retains its concrete type when rethrown. */
TEST_P(Errors_JITExceptions, rethrow_after_handled_error) {
  EXPECT_EQ(lookup<int (*)()>("sun_fixture_rethrow")(), 10);
}

/** Registered JIT frame tables support deep propagation past unrelated handlers. */
TEST_P(Errors_JITExceptions, deep_propagation_skips_unrelated_handler) {
  EXPECT_EQ(lookup<int (*)(int)>("sun_fixture_deep")(128), 10);
}

/** Threads share registered JIT tables without sharing active exception records. */
TEST_P(Errors_JITExceptions, simultaneous_threads) {
  EXPECT_EQ(concurrentFailures(lookup<int (*)()>("sun_fixture_nested")), 0);
}

INSTANTIATE_TEST_SUITE_P(BackendOptimization, Errors_JITExceptions,
                         testing::Bool());
