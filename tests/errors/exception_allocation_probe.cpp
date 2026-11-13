/** Enforces allocation-free transport after AOT startup or eager JIT indexing. */
#include <atomic>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>

#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>

#include "driver/sun_jit.h"
#include "runtime/exceptions.h"

extern "C" {
/** Runs native propagation with nested, internally handled cleanup failures. */
int sun_fixture_nested();
/** Rethrows a concrete exception after handling an independent exception. */
int sun_fixture_rethrow();
}

/** Keeps allocation instrumentation independent of the test framework. */
namespace {
std::atomic<bool> rejectAllocations{false};
std::atomic<bool> auditAllocations{false};
std::atomic<unsigned> allocationAttempts{0};
void* firstAllocationCaller = nullptr;

/** Counts transport allocation attempts while keeping diagnostic storage static. */
bool rejected(void* caller) noexcept {
  if (!auditAllocations.load(std::memory_order_relaxed)) return false;
  if (!allocationAttempts.fetch_add(1, std::memory_order_relaxed))
    firstAllocationCaller = caller;
  return rejectAllocations.load(std::memory_order_relaxed);
}

/** Creates a JIT and binds transport services without executing generated code. */
std::unique_ptr<sun::driver::SunJIT> prepareJIT() {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  auto jit = llvm::cantFail(sun::driver::SunJIT::Create());
  llvm::orc::SymbolMap symbols;
  /** Makes a transport service visible to generated fixture code. */
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
  return jit;
}

/** Retains generated entrypoints until their owning resource tracker is removed. */
struct LoadedFixture {
  llvm::orc::ResourceTrackerSP tracker;
  int (*nested)();
  int (*rethrow)();
};

/** Materializes a separately named module and eagerly indexes all of its frames. */
LoadedFixture loadFixture(sun::driver::SunJIT& jit, const std::string& prefix) {
  auto context = std::make_unique<llvm::LLVMContext>();
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(SUN_EXCEPTION_FIXTURE, diagnostic, *context);
  if (!module) {
    diagnostic.print("allocation fixture", llvm::errs());
    std::abort();
  }
  for (auto& function : *module)
    if (!function.isDeclaration()) function.setName(prefix + function.getName());
  module->setDataLayout(jit.getDataLayout());
  module->setTargetTriple(jit.getTargetTriple().str());
  auto tracker = jit.getMainJITDylib().createResourceTracker();
  llvm::cantFail(jit.addModule(llvm::orc::ThreadSafeModule(
      std::move(module), std::move(context)), tracker));
  auto nested = llvm::cantFail(jit.lookup(prefix + "sun_fixture_nested"))
                    .getAddress().toPtr<int (*)()>();
  auto rethrow = llvm::cantFail(jit.lookup(prefix + "sun_fixture_rethrow"))
                     .getAddress().toPtr<int (*)()>();
  return {std::move(tracker), nested, rethrow};
}
}  // namespace

#if defined(__GLIBC__)
extern "C" {
/** Calls glibc directly so instrumentation also sees shared unwinder allocations. */
void* __libc_malloc(std::size_t) noexcept;
/** Calls the original zero-initialized allocation implementation. */
void* __libc_calloc(std::size_t, std::size_t) noexcept;
/** Calls the original resize implementation. */
void* __libc_realloc(void*, std::size_t) noexcept;
/** Calls the original aligned allocation implementation. */
void* __libc_memalign(std::size_t, std::size_t) noexcept;

/** Rejects heap allocation while exception transport is being exercised. */
void* malloc(std::size_t size) noexcept {
  return rejected(__builtin_return_address(0)) ? nullptr : __libc_malloc(size);
}
/** Rejects zero-initialized allocation during exception transport. */
void* calloc(std::size_t count, std::size_t size) noexcept {
  return rejected(__builtin_return_address(0)) ? nullptr : __libc_calloc(count, size);
}
/** Rejects resizing during exception transport. */
void* realloc(void* pointer, std::size_t size) noexcept {
  return rejected(__builtin_return_address(0)) ? nullptr : __libc_realloc(pointer, size);
}
/** Rejects aligned allocation during exception transport. */
void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
  return rejected(__builtin_return_address(0)) ? nullptr : __libc_memalign(alignment, size);
}
/** Rejects POSIX aligned allocation during exception transport. */
int posix_memalign(void** pointer, std::size_t alignment, std::size_t size) noexcept {
  if (rejected(__builtin_return_address(0))) return ENOMEM;
  if (alignment < sizeof(void*) || (alignment & (alignment - 1))) return EINVAL;
  auto* result = __libc_memalign(alignment, size);
  if (!result) return ENOMEM;
  *pointer = result;
  return 0;
}
}
#elif defined(__APPLE__)
#include <malloc/malloc.h>

