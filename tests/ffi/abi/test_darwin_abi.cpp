// tests/ffi/abi/test_darwin_abi.cpp — Apple arm64 (Darwin AAPCS64) argument
// classification
//
// The expectations here are clang's actual output for the equivalent C
// declarations on arm64-apple-darwin, e.g.
//
//   void s(char a, unsigned short b);
//     -> declare void @s(i8 signext, i16 zeroext)
//
// Darwin classifies aggregates exactly like aarch64 ELF; what it adds is
// caller-side extension of integers narrower than 32 bits (on returns too),
// and what it drops is the alignstack attribute on HFA arguments. Reproduce
// any expectation with:
//
//   echo '<decls>' | clang --target=arm64-apple-darwin -S -emit-llvm -o - -x c -

#include <gtest/gtest.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "codegen/abi/aapcs64.h"
#include "codegen/abi/c_abi.h"
#include "driver/compiler.h"
#include "driver/driver.h"
#include "moon_bundling/library_cache.h"
#include "support/error.h"

namespace {

using sun::abi::aapcs64::Variant;

class Ffi_Abi_Aapcs64Darwin : public ::testing::Test {
 protected:
  llvm::LLVMContext ctx;
  // Standard arm64-apple-darwin layout (clang's), so the test does not
  // depend on the host. `m:o` is Mach-O name mangling: the leading
  // underscore C symbols get on Apple platforms.
  llvm::DataLayout dl{
      "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-"
      "i128:128-n32:64-S128-Fn32"};

  llvm::Type* i1() { return llvm::Type::getInt1Ty(ctx); }
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

  sun::abi::ArgLowering darwinArg(llvm::Type* t, bool isSigned = false) {
    return sun::abi::aapcs64::lowerArgument(t, dl, Variant::Darwin, isSigned);
  }

  sun::abi::ArgLowering darwinRet(llvm::Type* t, bool isSigned = false) {
    return sun::abi::aapcs64::lowerReturn(t, dl, Variant::Darwin, isSigned);
  }
};

}  // namespace

// --- aggregates classify exactly like ELF ------------------------------------

TEST_F(Ffi_Abi_Aapcs64Darwin, small_aggregates_still_round_up_to_registers) {
  // declare void @take1(i64) ... @take8(i64): Darwin rounds argument
  // aggregates to whole registers just like ELF, packing only stack slots
  // (which is the backend's business, not classification's).
  for (auto* small : {structOf({i8()}), structOf({i8(), i8(), i8()}),
                      structOf({i32(), i32()})}) {
    auto lowering = darwinArg(small);
    ASSERT_TRUE(lowering.isCoerced());
    ASSERT_EQ(lowering.pieces.size(), 1u);
    EXPECT_EQ(lowering.pieces[0], i64());
  }
}

TEST_F(Ffi_Abi_Aapcs64Darwin, twelve_bytes_coerce_to_an_i64_pair) {
  // declare void @take12([2 x i64])
  auto lowering = darwinArg(structOf({i64(), i32()}));
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], arrayOf(i64(), 2));
}

TEST_F(Ffi_Abi_Aapcs64Darwin, seventeen_bytes_pass_indirect_without_byval) {
  // declare void @take17(ptr)
  auto lowering = darwinArg(structOf({i64(), i64(), i8()}));
  ASSERT_TRUE(lowering.isIndirect());
  EXPECT_FALSE(lowering.indirectByval);
}

TEST_F(Ffi_Abi_Aapcs64Darwin, small_returns_use_the_exact_width) {
  // declare i24 @ret3()
  auto lowering = darwinRet(structOf({i8(), i8(), i8()}));
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], llvm::IntegerType::get(ctx, 24));
}

TEST_F(Ffi_Abi_Aapcs64Darwin, big_returns_use_sret) {
  // declare void @ret17(ptr sret(%struct.S17))
  auto lowering = darwinRet(structOf({i64(), i64(), i8()}));
  EXPECT_TRUE(lowering.isIndirect());
}

// --- HFAs coerce like ELF but carry no alignstack ----------------------------

TEST_F(Ffi_Abi_Aapcs64Darwin, hfa_argument_has_no_alignstack) {
  // ELF: declare void @takeh2([2 x float] alignstack(8))
  // Darwin: declare void @takeh2([2 x float])
  auto* hfa = structOf({f32(), f32()});

  auto darwin = darwinArg(hfa);
  ASSERT_TRUE(darwin.isCoerced());
  ASSERT_EQ(darwin.pieces.size(), 1u);
  EXPECT_EQ(darwin.pieces[0], arrayOf(f32(), 2));
  EXPECT_EQ(darwin.stackAlign, 0u);

  auto elf = sun::abi::aapcs64::lowerArgument(hfa, dl, Variant::Elf);
  EXPECT_EQ(elf.stackAlign, 8u);
}

