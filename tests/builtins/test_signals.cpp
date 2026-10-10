/** Check the native signal boundary and complete disposition restoration. */
#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>
#include <signal.h>

#include <chrono>
#include <thread>

#include "driver/execution_utils.h"

/** Keep native signal fixtures private to this translation unit. */
namespace {
volatile sig_atomic_t observedSignal = 0;

/** Record delivery without allocation, locks, or non-signal-safe library calls.
 */
void previousHandler(int signal, siginfo_t*, void*) { observedSignal = signal; }

/** Restore the test process's original dispositions even if a test fails. */
class SavedSignals {
 public:
  struct sigaction interrupt{};
  struct sigaction terminate{};

  /** Capture both dispositions before a test replaces them. */
  SavedSignals() {
    EXPECT_EQ(sigaction(SIGINT, nullptr, &interrupt), 0);
    EXPECT_EQ(sigaction(SIGTERM, nullptr, &terminate), 0);
  }

  /** Put back the original actions after the JIT program has finished. */
  ~SavedSignals() {
    sigaction(SIGINT, &interrupt, nullptr);
    sigaction(SIGTERM, &terminate, nullptr);
  }
};
}  // namespace

/** Scope exit restores the native callback, mask, and flags on both platforms.
 */
TEST(Builtins_Signals, restores_complete_native_dispositions) {
  SavedSignals saved;
  struct sigaction action{};
  action.sa_sigaction = previousHandler;
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGUSR1);
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  ASSERT_EQ(sigaction(SIGINT, &action, nullptr), 0);
  ASSERT_EQ(sigaction(SIGTERM, &action, nullptr), 0);
  struct sigaction before{};
  ASSERT_EQ(sigaction(SIGINT, nullptr, &before), 0);
  observedSignal = 0;
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    using std;
    /** Deliver synchronously to the calling thread. */
    extern "C" function native_raise(sig: i32) i32 as "raise";
    /** Exercise automatic restoration from a compiled-library import. */
    function main() i32 {
      match std.signal.SignalSubscription() {
        SystemResult.Ok(subscription) => {
          unsafe { native_raise(std.process.SIGINT); native_raise(std.process.SIGTERM); };
          match subscription.take_pending() {
            SystemResult.Ok(events) => {
              if (not events.interrupted() or not events.terminated()) { return 2; }
            },
            SystemResult.Error(error) => { return 3; }
          };
        },
        SystemResult.Error(error) => { return 1; }
      };
      return 0;
    }
  )"),
            0);
  EXPECT_EQ(observedSignal, 0);
  for (int signal : {SIGINT, SIGTERM}) {
    struct sigaction after{};
    ASSERT_EQ(sigaction(signal, nullptr, &after), 0);
    EXPECT_EQ(after.sa_sigaction, before.sa_sigaction);
    EXPECT_EQ(after.sa_flags, before.sa_flags);
    for (int member = 1; member < NSIG; ++member) {
      EXPECT_EQ(sigismember(&after.sa_mask, member),
                sigismember(&before.sa_mask, member));
    }
    ASSERT_EQ(raise(signal), 0);
    EXPECT_EQ(observedSignal, signal);
  }
}

/** Concurrent delivery must never write into a closed or reused descriptor. */
TEST(Builtins_Signals, concurrent_delivery_during_restoration) {
  SavedSignals saved;
  struct sigaction ignored{};
  ignored.sa_handler = SIG_IGN;
  sigemptyset(&ignored.sa_mask);
  ASSERT_EQ(sigaction(SIGINT, &ignored, nullptr), 0);
  ASSERT_EQ(sigaction(SIGTERM, &ignored, nullptr), 0);
  sun::driver::initTestEnvironment();
  auto driver = sun::driver::Driver::createForJIT();
  driver->setMoonImports(sun::driver::getStdlibMoonImports());
  // A worker-targeted signal lets installation and restoration run
  // concurrently.
  /** Keep delivering until the scoped worker is joined. */
  std::jthread sender([](std::stop_token stop) {
    while (!stop.stop_requested()) {
      raise(SIGINT);
      raise(SIGTERM);
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
  });
  EXPECT_EQ(driver->executeString(R"(
    using std;
    /** Repeatedly retire and reuse the subscription's pipe descriptors. */
    function main() i32 {
      for (var i: i32 = 0; i < 100; i = i + 1) {
        match std.signal.SignalSubscription() {
          SystemResult.Ok(subscription) => {
            std.time.sleep(std.time.create_duration_millis(1));
            match subscription.take_pending() {
              SystemResult.Ok(events) => {},
              SystemResult.Error(error) => { return 2; }
            };
            match subscription.restore() {
              SystemResult.Ok => {},
              SystemResult.Error(error) => { return 3; }
            };
          },
          SystemResult.Error(error) => { return 1; }
        };
      }
      return 0;
    }
  )"),
            0);
  sender.request_stop();
  sender.join();
}

/** The handler has only the approved libc calls and explicit lock-free atomics.
 */
TEST(Builtins_Signals, handler_contains_only_signal_safe_operations) {
  for (const auto* target : {"x86_64-linux-gnu", "aarch64-linux-gnu",
                             "arm64-apple-darwin", "x86_64-apple-darwin"}) {
    auto driver = sun::driver::Driver::createForAOT("signal_ir", target);
    driver->compileString(R"(
      /** Keep both native addresses observable through optimization. */
      extern "C" function capture(handler: raw_ptr<u8>, state: raw_ptr<u8>) void;
      /** Force emission of the native handler without installing it. */
      function main() i32 {
        unsafe { capture(_signal_handler(), _signal_state()); };
        return 0;
      }
    )");
    auto* handler = driver->getModule().getFunction("__sun_signal_handler");
    ASSERT_NE(handler, nullptr) << target;
    int atomicCount = 0;
    int callCount = 0;
    for (const auto& block : *handler) {
      for (const auto& instruction : block) {
        if (const auto* atomic =
                llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction)) {
          ++atomicCount;
          EXPECT_TRUE(atomic->getValOperand()->getType()->isIntegerTy(32));
          EXPECT_EQ(atomic->getAlign(), llvm::Align(4));
        }
        if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
          ++callCount;
          ASSERT_NE(call->getCalledFunction(), nullptr);
          auto name = call->getCalledFunction()->getName();
          EXPECT_TRUE(name == "write" || name == "__errno_location" ||
                      name == "__error")
              << name.str();
        }
      }
    }
    EXPECT_EQ(atomicCount, 3);
    EXPECT_EQ(callCount, 2);
  }
}

/** Applications cannot obtain a native callback or mutate its state in safe
 * code. */
TEST(Builtins_Signals, signal_intrinsics_require_unsafe) {
  for (const auto* intrinsic : {"_signal_handler", "_signal_state"}) {
    EXPECT_SUN_ERROR_WITH_MESSAGE(
        sun::driver::executeString(
            std::string("/** Attempt unguarded native access. */ function "
                        "main() i32 { var p = ") +
            intrinsic + "(); return 0; }"),
        "can only be used in an unsafe block");
  }
}
