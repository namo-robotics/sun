/** Checks vector lowering and unsafe boundaries for fixed-width float
 * intrinsics.
 */
#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>

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
        _simd_f32x4_fma(out, a, b, out);
        _simd_f32x4_mul_add(out, a, b, out);
        _simd_f32x4_copy(out, a);
      };
    }
    /** Keeps wide lowering independent of stores in the narrow test. */
    function compute8(out: raw_ptr<f32>, a: raw_ptr<f32>, b: raw_ptr<f32>) void {
      unsafe {
        _simd_f32x8_splat(out, 2.0f32);
        _simd_f32x8_add(out, a, b);
        _simd_f32x8_sub(out, a, b);
        _simd_f32x8_mul(out, a, b);
        _simd_f32x8_fma(out, a, b, out);
        _simd_f32x8_mul_add(out, a, b, out);
        _simd_f32x8_copy(out, a);
      };
    }
  )");
  std::string ir;
  llvm::raw_string_ostream stream(ir);
  driver->getModule().print(stream, nullptr);
  EXPECT_NE(ir.find("llvm.fma.v8f32"), std::string::npos);
  EXPECT_NE(ir.find("llvm.fmuladd.v8f32"), std::string::npos);
  EXPECT_NE(ir.find("fadd <8 x float>"), std::string::npos);
  EXPECT_NE(ir.find("fsub <8 x float>"), std::string::npos);
  EXPECT_NE(ir.find("fmul <8 x float>"), std::string::npos);
  EXPECT_NE(ir.find("load <8 x float>"), std::string::npos);
  EXPECT_NE(ir.find("store <8 x float>"), std::string::npos);
  EXPECT_NE(ir.find("llvm.fma.v4f32"), std::string::npos);
  EXPECT_NE(ir.find("llvm.fmuladd.v4f32"), std::string::npos);
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
  for (const std::string operation :
       {"copy", "splat", "add", "sub", "mul", "fma", "mul_add"}) {
    const std::string arguments = (operation == "fma" || operation == "mul_add")
                                      ? "p, p, p, p"
                                  : operation == "splat" ? "p, 1.0f32"
                                  : operation == "copy"  ? "p, p"
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
      var contracted = factor.mul_add(factor, factor);
      if (contracted.w != 6.0f32) { return -6; }
      var fused = factor.fma(factor, factor);
      if (fused.w != 6.0f32) { return -4; }
      var wide = std.simd.F32x8(2.0f32);
      var wide_fused = wide.fma(wide, wide);
      if (wide_fused.x7 != 6.0f32) { return -5; }
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

/** Eight-lane primitives retain the same explicit unsafe boundary. */
TEST(Builtins_SIMD, wide_operations_require_unsafe) {
  for (const std::string operation :
       {"copy", "splat", "add", "sub", "mul", "fma", "mul_add"}) {
    const std::string arguments = (operation == "fma" || operation == "mul_add")
                                      ? "p, p, p, p"
                                  : operation == "splat" ? "p, 1.0f32"
                                  : operation == "copy"  ? "p, p"
                                                         : "p, p, p";
    EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(R"(
      /** Rejects raw access outside unsafe. */
      function main() i32 {
        var p: raw_ptr<f32> = null;
    )" + std::string("_simd_f32x8_") + operation + "(" + arguments +
                                                             R"();
        return 0;
      }
    )"),
                                  "unsafe");
  }
}

/** Wide loads complete before overlapping stores, without vector alignment. */
TEST(Builtins_SIMD, overlapping_wide_copy) {
  EXPECT_EQ(sun::driver::executeString(R"(
    /** Shifts eight initialized floats by one element. */
    function main() i32 {
      var lanes = [1.0f32, 2.0f32, 3.0f32, 4.0f32, 5.0f32, 6.0f32, 7.0f32, 8.0f32, 9.0f32];
      unsafe { _simd_f32x8_copy(_address_of<f32>(lanes[1]), _address_of<f32>(lanes[0])); };
      var result: f32 = 0.0f32;
      for (var i: i64 = 1; i <= 8; i = i + 1) { result = result + lanes[i]; }
      return _convert<i32>(result);
    }
  )"),
            36);
}

/** Scratch is usable within its function and rejects unsafe or unbounded
 * requests. */
TEST(Builtins_SIMD, bounded_stack_scratch) {
  EXPECT_EQ(sun::driver::executeString(R"(
    /** Initializes stack bytes before reading them. */
    function main() i32 {
      return unsafe {
        var p = _stack_alloc(32);
        _store<i32>(p, 7, 42);
        _load<i32>(p, 7);
      };
    }
  )"),
            42);
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(R"(
    /** Rejects scratch storage outside unsafe. */
    function main() i32 { var p = _stack_alloc(32); return 0; }
  )"),
                                "unsafe");
  for (const std::string bytes : {"0", "-1", "65537"}) {
    EXPECT_SUN_ERROR_WITH_MESSAGE(
        sun::driver::executeString(
            "/** Rejects invalid scratch sizes. */ function main() i32 { "
            "unsafe { var p = _stack_alloc(" +
            bytes + "); }; return 0; }"),
        "constant byte count");
  }
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(R"(
    /** Rejects dynamically sized stack reservations. */
    function reserve(n: i64) i32 { unsafe { var p = _stack_alloc(n); }; return 0; }
    /** Calls the invalid reservation. */
    function main() i32 { return reserve(32); }
  )"),
                                "constant byte count");
}