TEST_F(Ffi_Abi_Aapcs64Darwin, four_double_hfa_still_coerces) {
  // declare void @takeh4([4 x double]): HFAs stay register-bound past 16
  // bytes on Darwin too.
  auto lowering = darwinArg(structOf({f64(), f64(), f64(), f64()}));
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], arrayOf(f64(), 4));
  EXPECT_EQ(lowering.stackAlign, 0u);
}

// --- small integers must be extended by the caller ---------------------------

TEST_F(Ffi_Abi_Aapcs64Darwin, small_integer_params_take_an_extension) {
  // declare void @scalars(i8 signext, i8 zeroext, i16 signext, i16 zeroext,
  //                       i32, i32)
  EXPECT_EQ(darwinArg(i8(), /*isSigned=*/true).extend, sun::abi::Extend::Sign);
  EXPECT_EQ(darwinArg(i8(), /*isSigned=*/false).extend, sun::abi::Extend::Zero);
  EXPECT_EQ(darwinArg(i16(), /*isSigned=*/true).extend, sun::abi::Extend::Sign);
  EXPECT_EQ(darwinArg(i16(), /*isSigned=*/false).extend,
            sun::abi::Extend::Zero);
  EXPECT_EQ(darwinArg(i1(), /*isSigned=*/false).extend, sun::abi::Extend::Zero);
}

TEST_F(Ffi_Abi_Aapcs64Darwin, wide_scalars_take_no_extension) {
  EXPECT_EQ(darwinArg(i32(), /*isSigned=*/true).extend, sun::abi::Extend::None);
  EXPECT_EQ(darwinArg(i64(), /*isSigned=*/true).extend, sun::abi::Extend::None);
  EXPECT_EQ(darwinArg(f32()).extend, sun::abi::Extend::None);
  EXPECT_EQ(darwinArg(ptr()).extend, sun::abi::Extend::None);
}

TEST_F(Ffi_Abi_Aapcs64Darwin, small_integer_returns_take_an_extension) {
  // declare signext i8 @retc(); declare zeroext i16 @retus()
  EXPECT_EQ(darwinRet(i8(), /*isSigned=*/true).extend, sun::abi::Extend::Sign);
  EXPECT_EQ(darwinRet(i16(), /*isSigned=*/false).extend,
            sun::abi::Extend::Zero);
  EXPECT_EQ(darwinRet(i32(), /*isSigned=*/true).extend, sun::abi::Extend::None);
}

TEST_F(Ffi_Abi_Aapcs64Darwin, elf_variant_never_extends) {
  // ELF leaves the upper bits unspecified; clang emits no attribute there.
  EXPECT_EQ(
      sun::abi::aapcs64::lowerArgument(i8(), dl, Variant::Elf, true).extend,
      sun::abi::Extend::None);
  EXPECT_EQ(
      sun::abi::aapcs64::lowerReturn(i16(), dl, Variant::Elf, true).extend,
      sun::abi::Extend::None);
}

TEST_F(Ffi_Abi_Aapcs64Darwin, an_extension_makes_the_signature_non_trivial) {
  // The fast call path skips marshalling for trivial signatures; a required
  // extension must force the marshalled path so the call site gets the
  // attribute.
  llvm::Type* params[] = {i8()};
  sun::abi::SignednessInfo signs;
  signs.paramSigned = {true};
  auto lowering = sun::abi::aapcs64::lowerCSignature(
      llvm::Type::getVoidTy(ctx), params, dl, Variant::Darwin, &signs);
  EXPECT_FALSE(lowering.isTrivial());

  llvm::Type* wide[] = {i32(), i64(), ptr()};
  sun::abi::SignednessInfo wideSigns;
  wideSigns.paramSigned = {true, true, false};
  EXPECT_TRUE(sun::abi::aapcs64::lowerCSignature(i32(), wide, dl,
                                                 Variant::Darwin, &wideSigns)
                  .isTrivial());
}

// --- triple dispatch ---------------------------------------------------------

