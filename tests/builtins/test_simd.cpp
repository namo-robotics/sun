/** Checks vector lowering and unsafe boundaries for four-lane float intrinsics.
 */
#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

#include "driver/execution_utils.h"

/** Verifies actual vector arithmetic and conservative alignment in emitted IR.
 */
TEST(Builtins_SIMD, vector_ir_and_alignment) {
  sun::driver::initTestEnvironment();
  auto driver = sun::driver::Driver::createForAOT("simd_ir");
  driver->compileString(R"(
    /** Uses raw operations only inside the explicit unsafe boundary. */
    function compute(out: raw_ptr<f32>, a: raw_ptr<f32>, b: raw_ptr<f32>) void {
      unsafe {
        _simd_f32x4_splat(out, 2.0f32);
        _simd_f32x4_add(out, a, b);
        _simd_f32x4_sub(out, a, b);
        _simd_f32x4_mul(out, a, b);
        _simd_f32x4_copy(out, a);
      };
    }
  )");
  std::string ir;
  llvm::raw_string_ostream stream(ir);
  driver->getModule().print(stream, nullptr);
  EXPECT_NE(ir.find("fadd <4 x float>"), std::string::npos);
  EXPECT_NE(ir.find("fsub <4 x float>"), std::string::npos);
  EXPECT_NE(ir.find("fmul <4 x float>"), std::string::npos);
  EXPECT_NE(ir.find("load <4 x float>"), std::string::npos);
  EXPECT_NE(ir.find("store <4 x float>"), std::string::npos);
  for (const auto& function : driver->getModule()) {
    for (const auto& block : function) {
      for (const auto& instruction : block) {
        if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
          if (load->getType()->isVectorTy())
            EXPECT_EQ(load->getAlign().value(), 4);
        }
        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
          if (store->getValueOperand()->getType()->isVectorTy())
            EXPECT_EQ(store->getAlign().value(), 4);
        }
      }
    }
  }
  EXPECT_EQ(ir.find("fadd fast"), std::string::npos);
  EXPECT_EQ(ir.find("fmul contract"), std::string::npos);
}

/** Ensures every raw-memory SIMD operation requires an unsafe block. */
TEST(Builtins_SIMD, raw_operations_require_unsafe) {
  for (const std::string operation : {"copy", "splat", "add", "sub", "mul"}) {
    const std::string arguments = operation == "splat"  ? "p, 1.0f32"
                                  : operation == "copy" ? "p, p"
                                                        : "p, p, p";
    EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(R"(
      /** Attempts unchecked memory access outside unsafe. */
      function main() i32 {
        var lanes: array<f32, 4> = [1.0f32, 2.0f32, 3.0f32, 4.0f32];
        var p = _address_of<f32>(lanes[0]);
    )" + std::string("_simd_f32x4_") + operation + "(" + arguments +
                                                             R"();
        return 0;
      }
    )"),
                                  "unsafe");
  }
}

/** Checks read-before-write semantics when vector source and destination
 * overlap. */
TEST(Builtins_SIMD, overlapping_copy) {
  EXPECT_EQ(sun::driver::executeString(R"(
    /** Copies into an overlapping float range without requiring vector alignment. */
    function main() i32 {
      var lanes: array<f32, 5> = [1.0f32, 2.0f32, 3.0f32, 4.0f32, 5.0f32];
      unsafe { _simd_f32x4_copy(_address_of<f32>(lanes[1]), _address_of<f32>(lanes[0])); };
      return _convert<i32>(lanes[1] + lanes[2] + lanes[3] + lanes[4]);
    }
  )"),
            10);
}

/** Exercises SIMD-backed GEMM and vector values imported from the library
 * bundle. */
TEST(Builtins_SIMD, matrix_and_vectors_from_bundle) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    using std;
    /** Runs register tiles and both row and column tails through bundled Matrix operators. */
    function main() i32 {
      var alloc = make_heap_allocator();
      var a = Matrix<f32>([[1.0f32, 2.0f32], [3.0f32, 4.0f32],
                           [5.0f32, 6.0f32], [7.0f32, 8.0f32],
                           [9.0f32, 10.0f32]], alloc);
      var b = Matrix<f32>([[1.0f32, 2.0f32, 3.0f32, 4.0f32, 5.0f32],
                           [6.0f32, 7.0f32, 8.0f32, 9.0f32, 10.0f32]], alloc);
      var factor = std.simd.splat(2.0f32);
      var squared = factor * factor;
      if (squared.w != 4.0f32) { return -1; }
      match a * b {
        std.linalg.LinalgResult.Ok(c) => {
          if (c[3, 3] != 100.0f32 or c[4, 4] != 145.0f32) { return -3; }
          return _convert<i32>(c[0, 0] + c[1, 4]);
        },
        std.linalg.LinalgResult.Error(error) => { return -2; }
      };
    }
  )"),
            68);
}
