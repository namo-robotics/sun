#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>

#include "driver/driver.h"
#include "driver/execution_utils.h"

TEST(Tooling_Backend_Optimization, buffer_accessor_inlines_after_moon_linking) {
  initTestEnvironment();
  for (bool optimize : {false, true}) {
    auto driver = Driver::createForAOT("buffer_inline", "", false, optimize);
    driver->setMoonImports(getStdlibMoonImports());
    driver->compileString(R"(
      using std;
      function read_byte(buf: const ref ContiguousBuffer<u8>, index: i64) u8 {
        return unsafe { buf.get_unchecked(index); };
      }
      function main() i32 { return 0; }
    )");
    llvm::Function* reader = nullptr;
    for (auto& function : driver->getModule()) {
      if (function.getName().starts_with("read_byte$") && !function.isDeclaration()) {
        reader = &function;
      }
    }
    ASSERT_NE(reader, nullptr);
    unsigned calls = 0;
    unsigned byteLoads = 0;
    for (auto& block : *reader) {
      for (auto& instruction : block) {
        if (llvm::isa<llvm::CallBase>(instruction)) ++calls;
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
          if (load->getType()->isIntegerTy(8)) ++byteLoads;
        }
      }
    }
    if (optimize) {
      EXPECT_EQ(calls, 0u);
      EXPECT_EQ(byteLoads, 1u);
    } else {
      EXPECT_GT(calls, 0u);
    }
  }
}
