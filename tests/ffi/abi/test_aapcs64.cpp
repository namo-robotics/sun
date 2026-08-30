// tests/ffi/abi/test_aapcs64.cpp — AArch64 AAPCS64 (ELF) argument
// classification
//
// The expectations here are clang's actual output for the equivalent C
// declarations on aarch64-linux-gnu, e.g.
//
//   struct T { int a, b, c; };  void t(struct T);
//     -> declare void @t([2 x i64])
//
// If one of these ever disagrees with clang, the calls Sun emits to C are
// wrong, so they are written as exact type comparisons rather than
// approximate shape checks. Reproduce any of them with:
//
//   echo '<decls>' | clang --target=aarch64-linux-gnu -S -emit-llvm -o - -x c -

#include <gtest/gtest.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <sys/wait.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

#include "codegen/abi/aapcs64.h"
#include "codegen/abi/c_abi.h"
#include "codegen/abi/sysv_x86_64.h"
#include "driver/compiler.h"
#include "driver/driver.h"
#include "support/error.h"

namespace {

class Ffi_Abi_Aapcs64 : public ::testing::Test {
 protected:
  llvm::LLVMContext ctx;
  // Standard aarch64-linux-gnu layout, so the test does not depend on the
  // host.
  llvm::DataLayout dl{
      "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-"
      "i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"};

  llvm::Type* i8() { return llvm::Type::getInt8Ty(ctx); }
  llvm::Type* i16() { return llvm::Type::getInt16Ty(ctx); }
  llvm::Type* i32() { return llvm::Type::getInt32Ty(ctx); }
  llvm::Type* i64() { return llvm::Type::getInt64Ty(ctx); }
  llvm::Type* f32() { return llvm::Type::getFloatTy(ctx); }
  llvm::Type* f64() { return llvm::Type::getDoubleTy(ctx); }
  llvm::Type* ptr() { return llvm::PointerType::getUnqual(ctx); }

  llvm::StructType* structOf(std::initializer_list<llvm::Type*> fields) {
    return llvm::StructType::get(ctx, std::vector<llvm::Type*>(fields));
  }

  llvm::Type* arrayOf(llvm::Type* elem, uint64_t n) {
    return llvm::ArrayType::get(elem, n);
  }
};

}  // namespace

// --- scalars pass through untouched -----------------------------------------

TEST_F(Ffi_Abi_Aapcs64, scalars_are_direct) {
  for (llvm::Type* t : {i8(), i32(), i64(), f32(), f64(), ptr()}) {
    auto lowering = sun::abi::aapcs64::lowerArgument(t, dl);
    EXPECT_TRUE(lowering.isDirect()) << "type should not be rewritten";
    EXPECT_EQ(lowering.type, t);
  }
}

TEST_F(Ffi_Abi_Aapcs64, void_return_is_direct) {
  EXPECT_TRUE(sun::abi::aapcs64::lowerReturn(llvm::Type::getVoidTy(ctx), dl)
                  .isDirect());
}

// --- small non-HFA aggregates coerce to whole registers ----------------------

