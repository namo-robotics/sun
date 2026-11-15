/** Provides JIT setup shared by runtime assertions and allocation probes. */
#pragma once
#include "driver/sun_jit.h"

/** Owns and loads the LLVM fixtures used to exercise exception transport. */
namespace sun::tests::exceptions {
/** Binds the runtime entrypoints without executing or warming fixture code. */
std::unique_ptr<sun::driver::SunJIT> createExceptionJIT(bool optimize = true);
/** Loads a separately named fixture; removing the tracker releases its code and
 * unwind tables. */
llvm::orc::ResourceTrackerSP loadExceptionFixture(
    sun::driver::SunJIT& jit, const std::string& prefix = "");
}  // namespace sun::tests::exceptions