/** Packed helpers and wide vector operations remain callable through
 * stdlib.moon. */
TEST(Builtins_SIMD, packed_matrix_from_bundle) {
  EXPECT_EQ(sun::driver::executeStringWithStdlib(R"(
    using std;
    /** Crosses an inner-panel boundary with initialized inputs and fresh output. */
    function main() i32 {
      var alloc = make_heap_allocator();
      var a = Matrix<f32>(alloc, [16i64, 65i64]);
      var b = Matrix<f32>(alloc, [65i64, 16i64]);
      var c = Matrix<f32>(alloc, [16i64, 16i64]);
      for (var i: i64 = 0; i < 16; i = i + 1) {
        for (var p: i64 = 0; p < 65; p = p + 1) { a[i, p] = 1.0f32; b[p, i] = 1.0f32; }
      }
      match a.matmul_into(b, c) {
        std.linalg.LinalgResult.Ok => { return _convert<i32>(c[0, 0] + c[15, 15]); },
        std.linalg.LinalgResult.Error(error) => { return -1; }
      };
    }
  )"),
            130);
}

/** Fused inputs are read before overlapping output is written at f32 alignment.
 */
TEST(Builtins_SIMD, overlapping_fma) {
  for (const int lanes : {4, 8}) {
    for (const std::string operation : {"fma", "mul_add"}) {
      EXPECT_EQ(
          sun::driver::executeString(
              R"(
      /** Uses the destination as an overlapping source and addend. */
      function main() i32 {
        var values = [1.0f32, 2.0f32, 3.0f32, 4.0f32, 5.0f32, 6.0f32, 7.0f32, 8.0f32, 9.0f32];
        unsafe { _simd_f32x)" +
              std::to_string(lanes) + "_" + operation +
              R"((_address_of<f32>(values[1]), _address_of<f32>(values[0]), _address_of<f32>(values[0]), _address_of<f32>(values[1])); };
        return _convert<i32>(values[1] + values[)" +
              std::to_string(lanes) + R"(]);
      }
    )"),
          lanes == 4 ? 24 : 76);
    }
  }
}

/** Checks kernel isolation on x86 and removal of unreachable AVX code on ARM.
 */
TEST(Builtins_SIMD, runtime_dispatch_ir) {
  sun::driver::initTestEnvironment();
  for (const std::string target :
       {"", "x86_64-unknown-linux-gnu", "aarch64-apple-darwin",
        "aarch64-linux-gnu"}) {
    SCOPED_TRACE(target.empty() ? "host target" : target);
    auto driver = sun::driver::Driver::createForAOT("dispatch_ir", target);
    driver->compileString(R"(
    /** Calls the specialized tile only after validating CPU support. */
    function compute(a: raw_ptr<u8>, b: raw_ptr<u8>, c: raw_ptr<u8>) void {
      if (_cpu_avx2_fma()) {
        unsafe { _gemm_tile_avx2_fma(a, b, c, 16, 32, 0.0f32); };
        unsafe { _gemm_block_avx2_fma(a, b, c, 32, 32, 0.0f32, 8, 32); };
      }
    }
  )");
    auto& module = driver->getModule();
    auto* kernel = module.getFunction("__sun_gemm_tile_avx2_fma");
    auto* blockKernel = module.getFunction("__sun_gemm_block_avx2_fma");
    if (module.getTargetTriple().starts_with("x86_64")) {
      ASSERT_NE(kernel, nullptr);
      EXPECT_TRUE(kernel->hasFnAttribute(llvm::Attribute::NoInline));
      ASSERT_NE(blockKernel, nullptr);
      EXPECT_TRUE(blockKernel->hasFnAttribute(llvm::Attribute::NoInline));
      EXPECT_EQ(kernel->getFnAttribute("target-features").getValueAsString(),
                "+avx,+avx2,+fma,-avx512f");
      EXPECT_EQ(
          blockKernel->getFnAttribute("target-features").getValueAsString(),
          "+avx,+avx2,+fma,-avx512f");
      auto* detection = module.getFunction("__sun_cpu_avx2_fma");
      ASSERT_NE(detection, nullptr);
      EXPECT_TRUE(detection->hasFnAttribute(llvm::Attribute::NoInline));
      EXPECT_EQ(detection->getFnAttribute("target-cpu").getValueAsString(),
                "x86-64");
      std::string ir;
      llvm::raw_string_ostream stream(ir);
      detection->print(stream);
      EXPECT_NE(ir.find("xgetbv"), std::string::npos);
      EXPECT_NE(ir.find("cpuid"), std::string::npos);
      EXPECT_NE(ir.find("br i1"), std::string::npos);
    } else {
      EXPECT_EQ(module.getFunction("__sun_cpu_avx2_fma"), nullptr);
      // The capability query is false on ARM, so neither guarded kernel
      // survives.
      EXPECT_EQ(kernel, nullptr);
      EXPECT_EQ(blockKernel, nullptr);
    }
  }
}

/** Unchecked packed memory access remains behind the language unsafe boundary.
 */
TEST(Builtins_SIMD, packed_tile_requires_unsafe) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(R"(
    /** Attempts to access packed storage without unsafe. */
    function main() i32 {
      var p: raw_ptr<u8> = null;
      _gemm_tile_avx2_fma(p, p, p, 16, 32, 0.0f32);
      return 0;
    }
  )"),
                                "unsafe");
}