TEST_F(Ffi_Abi_Aapcs64Darwin, dispatch_accepts_apple_arm64_triples) {
  llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
  llvm::Type* params[] = {structOf({f32(), f32()})};
  for (const char* tripleStr :
       {"arm64-apple-darwin", "aarch64-apple-darwin", "arm64-apple-macosx14"}) {
    auto lowering = sun::abi::lowerCSignature(llvm::Triple(tripleStr), voidTy,
                                              params, dl);
    ASSERT_TRUE(lowering.params[0].isCoerced()) << tripleStr;
    EXPECT_EQ(lowering.params[0].stackAlign, 0u)
        << tripleStr << " should use the Darwin variant";
  }
}

TEST_F(Ffi_Abi_Aapcs64Darwin, dispatch_extends_ints_only_for_darwin) {
  llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
  llvm::Type* params[] = {i8()};
  sun::abi::SignednessInfo signs;
  signs.paramSigned = {true};

  auto darwin = sun::abi::lowerCSignature(llvm::Triple("arm64-apple-darwin"),
                                          voidTy, params, dl, &signs);
  EXPECT_EQ(darwin.params[0].extend, sun::abi::Extend::Sign);

  auto elf = sun::abi::lowerCSignature(llvm::Triple("aarch64-linux-gnu"),
                                       voidTy, params, dl, &signs);
  EXPECT_EQ(elf.params[0].extend, sun::abi::Extend::None);
}

TEST_F(Ffi_Abi_Aapcs64Darwin, intel_mac_uses_the_sysv_rules) {
  // x86_64-apple-darwin follows the same System V classification as Linux:
  // struct { int, double } splits into an integer and an SSE eightbyte.
  llvm::DataLayout x86Dl{
      "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-"
      "i128:128-f80:128-n8:16:32:64-S128"};
  llvm::Type* params[] = {structOf({i32(), f64()})};
  auto lowering =
      sun::abi::lowerCSignature(llvm::Triple("x86_64-apple-darwin"),
                                llvm::Type::getVoidTy(ctx), params, x86Dl);
  ASSERT_EQ(lowering.params[0].pieces.size(), 2u);
  EXPECT_EQ(lowering.params[0].pieces[0], i32());
  EXPECT_EQ(lowering.params[0].pieces[1], f64());
}

// --- bundle matching must see the operating system ---------------------------

TEST(Ffi_Abi_DarwinBundles, os_family_separates_linux_from_darwin) {
  // An aarch64 Linux stdlib.moon must never satisfy an arm64 macOS build:
  // open() flags, futex vs ulock, and struct layouts are baked into it.
  EXPECT_FALSE(sun::sameOsFamily(llvm::Triple("aarch64-linux-gnu"),
                                 llvm::Triple("arm64-apple-darwin")));
  EXPECT_TRUE(sun::sameOsFamily(llvm::Triple("arm64-apple-darwin"),
                                llvm::Triple("arm64-apple-macosx14.0")));
  // Environment stays uncompared: musl and glibc layouts are compatible.
  EXPECT_TRUE(sun::sameOsFamily(llvm::Triple("aarch64-linux-musl"),
                                llvm::Triple("aarch64-linux-gnu")));
}

// ============================================================================
// Cross-target driver integration (--target arm64-apple-darwin)
// ============================================================================

TEST(Ffi_Abi_CrossTargetDarwin, extern_ir_carries_extension_attributes) {
  auto driver = Driver::createForAOT("darwin_ir_module", "arm64-apple-darwin");
  driver->compileString(R"(
    extern "C" function sun_ffi_take_byte(b: u8) i32;
    extern "C" function sun_ffi_give_short() i16;

    function main() i32 {
        var b: u8 = 7;
        unsafe {
            var s = sun_ffi_give_short();
            return sun_ffi_take_byte(b);
        };
    }
  )");

  std::string ir;
  llvm::raw_string_ostream os(ir);
  driver->getModule().print(os, nullptr);

  // Declaration and call site both need the attribute; the backend lowers
  // the call from the call site's.
  EXPECT_NE(ir.find("declare i32 @sun_ffi_take_byte(i8 zeroext"),
            std::string::npos)
      << ir;
  EXPECT_NE(ir.find("call i32 @sun_ffi_take_byte(i8 zeroext"),
            std::string::npos)
      << ir;
  EXPECT_NE(ir.find("declare signext i16 @sun_ffi_give_short"),
            std::string::npos)
      << ir;
}

TEST(Ffi_Abi_CrossTargetDarwin, struct_argument_coerces_like_elf) {
  auto driver = Driver::createForAOT("darwin_struct_module",
                                     "arm64-apple-darwin");
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
      << "12-byte struct should coerce to [2 x i64] on arm64 Darwin";
}

