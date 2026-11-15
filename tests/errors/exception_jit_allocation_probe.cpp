/** Enforces allocation-free JIT transport after eager unwind registration. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "exception_allocation_support.h"
#include "exception_jit_fixture.h"
#include "runtime/exceptions.h"

/** Keeps JIT fixture ownership local to this probe. */
namespace {
/** Retains generated entrypoints until their owning resource tracker is
 * removed. */
struct LoadedFixture {
  llvm::orc::ResourceTrackerSP tracker;
  sun::tests::exceptions::TransportFixture calls;
};

/** Resolves the two transport scenarios before allocations are denied. */
LoadedFixture loadFixture(sun::driver::SunJIT& jit, const std::string& prefix) {
  auto tracker = sun::tests::exceptions::loadExceptionFixture(jit, prefix);
  auto nested = llvm::cantFail(jit.lookup(prefix + "sun_fixture_nested"))
                    .getAddress()
                    .toPtr<int (*)()>();
  auto rethrow = llvm::cantFail(jit.lookup(prefix + "sun_fixture_rethrow"))
                     .getAddress()
                     .toPtr<int (*)()>();
  return {std::move(tracker), {nested, rethrow}};
}
}  // namespace

/** Requires zero allocations on first use, additional modules, and reused
 * addresses. */
int main(int argc, char** argv) {
  using sun::tests::exceptions::checkConcurrentTransport;
  using sun::tests::exceptions::checkTransport;
  if (argc != 2 ||
      (std::strcmp(argv[1], "--jit") && std::strcmp(argv[1], "--jit-reload") &&
       std::strcmp(argv[1], "--jit-threads"))) {
    std::fputs(
        "usage: sun_exception_allocation_probe "
        "[--capture|--jit|--jit-reload|--jit-threads]\n",
        stderr);
    return 64;
  }
  auto jit = sun::tests::exceptions::createExceptionJIT();
  const bool reload = std::strcmp(argv[1], "--jit-reload") == 0;
  const bool concurrent = std::strcmp(argv[1], "--jit-threads") == 0;
  for (int iteration = 0; iteration < (reload ? 3 : 1); ++iteration) {
    if (reload) {
      std::fprintf(stderr, "Reload round %d: first module\n", iteration);
    }
    auto first = loadFixture(*jit, "first_");
    const int firstResult = concurrent ? checkConcurrentTransport(first.calls)
                                       : checkTransport(first.calls);
    if (firstResult) {
      return firstResult;
    }
    if (reload) {
      std::fputs("Registering second module\n", stderr);
      auto second = loadFixture(*jit, "second_");
      std::fputs("Executing second module\n", stderr);
      if (int result = checkTransport(second.calls)) {
        return result;
      }
      std::fputs("Removing first module\n", stderr);
      llvm::cantFail(first.tracker->remove());
      std::fputs("Executing surviving module\n", stderr);
      if (int result = checkTransport(second.calls)) {
        return result;
      }
      std::fputs("Removing second module\n", stderr);
      llvm::cantFail(second.tracker->remove());
    } else {
      llvm::cantFail(first.tracker->remove());
    }
  }
  return 0;
}