/** Rejects malloc through dyld interposition, retaining the original implementation. */
static void* probeMalloc(std::size_t size) {
  return rejected(__builtin_return_address(0)) ? nullptr : malloc(size);
}
/** Rejects calloc through dyld interposition. */
static void* probeCalloc(std::size_t count, std::size_t size) {
  return rejected(__builtin_return_address(0)) ? nullptr : calloc(count, size);
}
/** Rejects realloc through dyld interposition. */
static void* probeRealloc(void* pointer, std::size_t size) {
  return rejected(__builtin_return_address(0)) ? nullptr : realloc(pointer, size);
}
/** Rejects aligned allocation through dyld interposition. */
static void* probeAlignedAlloc(std::size_t alignment, std::size_t size) {
  return rejected(__builtin_return_address(0)) ? nullptr : aligned_alloc(alignment, size);
}
/** Rejects POSIX aligned allocation through dyld interposition. */
static int probePosixMemalign(void** pointer, std::size_t alignment, std::size_t size) {
  return rejected(__builtin_return_address(0)) ? ENOMEM : posix_memalign(pointer, alignment, size);
}
/** Associates allocator replacements with the system entrypoints they intercept. */
struct Interposition { const void* replacement; const void* original; };
__attribute__((used, section("__DATA,__interpose")))
static const Interposition interpositions[] = {
    {reinterpret_cast<const void*>(&probeMalloc), reinterpret_cast<const void*>(&malloc)},
    {reinterpret_cast<const void*>(&probeCalloc), reinterpret_cast<const void*>(&calloc)},
    {reinterpret_cast<const void*>(&probeRealloc), reinterpret_cast<const void*>(&realloc)},
    {reinterpret_cast<const void*>(&probeAlignedAlloc), reinterpret_cast<const void*>(&aligned_alloc)},
    {reinterpret_cast<const void*>(&probePosixMemalign), reinterpret_cast<const void*>(&posix_memalign)}};
#else
#error Allocation instrumentation requires glibc or Apple dyld interposition.
#endif

/** Measures cold transport without including thread creation or thread teardown. */
static int checkFixture(const LoadedFixture& fixture, bool useJIT,
                        bool concurrent = false) {
  sun_eh_initialize();
  allocationAttempts.store(0);
  firstAllocationCaller = nullptr;
  std::atomic<unsigned> ready{0}, done{0}, failures{0};
  std::atomic<bool> start{false}, leave{false};
  std::array<std::thread, 4> workers;
  if (concurrent) {
    for (auto& worker : workers) {
      /** Waits until startup is excluded from the transport allocation audit. */
      worker = std::thread([&] {
        sun_eh_initialize();
        ++ready;
        while (!start.load()) std::this_thread::yield();
        if (fixture.nested() != 60 || fixture.rethrow() != 10) ++failures;
        ++done;
        while (!leave.load()) std::this_thread::yield();
      });
    }
    while (ready.load() != workers.size()) std::this_thread::yield();
  }
  rejectAllocations.store(true, std::memory_order_relaxed);
  auditAllocations.store(true, std::memory_order_relaxed);
  if (concurrent) {
    start.store(true);
    while (done.load() != workers.size()) std::this_thread::yield();
  } else if (fixture.nested() != 60 || fixture.rethrow() != 10) {
    ++failures;
  }
  auditAllocations.store(false, std::memory_order_relaxed);
  rejectAllocations.store(false, std::memory_order_relaxed);
  if (concurrent) {
    leave.store(true);
    for (auto& worker : workers) worker.join();
  }
  if (allocationAttempts.load(std::memory_order_relaxed)) {
    std::fputs(useJIT ? "JIT exception transport attempted heap allocation\n"
                      : "AOT exception transport attempted heap allocation\n",
               stderr);
    Dl_info info{};
    if (dladdr(firstAllocationCaller, &info)) {
      std::fprintf(stderr, "%u attempts; first caller: %s + 0x%zx (%s)\n",
                   allocationAttempts.load(), info.dli_fname,
                   reinterpret_cast<std::uintptr_t>(firstAllocationCaller) -
                       reinterpret_cast<std::uintptr_t>(info.dli_fbase),
                   info.dli_sname ? info.dli_sname : "private symbol");
    }
    return 1;
  }
  return failures.load() ? 2 : 0;
}

/** Requires zero allocations on first use, additional modules, and reused addresses. */
int main(int argc, char** argv) {
  if (argc == 1)
    return checkFixture({nullptr, sun_fixture_nested, sun_fixture_rethrow}, false);
  if (argc != 2 || (std::strcmp(argv[1], "--jit") &&
                    std::strcmp(argv[1], "--jit-reload") &&
                    std::strcmp(argv[1], "--jit-threads"))) {
    std::fputs("usage: sun_exception_allocation_probe [--jit|--jit-reload|--jit-threads]\n", stderr);
    return 64;
  }
  auto jit = prepareJIT();
  const bool reload = std::strcmp(argv[1], "--jit-reload") == 0;
  const bool concurrent = std::strcmp(argv[1], "--jit-threads") == 0;
  for (int iteration = 0; iteration < (reload ? 3 : 1); ++iteration) {
    if (reload) std::fprintf(stderr, "Reload round %d: first module\n", iteration);
    auto first = loadFixture(*jit, "first_");
    if (int result = checkFixture(first, true, concurrent)) return result;
    if (reload) {
      std::fputs("Registering second module\n", stderr);
      auto second = loadFixture(*jit, "second_");
      std::fputs("Executing second module\n", stderr);
      if (int result = checkFixture(second, true)) return result;
      std::fputs("Removing first module\n", stderr);
      llvm::cantFail(first.tracker->remove());
      std::fputs("Executing surviving module\n", stderr);
      if (int result = checkFixture(second, true)) return result;
      std::fputs("Removing second module\n", stderr);
      llvm::cantFail(second.tracker->remove());
    } else {
      llvm::cantFail(first.tracker->remove());
    }
  }
  return 0;
}