TEST(Ffi_Abi_CrossTargetDarwin, dead_target_branch_emits_no_extern_call) {
  // The reason _target_is folds at codegen rather than trusting an
  // optimizer: a call to a symbol the target's libc lacks must never reach
  // the object file, even though the declaration may. Guarded per-OS code is
  // how the stdlib declares Darwin's __error next to glibc's
  // __errno_location.
  auto driver = Driver::createForAOT("darwin_fold_module",
                                     "arm64-apple-darwin");
  driver->compileString(R"(
    extern "C" function c_errno_linux() raw_ptr<i32> as "__errno_location";
    extern "C" function c_errno_darwin() raw_ptr<i32> as "__error";

    function read_errno() i32 {
        unsafe {
            if (_target_is("macos")) {
                return _load<i32>(c_errno_darwin(), 0);
            } else {
                return _load<i32>(c_errno_linux(), 0);
            }
        };
    }

    function main() i32 {
        return read_errno();
    }
  )");

  std::string ir;
  llvm::raw_string_ostream os(ir);
  driver->getModule().print(os, nullptr);

  EXPECT_NE(ir.find("call ptr @__error()"), std::string::npos) << ir;
  EXPECT_EQ(ir.find("call ptr @__errno_location()"), std::string::npos)
      << "the Linux branch is dead on a Darwin target and must emit no call";
}

TEST(Ffi_Abi_CrossTargetDarwin, futex_intrinsics_lower_to_ulock) {
  // macOS has no futex; the same Sun intrinsics must reach Darwin's
  // __ulock_wait/__ulock_wake instead of a Linux syscall number.
  auto driver = Driver::createForAOT("darwin_ulock_module",
                                     "arm64-apple-darwin");
  driver->compileString(R"(
    function main() i32 {
        var word: i32 = 1;
        unsafe {
            _futex_wait(_address_of<i32>(word), 0);
            _futex_wake(_address_of<i32>(word));
        };
        return 0;
    }
  )");

  std::string ir;
  llvm::raw_string_ostream os(ir);
  driver->getModule().print(os, nullptr);

  EXPECT_NE(ir.find("@__ulock_wait"), std::string::npos) << ir;
  EXPECT_NE(ir.find("@__ulock_wake"), std::string::npos) << ir;
  EXPECT_EQ(ir.find("@syscall"), std::string::npos)
      << "Darwin targets must not emit the Linux futex syscall";
}

TEST(Ffi_Abi_CrossTargetDarwin, file_open_uses_darwin_flag_values) {
  // Linux 0x241/0x441 decode to the wrong bits on Darwin (no truncation,
  // async I/O); the helper must bake in 0x601/0x209 instead.
  auto driver = Driver::createForAOT("darwin_open_module",
                                     "arm64-apple-darwin");
  driver->compileString(R"(
    function main() i32 {
        unsafe { __file_open("out.txt", 1); };
        return 0;
    }
  )");

  std::string ir;
  llvm::raw_string_ostream os(ir);
  driver->getModule().print(os, nullptr);

  EXPECT_NE(ir.find("i32 1537"), std::string::npos) << ir;  // 0x601
  EXPECT_NE(ir.find("i32 521"), std::string::npos) << ir;   // 0x209
}

TEST(Ffi_Abi_CrossTargetDarwin, sockaddr_gets_len_and_family_bytes) {
  // Darwin's sockaddr_in begins u8 sin_len, u8 sin_family; a 16-bit store of
  // AF_INET at offset 0 (the Linux shape) would leave the family AF_UNSPEC.
  auto driver = Driver::createForAOT("darwin_sockaddr_module",
                                     "arm64-apple-darwin");
  driver->compileString(R"(
    function main() i32 {
        unsafe { __bind_ipv4(0, 0, 8080); };
        return 0;
    }
  )");

  std::string ir;
  llvm::raw_string_ostream os(ir);
  driver->getModule().print(os, nullptr);

  EXPECT_NE(ir.find("store i8 16"), std::string::npos) << ir;  // sin_len
  EXPECT_NE(ir.find("store i8 2"), std::string::npos) << ir;   // AF_INET
  EXPECT_EQ(ir.find("store i16 2"), std::string::npos)
      << "the 16-bit Linux family store must not appear for Darwin";
}