TEST_F(Ffi_Abi_Aapcs64, two_ints_coerce_to_one_i64) {
  // void fp(struct { int, int }) -> declare void @fp(i64)
  auto lowering =
      sun::abi::aapcs64::lowerArgument(structOf({i32(), i32()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], i64());
  ASSERT_EQ(lowering.pieceOffsets.size(), 1u);
  EXPECT_EQ(lowering.pieceOffsets[0], 0u);
}

TEST_F(Ffi_Abi_Aapcs64, small_argument_rounds_up_to_a_full_register) {
  // Unlike x86-64's exact widths, a 1- or 3-byte struct is still i64:
  // declare void @c1(i64), declare void @c3(i64)
  for (auto* small : {structOf({i8()}), structOf({i8(), i8(), i8()})}) {
    auto lowering = sun::abi::aapcs64::lowerArgument(small, dl);
    ASSERT_TRUE(lowering.isCoerced());
    ASSERT_EQ(lowering.pieces.size(), 1u);
    EXPECT_EQ(lowering.pieces[0], i64());
  }
}

TEST_F(Ffi_Abi_Aapcs64, twelve_bytes_coerce_to_an_i64_pair) {
  // void ft(struct { int, int, int }) -> declare void @ft([2 x i64])
  auto lowering =
      sun::abi::aapcs64::lowerArgument(structOf({i32(), i32(), i32()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], arrayOf(i64(), 2));
}

TEST_F(Ffi_Abi_Aapcs64, nine_byte_array_member_coerces_to_an_i64_pair) {
  // declare void @s9([2 x i64]) for struct { char c[9]; }
  auto lowering =
      sun::abi::aapcs64::lowerArgument(structOf({arrayOf(i8(), 9)}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], arrayOf(i64(), 2));
}

TEST_F(Ffi_Abi_Aapcs64, int_and_double_use_integer_registers_not_sse) {
  // No per-eightbyte FP classes here, unlike SysV:
  // declare void @m([2 x i64]) for struct { int, double }
  auto lowering =
      sun::abi::aapcs64::lowerArgument(structOf({i32(), f64()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], arrayOf(i64(), 2));
}

TEST_F(Ffi_Abi_Aapcs64, float_beside_an_int_is_not_an_hfa) {
  // declare void @fi(i64) for struct { float, int }
  auto lowering =
      sun::abi::aapcs64::lowerArgument(structOf({f32(), i32()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], i64());
}

TEST_F(Ffi_Abi_Aapcs64, mixed_float_and_double_is_not_an_hfa) {
  // Base types differ -> integer registers: [2 x i64] for { float, double }
  auto lowering =
      sun::abi::aapcs64::lowerArgument(structOf({f32(), f64()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], arrayOf(i64(), 2));
}

// --- HFAs travel in FP registers ---------------------------------------------

TEST_F(Ffi_Abi_Aapcs64, single_float_struct_is_a_one_element_hfa) {
  // declare void @fh1([1 x float] alignstack(8)) — yes, clang really keeps
  // the one-element array.
  auto lowering = sun::abi::aapcs64::lowerArgument(structOf({f32()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], arrayOf(f32(), 1));
  EXPECT_EQ(lowering.stackAlign, 8u);
}

TEST_F(Ffi_Abi_Aapcs64, homogeneous_floats_coerce_to_a_float_array) {
  // declare void @ff([2 x float] alignstack(8)),
  // declare void @fh([3 x float] alignstack(8))
  auto two = sun::abi::aapcs64::lowerArgument(structOf({f32(), f32()}), dl);
  ASSERT_TRUE(two.isCoerced());
  ASSERT_EQ(two.pieces.size(), 1u);
  EXPECT_EQ(two.pieces[0], arrayOf(f32(), 2));

  auto three =
      sun::abi::aapcs64::lowerArgument(structOf({f32(), f32(), f32()}), dl);
  ASSERT_TRUE(three.isCoerced());
  ASSERT_EQ(three.pieces.size(), 1u);
  EXPECT_EQ(three.pieces[0], arrayOf(f32(), 3));
  EXPECT_EQ(three.stackAlign, 8u);
}

TEST_F(Ffi_Abi_Aapcs64, four_doubles_are_an_hfa_despite_being_32_bytes) {
  // HFAs are exempt from the 16-byte limit:
  // declare void @fh4([4 x double] alignstack(8))
  auto lowering = sun::abi::aapcs64::lowerArgument(
      structOf({f64(), f64(), f64(), f64()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], arrayOf(f64(), 4));
  EXPECT_EQ(lowering.stackAlign, 8u);
}

TEST_F(Ffi_Abi_Aapcs64, nested_structs_and_arrays_flatten_for_hfa_detection) {
  // struct { struct { float, float }, float } and struct { float[3] } both
  // -> declare void @n([3 x float] alignstack(8))
  auto nested = sun::abi::aapcs64::lowerArgument(
      structOf({structOf({f32(), f32()}), f32()}), dl);
  ASSERT_TRUE(nested.isCoerced());
  ASSERT_EQ(nested.pieces.size(), 1u);
  EXPECT_EQ(nested.pieces[0], arrayOf(f32(), 3));

  auto viaArray =
      sun::abi::aapcs64::lowerArgument(structOf({arrayOf(f32(), 3)}), dl);
  ASSERT_TRUE(viaArray.isCoerced());
  ASSERT_EQ(viaArray.pieces.size(), 1u);
  EXPECT_EQ(viaArray.pieces[0], arrayOf(f32(), 3));
}

TEST_F(Ffi_Abi_Aapcs64, five_floats_are_not_an_hfa_and_go_to_memory) {
  // 5 > 4 members disqualifies the HFA, and 20 bytes exceeds two registers.
  auto lowering = sun::abi::aapcs64::lowerArgument(
      structOf({f32(), f32(), f32(), f32(), f32()}), dl);
  EXPECT_TRUE(lowering.isIndirect());
}

// --- memory-class aggregates
// --------------------------------------------------

TEST_F(Ffi_Abi_Aapcs64, aggregate_over_16_bytes_is_a_plain_pointer) {
  // declare void @fb(ptr noundef) — no byval, unlike x86-64. The caller
  // makes the copy; the pointer itself is the argument.
  auto big = structOf({i32(), i32(), i32(), i32(), i32()});
  auto lowering = sun::abi::aapcs64::lowerArgument(big, dl);
  ASSERT_TRUE(lowering.isIndirect());
  EXPECT_EQ(lowering.type, big);
  EXPECT_FALSE(lowering.indirectByval);
}

TEST_F(Ffi_Abi_Aapcs64, exactly_16_bytes_still_fits_in_registers) {
  auto lowering =
      sun::abi::aapcs64::lowerArgument(structOf({i64(), i64()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], arrayOf(i64(), 2));
}

// --- zero-sized aggregates
// ----------------------------------------------------

TEST_F(Ffi_Abi_Aapcs64, empty_struct_passes_nothing) {
  // declare void @e() for void e(struct {})
  auto lowering = sun::abi::aapcs64::lowerArgument(structOf({}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  EXPECT_TRUE(lowering.pieces.empty());
}

// --- returns
// -------------------------------------------------------------------

TEST_F(Ffi_Abi_Aapcs64, small_return_uses_the_exact_width) {
  // Returns are NOT rounded up the way arguments are:
  // declare i24 @rc3() for struct { char a, b, c }
  auto three = sun::abi::aapcs64::lowerReturn(structOf({i8(), i8(), i8()}), dl);
  ASSERT_TRUE(three.isCoerced());
  ASSERT_EQ(three.pieces.size(), 1u);
  EXPECT_EQ(three.pieces[0], llvm::IntegerType::get(ctx, 24));

  // declare i64 @rp() for struct { int, int }
  auto pair = sun::abi::aapcs64::lowerReturn(structOf({i32(), i32()}), dl);
  ASSERT_TRUE(pair.isCoerced());
  ASSERT_EQ(pair.pieces.size(), 1u);
  EXPECT_EQ(pair.pieces[0], i64());
}

TEST_F(Ffi_Abi_Aapcs64, nine_to_sixteen_byte_return_is_an_i64_pair) {
  // declare [2 x i64] @rm() for struct { int, double },
  // declare [2 x i64] @rs9() for struct { char c[9] }
  for (auto* t : {structOf({i32(), f64()}), structOf({arrayOf(i8(), 9)})}) {
    auto lowering = sun::abi::aapcs64::lowerReturn(t, dl);
    ASSERT_TRUE(lowering.isCoerced());
    ASSERT_EQ(lowering.pieces.size(), 1u);
    EXPECT_EQ(lowering.pieces[0], arrayOf(i64(), 2));
  }
}

TEST_F(Ffi_Abi_Aapcs64, hfa_return_stays_the_literal_struct_type) {
  // declare %struct.FF @rff() — Direct, not an array coercion; LLVM assigns
  // the FP registers itself.
  auto hfa = structOf({f32(), f32()});
  auto lowering = sun::abi::aapcs64::lowerReturn(hfa, dl);
  EXPECT_TRUE(lowering.isDirect());
  EXPECT_EQ(lowering.type, hfa);
}

TEST_F(Ffi_Abi_Aapcs64, large_struct_return_uses_sret) {
  // declare void @rb(ptr sret(%struct.Big) align 4)
  auto big = structOf({i32(), i32(), i32(), i32(), i32()});
  auto lowering = sun::abi::aapcs64::lowerReturn(big, dl);
  ASSERT_TRUE(lowering.isIndirect());
  EXPECT_EQ(lowering.type, big);
}

// --- assembled function types ----------------------------------------------

TEST_F(Ffi_Abi_Aapcs64, coerced_array_stays_one_parameter) {
  // Unlike SysV's expanded eightbytes, [2 x i64] is a single LLVM argument:
  // void f(int, struct{int,int,int}, int) -> void f(i32, [2 x i64], i32)
  llvm::Type* params[] = {i32(), structOf({i32(), i32(), i32()}), i32()};
  auto lowering = sun::abi::aapcs64::lowerCSignature(llvm::Type::getVoidTy(ctx),
                                                     params, dl);
  auto* fnTy = sun::abi::buildLoweredFunctionType(lowering, ctx, false);

  ASSERT_EQ(fnTy->getNumParams(), 3u);
  EXPECT_EQ(fnTy->getParamType(0), i32());
  EXPECT_EQ(fnTy->getParamType(1), arrayOf(i64(), 2));
  EXPECT_EQ(fnTy->getParamType(2), i32());
}

TEST_F(Ffi_Abi_Aapcs64, lowered_type_prepends_the_sret_pointer) {
  auto big = structOf({i32(), i32(), i32(), i32(), i32()});
  llvm::Type* params[] = {i32()};
  auto lowering = sun::abi::aapcs64::lowerCSignature(big, params, dl);
  ASSERT_TRUE(lowering.usesSret());

  auto* fnTy = sun::abi::buildLoweredFunctionType(lowering, ctx, false);
  ASSERT_EQ(fnTy->getNumParams(), 2u);
  EXPECT_TRUE(fnTy->getParamType(0)->isPointerTy());
  EXPECT_EQ(fnTy->getParamType(1), i32());
  EXPECT_TRUE(fnTy->getReturnType()->isVoidTy());
}

TEST_F(Ffi_Abi_Aapcs64, all_scalar_signature_is_trivial) {
  llvm::Type* params[] = {i32(), f64(), ptr()};
  EXPECT_TRUE(
      sun::abi::aapcs64::lowerCSignature(i32(), params, dl).isTrivial());
}

TEST_F(Ffi_Abi_Aapcs64, signature_with_an_aggregate_is_not_trivial) {
  llvm::Type* params[] = {structOf({i32(), i32()})};
  EXPECT_FALSE(
      sun::abi::aapcs64::lowerCSignature(llvm::Type::getVoidTy(ctx), params, dl)
          .isTrivial());
}

// ============================================================================
// Triple dispatch
// ============================================================================

namespace {

class Ffi_Abi_CDispatch : public Ffi_Abi_Aapcs64 {};

}  // namespace

TEST_F(Ffi_Abi_CDispatch, same_struct_lowers_differently_per_target) {
  // struct { int, double }: SysV splits it into an integer and an SSE
  // eightbyte; AAPCS64 uses two integer registers.
  llvm::DataLayout x86Dl{
      "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-"
      "i128:128-f80:128-n8:16:32:64-S128"};
  llvm::Type* params[] = {structOf({i32(), f64()})};
  llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);

  auto sysvLowering = sun::abi::lowerCSignature(
      llvm::Triple("x86_64-unknown-linux-gnu"), voidTy, params, x86Dl);
  ASSERT_EQ(sysvLowering.params[0].pieces.size(), 2u);
  EXPECT_EQ(sysvLowering.params[0].pieces[0], i32());
  EXPECT_EQ(sysvLowering.params[0].pieces[1], f64());

  auto aapcsLowering = sun::abi::lowerCSignature(
      llvm::Triple("aarch64-unknown-linux-gnu"), voidTy, params, dl);
  ASSERT_EQ(aapcsLowering.params[0].pieces.size(), 1u);
  EXPECT_EQ(aapcsLowering.params[0].pieces[0], arrayOf(i64(), 2));
}

TEST_F(Ffi_Abi_CDispatch, unimplemented_targets_are_a_compile_error) {
  llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
  EXPECT_THROW(sun::abi::lowerCSignature(
                   llvm::Triple("riscv64-unknown-linux-gnu"), voidTy, {}, dl),
               SunError);
  // Apple arm64 has its own AAPCS64 variant — covered in test_darwin_abi.cpp.
  EXPECT_NO_THROW(sun::abi::lowerCSignature(
      llvm::Triple("aarch64-apple-darwin"), voidTy, {}, dl));
}

// ============================================================================
// Cross-target driver integration (--target aarch64-linux-gnu)
// ============================================================================

TEST(Ffi_Abi_CrossTarget, aarch64_extern_ir_uses_aapcs64_lowering) {
  auto driver = Driver::createForAOT("cross_ir_module", "aarch64-linux-gnu");
  driver->compileString(R"(
    class Triplet {
        var a: i32; var b: i32; var c: i32;
        init(a: i32, b: i32, c: i32) {
            this.a = a; this.b = b; this.c = c;
        }
    }
    extern "C" function sun_ffi_take_triplet(t: Triplet) i32;

    function main() i32 {
        var t = Triplet(1, 2, 3);
        unsafe { return sun_ffi_take_triplet(t); };
    }
  )");

  std::string ir;
  llvm::raw_string_ostream os(ir);
  driver->getModule().print(os, nullptr);
  EXPECT_NE(ir.find("[2 x i64]"), std::string::npos)
      << "12-byte struct should coerce to [2 x i64] on aarch64";
  EXPECT_NE(driver->getModule().getTargetTriple().find("aarch64"),
            std::string::npos);
}

TEST(Ffi_Abi_CrossTarget, emits_an_aarch64_elf_object) {
  auto driver = Driver::createForAOT("cross_obj_module", "aarch64-linux-gnu");
  driver->compileString("function main() i32 { return 0; }");

  std::string path = ::testing::TempDir() + "sun_cross_target_test.o";
  std::string errorMsg;
  ASSERT_TRUE(sun::emitObjectFile(driver->getModule(), path, errorMsg))
      << errorMsg;

  std::ifstream obj(path, std::ios::binary);
  ASSERT_TRUE(obj.good());
  unsigned char header[20] = {};
  obj.read(reinterpret_cast<char*>(header), sizeof(header));
  ASSERT_EQ(obj.gcount(), static_cast<std::streamsize>(sizeof(header)));

  EXPECT_EQ(header[0], 0x7fu);
  EXPECT_EQ(header[1], 'E');
  EXPECT_EQ(header[2], 'L');
  EXPECT_EQ(header[3], 'F');
  // e_machine at offset 18, little-endian: EM_AARCH64 == 183.
  uint16_t machine = static_cast<uint16_t>(header[18]) |
                     (static_cast<uint16_t>(header[19]) << 8);
  EXPECT_EQ(machine, 183u);
}

// ============================================================================
// Execution under qemu (needs g++-aarch64-linux-gnu + qemu-user, in Dockerfile)
// ============================================================================

namespace {

constexpr const char* kQemuSysroot = "/usr/aarch64-linux-gnu";

bool haveCrossExecutionTools() {
  return std::system("command -v qemu-aarch64 >/dev/null 2>&1") == 0 &&
         std::system("command -v aarch64-linux-gnu-gcc >/dev/null 2>&1") == 0;
}

int runUnderQemu(const std::string& binary) {
  std::string cmd = "qemu-aarch64 -L " + std::string(kQemuSysroot) + " " +
                    binary + " >/dev/null 2>&1";
  int rc = std::system(cmd.c_str());
  return WEXITSTATUS(rc);
}

}  // namespace

TEST(Ffi_Abi_CrossTarget, cross_binary_runs_under_qemu) {
  if (!haveCrossExecutionTools()) {
    GTEST_SKIP() << "qemu-aarch64 / aarch64-linux-gnu-gcc not installed";
  }

  auto driver = Driver::createForAOT("qemu_run_module", "aarch64-linux-gnu");
  driver->compileString("function main() i32 { return 42; }");

  std::string binary = ::testing::TempDir() + "sun_qemu_run_test";
  std::string errorMsg;
  sun::LinkOptions linkOpts;
  linkOpts.targetTriple = "aarch64-linux-gnu";
  ASSERT_TRUE(sun::compileToExecutable(driver->getModule(), binary, errorMsg,
                                       /*keepObjectFile=*/false, linkOpts))
      << errorMsg;

  EXPECT_EQ(runUnderQemu(binary), 42);
}

TEST(Ffi_Abi_CrossTarget, extern_struct_call_runs_under_qemu) {
  // End-to-end proof of the AAPCS64 marshalling: a struct-by-value extern
  // crosses into C code compiled by the real aarch64 toolchain, on an
  // emulated aarch64 CPU. A classification mismatch shows up as a wrong
  // exit code, not a compile error.
  if (!haveCrossExecutionTools()) {
    GTEST_SKIP() << "qemu-aarch64 / aarch64-linux-gnu-gcc not installed";
  }

  std::string dir = ::testing::TempDir();
  std::string cSource = dir + "sun_cross_pair.c";
  {
    std::ofstream out(cSource);
    out << "struct Pair { int a; int b; };\n"
           "int take_pair(struct Pair p) { return p.a * 100 + p.b; }\n";
  }
  ASSERT_EQ(std::system(("aarch64-linux-gnu-gcc -c -o " + dir +
                         "sun_cross_pair.o " + cSource)
                            .c_str()),
            0);
  ASSERT_EQ(std::system(("aarch64-linux-gnu-ar rcs " + dir +
                         "libsun_cross_pair.a " + dir + "sun_cross_pair.o")
                            .c_str()),
            0);

  auto driver = Driver::createForAOT("qemu_ffi_module", "aarch64-linux-gnu");
  driver->compileString(R"(
    class Pair {
        var a: i32;
        var b: i32;
        init(a: i32, b: i32) { this.a = a; this.b = b; }
    }
    extern "C" function take_pair(p: Pair) i32;

    function main() i32 {
        var p = Pair(2, 42);
        unsafe { return take_pair(p); };
    }
  )");

  std::string binary = dir + "sun_qemu_ffi_test";
  std::string errorMsg;
  sun::LinkOptions linkOpts;
  linkOpts.targetTriple = "aarch64-linux-gnu";
  linkOpts.searchPaths = {dir};
  linkOpts.libraries = {"sun_cross_pair"};
  ASSERT_TRUE(sun::compileToExecutable(driver->getModule(), binary, errorMsg,
                                       /*keepObjectFile=*/false, linkOpts))
      << errorMsg;

  EXPECT_EQ(runUnderQemu(binary), 242);  // a*100 + b
}

// ============================================================================
// Static linking
// ============================================================================

namespace {

// True when the ELF at `path` needs no dynamic loader (no PT_INTERP segment).
bool isStaticBinary(const std::string& path) {
  std::string cmd = "! readelf -l " + path + " 2>/dev/null | grep -q INTERP";
  return std::system(cmd.c_str()) == 0;
}

}  // namespace

TEST(Ffi_Abi_StaticLink, host_static_binary_has_no_dynamic_dependencies) {
  if (llvm::Triple(llvm::sys::getDefaultTargetTriple()).isOSDarwin()) {
    GTEST_SKIP() << "macOS has no fully static binaries";
  }
  auto driver = Driver::createForAOT("static_host_module");
  driver->compileString("function main() i32 { return 42; }");

  std::string binary = ::testing::TempDir() + "sun_static_host_test";
  std::string errorMsg;
  sun::LinkOptions linkOpts;
  linkOpts.staticLink = true;
  ASSERT_TRUE(sun::compileToExecutable(driver->getModule(), binary, errorMsg,
                                       /*keepObjectFile=*/false, linkOpts))
      << errorMsg;

  EXPECT_TRUE(isStaticBinary(binary));
  int rc = std::system((binary + " >/dev/null 2>&1").c_str());
  EXPECT_EQ(WEXITSTATUS(rc), 42);
}

TEST(Ffi_Abi_StaticLink, cross_static_binary_runs_under_qemu_without_sysroot) {
  // The deployment shape cross-compilation exists for: one self-contained
  // aarch64 file that runs with no target rootfs at all — qemu gets no -L.
  if (!haveCrossExecutionTools()) {
    GTEST_SKIP() << "qemu-aarch64 / aarch64-linux-gnu-gcc not installed";
  }

  auto driver =
      Driver::createForAOT("static_cross_module", "aarch64-linux-gnu");
  driver->compileString("function main() i32 { return 42; }");

  std::string binary = ::testing::TempDir() + "sun_static_cross_test";
  std::string errorMsg;
  sun::LinkOptions linkOpts;
  linkOpts.targetTriple = "aarch64-linux-gnu";
  linkOpts.staticLink = true;
  ASSERT_TRUE(sun::compileToExecutable(driver->getModule(), binary, errorMsg,
                                       /*keepObjectFile=*/false, linkOpts))
      << errorMsg;

  EXPECT_TRUE(isStaticBinary(binary));
  int rc = std::system(("qemu-aarch64 " + binary + " >/dev/null 2>&1").c_str());
  EXPECT_EQ(WEXITSTATUS(rc), 42);
}

// ============================================================================
// musl targets
// ============================================================================
// musl is ABI-identical to glibc at the calling-convention level; only the
// libc the linker resolves against differs. The dispatcher must accept the
// musl environment, and static links must prefer a musl toolchain when one
// is installed (musl.cc's <arch>-linux-musl-gcc).

TEST_F(Ffi_Abi_CDispatch, musl_environment_uses_the_same_arch_rules) {
  llvm::Type* params[] = {structOf({i32(), i32(), i32()})};
  llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);

  auto aapcs = sun::abi::lowerCSignature(llvm::Triple("aarch64-linux-musl"),
                                         voidTy, params, dl);
  ASSERT_EQ(aapcs.params[0].pieces.size(), 1u);
  EXPECT_EQ(aapcs.params[0].pieces[0], arrayOf(i64(), 2));

  llvm::DataLayout x86Dl{
      "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-"
      "i128:128-f80:128-n8:16:32:64-S128"};
  auto sysv = sun::abi::lowerCSignature(llvm::Triple("x86_64-linux-musl"),
                                        voidTy, params, x86Dl);
  ASSERT_EQ(sysv.params[0].pieces.size(), 2u);
}

namespace {

bool haveHostMuslToolchain() {
  return std::system("command -v x86_64-linux-musl-gcc >/dev/null 2>&1") == 0;
}

bool haveAarch64MuslToolchain() {
  return std::system("command -v aarch64-linux-musl-gcc >/dev/null 2>&1") ==
             0 &&
         std::system("command -v qemu-aarch64 >/dev/null 2>&1") == 0;
}

// Exercises the C++ exception runtime, the part of static linking that
// genuinely depends on the toolchain shipping a musl-built libstdc++.
constexpr const char* kThrowingProgram = R"(
    class DivByZero implements IError {
      init() {}
      method code() i32 { return 1; }
      method message() static_ptr<u8> { return "division by zero"; }
    }
    function divide(a: i32, b: i32) i32 throws IError {
        if (b == 0) { throw DivByZero(); }
        return a / b;
    }
    function main() i32 {
        try {
            var x = divide(10, 0);
            return 1;
        } catch (e: IError) {
            return 42;
        }
    }
)";

}  // namespace

TEST(Ffi_Abi_StaticLink, static_links_prefer_the_musl_toolchain) {
  if (!haveHostMuslToolchain()) {
    GTEST_SKIP() << "x86_64-linux-musl-gcc not installed";
  }
  EXPECT_EQ(sun::linkerCommandFor("", /*staticLink=*/true),
            "x86_64-linux-musl-gcc");
  // Dynamic links stay on the host toolchain.
  EXPECT_EQ(sun::linkerCommandFor("", /*staticLink=*/false), "cc");
}

TEST(Ffi_Abi_StaticLink, host_musl_static_binary_handles_exceptions) {
  if (!haveHostMuslToolchain()) {
    GTEST_SKIP() << "x86_64-linux-musl-gcc not installed";
  }

  auto driver = Driver::createForAOT("musl_host_module");
  driver->compileString(kThrowingProgram);

  std::string binary = ::testing::TempDir() + "sun_musl_host_test";
  std::string errorMsg;
  sun::LinkOptions linkOpts;
  linkOpts.staticLink = true;
  ASSERT_TRUE(sun::compileToExecutable(driver->getModule(), binary, errorMsg,
                                       /*keepObjectFile=*/false, linkOpts))
      << errorMsg;

  EXPECT_TRUE(isStaticBinary(binary));
  int rc = std::system((binary + " >/dev/null 2>&1").c_str());
  EXPECT_EQ(WEXITSTATUS(rc), 42);
}

TEST(Ffi_Abi_StaticLink,
     cross_musl_static_binary_handles_exceptions_under_qemu) {
  if (!haveAarch64MuslToolchain()) {
    GTEST_SKIP() << "aarch64-linux-musl-gcc / qemu-aarch64 not installed";
  }

  auto driver = Driver::createForAOT("musl_cross_module", "aarch64-linux-musl");
  driver->compileString(kThrowingProgram);

  std::string binary = ::testing::TempDir() + "sun_musl_cross_test";
  std::string errorMsg;
  sun::LinkOptions linkOpts;
  linkOpts.targetTriple = "aarch64-linux-musl";
  linkOpts.staticLink = true;
  ASSERT_TRUE(sun::compileToExecutable(driver->getModule(), binary, errorMsg,
                                       /*keepObjectFile=*/false, linkOpts))
      << errorMsg;

  EXPECT_TRUE(isStaticBinary(binary));
  int rc = std::system(("qemu-aarch64 " + binary + " >/dev/null 2>&1").c_str());
  EXPECT_EQ(WEXITSTATUS(rc), 42);
}

TEST(Ffi_Abi_StaticLink, cross_musl_stdlib_binary_runs_under_qemu) {
  // Bundles resolve by exact name, so the cross build names the per-target
  // bundle explicitly. The aarch64-gnu bundle serves the musl target: the
  // bitcode is libc-agnostic (portable POSIX symbols), and the linker's
  // triple check compares architectures. The parser's pick and the linker's
  // pick must also agree, or symbol hashes diverge.
  if (!haveAarch64MuslToolchain()) {
    GTEST_SKIP() << "aarch64-linux-musl-gcc / qemu-aarch64 not installed";
  }

  auto driver =
      Driver::createForAOT("musl_stdlib_module", "aarch64-linux-musl");
  driver->setMoonImports(
      {sun::MoonImport("build/aarch64-linux-gnu/stdlib.moon")});
  driver->compileString(R"(
    using std;
    function main() i32 {
        println("hello from musl");
        return 42;
    }
  )");

  std::string binary = ::testing::TempDir() + "sun_musl_stdlib_test";
  std::string errorMsg;
  sun::LinkOptions linkOpts;
  linkOpts.targetTriple = "aarch64-linux-musl";
  linkOpts.staticLink = true;
  ASSERT_TRUE(sun::compileToExecutable(driver->getModule(), binary, errorMsg,
                                       /*keepObjectFile=*/false, linkOpts))
      << errorMsg;

  EXPECT_TRUE(isStaticBinary(binary));
  int rc = std::system(("qemu-aarch64 " + binary + " >/dev/null 2>&1").c_str());
  EXPECT_EQ(WEXITSTATUS(rc), 42);
}
