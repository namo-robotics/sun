/** Intercepts the platform allocator while a runtime scenario is under audit.
 */
#include "allocation_audit.h"

#include <dlfcn.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

/** Stores observations without allocating memory inside allocator callbacks. */
namespace {
std::atomic<bool> auditAllocations{false};
std::atomic<unsigned> allocationAttempts{0};
void* firstAllocationCaller = nullptr;

/** Counts an allocation attempt and records the first caller for later
 * diagnostics. */
bool rejected(void* caller) noexcept {
  if (!auditAllocations.load(std::memory_order_relaxed)) {
    return false;
  }
  if (allocationAttempts.fetch_add(1, std::memory_order_relaxed) == 0) {
    firstAllocationCaller = caller;
  }
  return true;
}
}  // namespace

#if defined(__GLIBC__)
extern "C" {
/** Calls glibc directly so instrumentation also sees shared unwinder
 * allocations. */
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
  return rejected(__builtin_return_address(0)) ? nullptr
                                               : __libc_calloc(count, size);
}

/** Rejects resizing during exception transport. */
void* realloc(void* pointer, std::size_t size) noexcept {
  return rejected(__builtin_return_address(0)) ? nullptr
                                               : __libc_realloc(pointer, size);
}

/** Rejects aligned allocation during exception transport. */
void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
  return rejected(__builtin_return_address(0))
             ? nullptr
             : __libc_memalign(alignment, size);
}

/** Rejects POSIX aligned allocation during exception transport. */
int posix_memalign(void** pointer, std::size_t alignment,
                   std::size_t size) noexcept {
  if (rejected(__builtin_return_address(0))) {
    return ENOMEM;
  }
  if (alignment < sizeof(void*) || (alignment & (alignment - 1))) {
    return EINVAL;
  }
  auto* result = __libc_memalign(alignment, size);
  if (!result) {
    return ENOMEM;
  }
  *pointer = result;
  return 0;
}
}
#elif defined(__APPLE__)
#include <malloc/malloc.h>

/** Rejects malloc through dyld interposition, retaining the original
 * implementation. */
static void* probeMalloc(std::size_t size) {
  return rejected(__builtin_return_address(0)) ? nullptr : malloc(size);
}

/** Rejects calloc through dyld interposition. */
static void* probeCalloc(std::size_t count, std::size_t size) {
  return rejected(__builtin_return_address(0)) ? nullptr : calloc(count, size);
}

/** Rejects realloc through dyld interposition. */
static void* probeRealloc(void* pointer, std::size_t size) {
  return rejected(__builtin_return_address(0)) ? nullptr
                                               : realloc(pointer, size);
}

/** Rejects aligned allocation through dyld interposition. */
static void* probeAlignedAlloc(std::size_t alignment, std::size_t size) {
  return rejected(__builtin_return_address(0)) ? nullptr
                                               : aligned_alloc(alignment, size);
}

/** Rejects POSIX aligned allocation through dyld interposition. */
static int probePosixMemalign(void** pointer, std::size_t alignment,
                              std::size_t size) {
  return rejected(__builtin_return_address(0))
             ? ENOMEM
             : posix_memalign(pointer, alignment, size);
}

/** Associates allocator replacements with the system entrypoints they
 * intercept. */
struct Interposition {
  const void* replacement;
  const void* original;
};

__attribute__((used, section("__DATA,__interpose"))) static const Interposition
    interpositions[] = {{reinterpret_cast<const void*>(&probeMalloc),
                         reinterpret_cast<const void*>(&malloc)},
                        {reinterpret_cast<const void*>(&probeCalloc),
                         reinterpret_cast<const void*>(&calloc)},
                        {reinterpret_cast<const void*>(&probeRealloc),
                         reinterpret_cast<const void*>(&realloc)},
                        {reinterpret_cast<const void*>(&probeAlignedAlloc),
                         reinterpret_cast<const void*>(&aligned_alloc)},
                        {reinterpret_cast<const void*>(&probePosixMemalign),
                         reinterpret_cast<const void*>(&posix_memalign)}};
#else
#error Allocation instrumentation requires glibc or Apple dyld interposition.
#endif

sun::tests::exceptions::AllocationAudit::AllocationAudit(
    const char* scenario) noexcept
    : scenario_(scenario) {
  allocationAttempts.store(0);
  firstAllocationCaller = nullptr;
  auditAllocations.store(true);
}

sun::tests::exceptions::AllocationAudit::~AllocationAudit() {
  auditAllocations.store(false);
}

bool sun::tests::exceptions::AllocationAudit::finish() noexcept {
  auditAllocations.store(false);
  const auto attempts = allocationAttempts.load();
  if (attempts == 0) {
    return true;
  }

  // Symbol lookup and diagnostics happen only after allocation is allowed
  // again.
  std::fprintf(stderr, "%s attempted heap allocation (%u attempts)\n",
               scenario_, attempts);
  Dl_info info{};
  if (dladdr(firstAllocationCaller, &info)) {
    const auto offset =
        reinterpret_cast<std::uintptr_t>(firstAllocationCaller) -
        reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    std::fprintf(stderr, "first caller: %s + 0x%zx (%s)\n", info.dli_fname,
                 offset, info.dli_sname ? info.dli_sname : "private symbol");
  }
  return false;
}