TEST(Ffi_Abi_CrossTargetDarwin, emits_an_arm64_macho_object) {
  auto driver = Driver::createForAOT("darwin_obj_module", "arm64-apple-darwin");
  driver->compileString("function main() i32 { return 0; }");

  std::string path = ::testing::TempDir() + "sun_darwin_target_test.o";
  std::string errorMsg;
  ASSERT_TRUE(sun::emitObjectFile(driver->getModule(), path, errorMsg))
      << errorMsg;

  std::ifstream obj(path, std::ios::binary);
  ASSERT_TRUE(obj.good());
  unsigned char header[8] = {};
  obj.read(reinterpret_cast<char*>(header), sizeof(header));
  ASSERT_EQ(obj.gcount(), static_cast<std::streamsize>(sizeof(header)));

  // MH_MAGIC_64 (0xfeedfacf), little-endian on disk.
  EXPECT_EQ(header[0], 0xcfu);
  EXPECT_EQ(header[1], 0xfau);
  EXPECT_EQ(header[2], 0xedu);
  EXPECT_EQ(header[3], 0xfeu);
  // cputype at offset 4: CPU_TYPE_ARM64 == CPU_TYPE_ARM | CPU_ARCH_ABI64
  // == 0x0100000c.
  uint32_t cputype = static_cast<uint32_t>(header[4]) |
                     (static_cast<uint32_t>(header[5]) << 8) |
                     (static_cast<uint32_t>(header[6]) << 16) |
                     (static_cast<uint32_t>(header[7]) << 24);
  EXPECT_EQ(cputype, 0x0100000cu);
}

TEST(Ffi_Abi_CrossTargetDarwin, stdlib_program_compiles_to_a_macho_object) {
  // End to end through the Darwin stdlib bundle: a program leaning on the
  // per-OS stdlib pieces (errno, io, time) compiles against
  // build/arm64-apple-darwin/stdlib.moon and lands in a Mach-O object.
  auto stdlibMoon =
      std::filesystem::path("build/arm64-apple-darwin/stdlib.moon");
  if (!std::filesystem::exists(stdlibMoon)) {
    GTEST_SKIP() << "Darwin stdlib bundle not built";
  }

  auto driver =
      Driver::createForAOT("darwin_stdlib_module", "arm64-apple-darwin");
  driver->setMoonImports(
      {{std::filesystem::absolute(stdlibMoon).string(), {}}});
  driver->compileString(R"(
    using std;
    using std.io;
    using std.time;

    function main() i32 {
        var alloc = make_heap_allocator();
        var v: Vec<i64> = Vec<i64>(alloc, 4);
        v.push(read_unix_time());
        var t = now();
        if (exists("nowhere.txt")) {
            return 1;
        }
        return 0;
    }
  )");

  std::string path = ::testing::TempDir() + "sun_darwin_stdlib_test.o";
  std::string errorMsg;
  ASSERT_TRUE(sun::emitObjectFile(driver->getModule(), path, errorMsg))
      << errorMsg;

  std::ifstream obj(path, std::ios::binary);
  unsigned char header[4] = {};
  obj.read(reinterpret_cast<char*>(header), sizeof(header));
  EXPECT_EQ(header[0], 0xcfu);
  EXPECT_EQ(header[3], 0xfeu);
}

TEST(Ffi_Abi_CrossTargetDarwin, linking_from_linux_names_the_missing_sdk) {
  // Mach-O cross-linking needs Apple's SDK, which cannot ship on Linux; the
  // error should say to stop at the object and link on a Mac. On a Darwin
  // host this test would link for real, so it only asserts from elsewhere.
  if (llvm::Triple(llvm::sys::getDefaultTargetTriple()).isOSDarwin()) {
    GTEST_SKIP() << "host links Mach-O natively";
  }

  std::string linker = sun::linkerCommandFor("arm64-apple-darwin");
  EXPECT_TRUE(linker.empty());

  std::string errorMsg;
  sun::LinkOptions linkOpts;
  linkOpts.targetTriple = "arm64-apple-darwin";
  EXPECT_FALSE(sun::linkExecutable("in.o", "out", errorMsg, linkOpts));
  EXPECT_NE(errorMsg.find("Mac"), std::string::npos) << errorMsg;
}

TEST(Ffi_Abi_CrossTargetDarwin, static_linking_is_rejected_for_darwin) {
  std::string errorMsg;
  sun::LinkOptions linkOpts;
  linkOpts.targetTriple = "arm64-apple-darwin";
  linkOpts.staticLink = true;
  linkOpts.sysroot = "/nonexistent-sdk";
  EXPECT_FALSE(sun::linkExecutable("in.o", "out", errorMsg, linkOpts));
}
