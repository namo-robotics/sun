/** Runs small source programs through the ordinary compiler and JIT. */
#pragma once
#include <gtest/gtest.h>

#include "driver/driver.h"
#include "support/error.h"

/** Runs each source scenario with both backend optimization settings. */
class Errors_SourceExceptions : public testing::TestWithParam<bool> {
 protected:
  /** Compiles and executes one source program using the real driver. */
  sun::driver::SunValue run(const std::string& source) {
    auto driver =
        sun::driver::Driver::createForJIT("exceptions", false, GetParam());
    return driver->executeString(
        R"(/** Borrows an array through the ordinary C ABI for runtime reporting. */
extern "C" function print_trace(frames: const ref array<u64>, status: u32) void as "sun_eh_print_trace";
)" + source);
  }
};