/** Block kernels retain the unchecked-storage safety boundary. */
TEST(Builtins_SIMD, packed_block_requires_unsafe) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(sun::driver::executeString(R"(
    /** Attempts block execution without the required unsafe boundary. */
    function main() i32 {
      var p: raw_ptr<u8> = null;
      _gemm_block_avx2_fma(p, p, p, 32, 3, 0.0f32, 8, 32);
      return 0;
    }
  )"),
                                "unsafe");
}

/** Checks multiple packed tiles, odd k, beta modes, and untouched row padding.
 */
TEST(Builtins_SIMD, packed_block_output_bounds) {
  EXPECT_EQ(sun::driver::executeString(R"(
    /** Uses guarded output to check the packed-block storage contract. */
    function main() i32 {
      if (not _cpu_avx2_fma()) { return 0; }
      unsafe {
        var a = _stack_alloc(96);
        var b = _stack_alloc(384);
        var storage = _stack_alloc(1128);
        var c = _ptr_offset(storage, 4);
        for (var i: i64 = 0; i < 282; i = i + 1) {
          _store<f32>(storage, i, 123.0f32);
        }
        for (var i: i64 = 0; i < 8; i = i + 1) {
          for (var p: i64 = 0; p < 3; p = p + 1) {
            _store<f32>(a, (i / 4) * 12 + p * 4 + i % 4, _convert<f32>(i + 1));
          }
          for (var j: i64 = 0; j < 32; j = j + 1) {
            _store<f32>(c, i * 35 + j, _bitcast<f32>(2143289344u32));
          }
        }
        for (var j: i64 = 0; j < 32; j = j + 1) {
          for (var p: i64 = 0; p < 3; p = p + 1) {
            _store<f32>(b, (j / 8) * 24 + p * 8 + j % 8, _convert<f32>((j + 1) * (p + 1)));
          }
        }
        for (var mode: i64 = 0; mode < 3; mode = mode + 1) {
          var beta: f32 = 0.0f32;
          var factor: f32 = 6.0f32;
          if (mode == 1) { beta = -0.5f32; factor = 3.0f32; }
          if (mode == 2) { beta = 1.0f32; factor = 9.0f32; }
          _gemm_block_avx2_fma(a, b, c, 35, 3, beta, 8, 32);
          for (var i: i64 = 0; i < 8; i = i + 1) {
            for (var j: i64 = 0; j < 35; j = j + 1) {
              var expected: f32 = 123.0f32;
              if (j < 32) { expected = factor * _convert<f32>((i + 1) * (j + 1)); }
              if (_load<f32>(c, i * 35 + j) != expected) { return 1; }
            }
          }
          if (_load<f32>(storage, 0) != 123.0f32 or _load<f32>(storage, 281) != 123.0f32) { return 2; }
        }
      };
      return 0;
    }
  )"),
            0);
}

/** Compares runtime dispatch with LLVM's independent host feature detection. */
TEST(Builtins_SIMD, runtime_cpu_features) {
  const auto features = llvm::sys::getHostCPUFeatures();
  const bool expected =
      llvm::sys::getDefaultTargetTriple().starts_with("x86_64") &&
      features.lookup("avx2") && features.lookup("fma");
  EXPECT_EQ(sun::driver::executeString(R"(
    /** Queries support without requiring unsafe or allocating storage. */
    function main() i32 {
      if (_cpu_avx2_fma()) { return 1; }
      return 0;
    }
  )"),
            expected ? 1 : 0);
}

/** Cross-compilation must not bake in the build host's AVX capabilities. */
TEST(Builtins_SIMD, runtime_dispatch_non_x86) {
  sun::driver::initTestEnvironment();
  auto driver =
      sun::driver::Driver::createForAOT("arm_dispatch", "aarch64-linux-gnu");
  driver->compileString(R"(
    /** ARM targets always select their portable matrix implementation. */
    function available() bool { return _cpu_avx2_fma(); }
  )");
  std::string ir;
  llvm::raw_string_ostream stream(ir);
  driver->getModule().print(stream, nullptr);
  EXPECT_EQ(ir.find("cpuid"), std::string::npos);
  EXPECT_EQ(ir.find("xgetbv"), std::string::npos);
  EXPECT_NE(ir.find("ret i1 false"), std::string::npos);
}
