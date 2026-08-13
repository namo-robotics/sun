// tests/test_sysv_abi.cpp — System V AMD64 argument classification
//
// The expectations here are clang's actual output for the equivalent C
// declarations on x86-64 Linux, e.g.
//
//   struct S16 { int a; long b; };  void t16(struct S16);
//     -> declare void @t16(i32, i64)
//
// If one of these ever disagrees with clang, the calls Sun emits to C are
// wrong, so they are written as exact type comparisons rather than
// approximate shape checks.

#include <gtest/gtest.h>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <string>

#include "sysv_abi.h"

namespace {

class SysVABITest : public ::testing::Test {
 protected:
  llvm::LLVMContext ctx;
  // Standard x86-64 Linux layout, so the test does not depend on the host.
  llvm::DataLayout dl{"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-"
                      "i128:128-f80:128-n8:16:32:64-S128"};

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
};

}  // namespace

// --- scalars pass through untouched -----------------------------------------

TEST_F(SysVABITest, scalars_are_direct) {
  for (llvm::Type* t : {i8(), i32(), i64(), f32(), f64(), ptr()}) {
    auto lowering = sun::sysv::lowerArgument(t, dl);
    EXPECT_TRUE(lowering.isDirect()) << "type should not be rewritten";
    EXPECT_EQ(lowering.type, t);
  }
}

TEST_F(SysVABITest, void_return_is_direct) {
  EXPECT_TRUE(sun::sysv::lowerReturn(llvm::Type::getVoidTy(ctx), dl).isDirect());
}

// --- register-class aggregates ----------------------------------------------

TEST_F(SysVABITest, two_ints_coerce_to_one_i64) {
  // struct { int, int } -> void t8(i64)
  auto lowering = sun::sysv::lowerArgument(structOf({i32(), i32()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], i64());
}

TEST_F(SysVABITest, int_then_long_splits_into_i32_and_i64) {
  // struct { int, long } -> void t16(i32, i64)
  // The first eightbyte is half padding, so it narrows to i32.
  auto lowering = sun::sysv::lowerArgument(structOf({i32(), i64()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 2u);
  EXPECT_EQ(lowering.pieces[0], i32());
  EXPECT_EQ(lowering.pieces[1], i64());
}

TEST_F(SysVABITest, two_doubles_stay_in_sse_registers) {
  // struct { double, double } -> void tf16(double, double)
  auto lowering = sun::sysv::lowerArgument(structOf({f64(), f64()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 2u);
  EXPECT_EQ(lowering.pieces[0], f64());
  EXPECT_EQ(lowering.pieces[1], f64());
}

TEST_F(SysVABITest, mixed_int_and_double_uses_one_of_each) {
  // struct { int, double } -> void tmix(i32, double)
  auto lowering = sun::sysv::lowerArgument(structOf({i32(), f64()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 2u);
  EXPECT_EQ(lowering.pieces[0], i32());
  EXPECT_EQ(lowering.pieces[1], f64());
}

TEST_F(SysVABITest, two_floats_pack_into_one_sse_eightbyte) {
  // struct { float, float } -> void tff(<2 x float>)
  // Both floats share one SSE eightbyte; clang represents that as a vector,
  // not as a double.
  auto lowering = sun::sysv::lowerArgument(structOf({f32(), f32()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], llvm::FixedVectorType::get(f32(), 2));
}

TEST_F(SysVABITest, single_float_stays_a_float) {
  // struct { float } -> void tf1(float)
  auto lowering = sun::sysv::lowerArgument(structOf({f32()}), dl);
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], f32());
}

TEST_F(SysVABITest, three_floats_split_into_a_vector_and_a_float) {
  // struct { float, float, float } -> void tf3(<2 x float>, float)
  auto lowering = sun::sysv::lowerArgument(structOf({f32(), f32(), f32()}), dl);
  ASSERT_EQ(lowering.pieces.size(), 2u);
  EXPECT_EQ(lowering.pieces[0], llvm::FixedVectorType::get(f32(), 2));
  EXPECT_EQ(lowering.pieces[1], f32());
}

TEST_F(SysVABITest, float_beside_an_int_is_integer_class) {
  // struct { float, int } -> void tfi(i64): a mixed eightbyte is INTEGER.
  auto lowering = sun::sysv::lowerArgument(structOf({f32(), i32()}), dl);
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], i64());
}

TEST_F(SysVABITest, float_then_double_uses_separate_eightbytes) {
  // struct { float, double } -> void tfd(float, double)
  auto lowering = sun::sysv::lowerArgument(structOf({f32(), f64()}), dl);
  ASSERT_EQ(lowering.pieces.size(), 2u);
  EXPECT_EQ(lowering.pieces[0], f32());
  EXPECT_EQ(lowering.pieces[1], f64());
}

TEST_F(SysVABITest, single_char_coerces_to_i8) {
  // struct { char } -> void t1(i8)
  auto lowering = sun::sysv::lowerArgument(structOf({i8()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], i8());
}

TEST_F(SysVABITest, integer_width_is_exact_not_rounded_up) {
  // clang emits the exact occupied width: 3 bytes is i24, not i32.
  auto bytes = [&](unsigned n) {
    return sun::sysv::lowerArgument(
               structOf({llvm::ArrayType::get(i8(), n)}), dl)
        .pieces[0];
  };
  EXPECT_EQ(sun::sysv::lowerArgument(structOf({i16()}), dl).pieces[0], i16());
  EXPECT_EQ(bytes(1), llvm::IntegerType::get(ctx, 8));
  EXPECT_EQ(bytes(3), llvm::IntegerType::get(ctx, 24));
  EXPECT_EQ(bytes(5), llvm::IntegerType::get(ctx, 40));
  EXPECT_EQ(bytes(6), llvm::IntegerType::get(ctx, 48));
  EXPECT_EQ(bytes(7), llvm::IntegerType::get(ctx, 56));
  EXPECT_EQ(bytes(8), llvm::IntegerType::get(ctx, 64));
}

TEST_F(SysVABITest, pointer_field_is_integer_class) {
  auto lowering = sun::sysv::lowerArgument(structOf({ptr()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  EXPECT_EQ(lowering.pieces[0], i64());
}

TEST_F(SysVABITest, nested_struct_is_flattened_before_classifying) {
  // struct { struct { int, int } } is classified as if the fields were
  // spelled inline: one INTEGER eightbyte.
  auto inner = structOf({i32(), i32()});
  auto lowering = sun::sysv::lowerArgument(structOf({inner}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], i64());
}

TEST_F(SysVABITest, small_array_member_is_flattened_too) {
  auto lowering =
      sun::sysv::lowerArgument(structOf({llvm::ArrayType::get(i32(), 2)}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], i64());
}

// --- memory-class aggregates ------------------------------------------------

TEST_F(SysVABITest, aggregate_over_16_bytes_goes_through_memory) {
  // struct { int a,b,c,d,e; } (20 bytes) -> byval pointer
  auto big = structOf({i32(), i32(), i32(), i32(), i32()});
  auto lowering = sun::sysv::lowerArgument(big, dl);
  ASSERT_TRUE(lowering.isIndirect());
  EXPECT_EQ(lowering.type, big);
  EXPECT_EQ(lowering.align, 4u);
}

TEST_F(SysVABITest, exactly_16_bytes_still_fits_in_registers) {
  auto lowering = sun::sysv::lowerArgument(structOf({i64(), i64()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  EXPECT_EQ(lowering.pieces.size(), 2u);
}

TEST_F(SysVABITest, seventeen_bytes_does_not) {
  auto lowering =
      sun::sysv::lowerArgument(structOf({i64(), i64(), i8()}), dl);
  EXPECT_TRUE(lowering.isIndirect());
}

TEST_F(SysVABITest, field_straddling_an_eightbyte_boundary_goes_to_memory) {
  // A packed { i8, i64 } puts the i64 at offset 1, crossing the boundary.
  auto packed = llvm::StructType::get(ctx, {i8(), i64()}, /*isPacked=*/true);
  EXPECT_TRUE(sun::sysv::lowerArgument(packed, dl).isIndirect());
}

// --- returns ----------------------------------------------------------------

TEST_F(SysVABITest, small_struct_return_is_coerced_not_indirect) {
  // struct S8 r8(void) -> i64 @r8()
  auto lowering = sun::sysv::lowerReturn(structOf({i32(), i32()}), dl);
  ASSERT_TRUE(lowering.isCoerced());
  ASSERT_EQ(lowering.pieces.size(), 1u);
  EXPECT_EQ(lowering.pieces[0], i64());
}

TEST_F(SysVABITest, large_struct_return_uses_sret) {
  auto big = structOf({i32(), i32(), i32(), i32(), i32()});
  auto lowering = sun::sysv::lowerReturn(big, dl);
  ASSERT_TRUE(lowering.isIndirect());
  EXPECT_EQ(lowering.type, big);
}

// --- assembled function types -----------------------------------------------

TEST_F(SysVABITest, lowered_type_expands_coerced_params_in_place) {
  // void f(int, struct{int,long}, int) -> void f(i32, i32, i64, i32)
  llvm::Type* params[] = {i32(), structOf({i32(), i64()}), i32()};
  auto lowering =
      sun::sysv::lowerCSignature(llvm::Type::getVoidTy(ctx), params, dl);
  auto* fnTy = sun::sysv::buildLoweredFunctionType(lowering, ctx, false);

  ASSERT_EQ(fnTy->getNumParams(), 4u);
  EXPECT_EQ(fnTy->getParamType(0), i32());
  EXPECT_EQ(fnTy->getParamType(1), i32());
  EXPECT_EQ(fnTy->getParamType(2), i64());
  EXPECT_EQ(fnTy->getParamType(3), i32());
  EXPECT_TRUE(fnTy->getReturnType()->isVoidTy());
}

TEST_F(SysVABITest, lowered_type_prepends_the_sret_pointer) {
  // struct S20 f(int) -> void f(ptr sret, i32)
  auto big = structOf({i32(), i32(), i32(), i32(), i32()});
  llvm::Type* params[] = {i32()};
  auto lowering = sun::sysv::lowerCSignature(big, params, dl);
  ASSERT_TRUE(lowering.usesSret());

  auto* fnTy = sun::sysv::buildLoweredFunctionType(lowering, ctx, false);
  ASSERT_EQ(fnTy->getNumParams(), 2u);
  EXPECT_TRUE(fnTy->getParamType(0)->isPointerTy());
  EXPECT_EQ(fnTy->getParamType(1), i32());
  EXPECT_TRUE(fnTy->getReturnType()->isVoidTy());
}

TEST_F(SysVABITest, two_piece_return_becomes_an_anonymous_struct) {
  // struct S16 r16(void) -> { i32, i64 } @r16()
  auto lowering = sun::sysv::lowerCSignature(structOf({i32(), i64()}), {}, dl);
  auto* fnTy = sun::sysv::buildLoweredFunctionType(lowering, ctx, false);
  auto* retTy = llvm::dyn_cast<llvm::StructType>(fnTy->getReturnType());
  ASSERT_NE(retTy, nullptr);
  ASSERT_EQ(retTy->getNumElements(), 2u);
  EXPECT_EQ(retTy->getElementType(0), i32());
  EXPECT_EQ(retTy->getElementType(1), i64());
}

TEST_F(SysVABITest, indirect_param_becomes_a_pointer) {
  auto big = structOf({i32(), i32(), i32(), i32(), i32()});
  llvm::Type* params[] = {big};
  auto lowering =
      sun::sysv::lowerCSignature(llvm::Type::getVoidTy(ctx), params, dl);
  auto* fnTy = sun::sysv::buildLoweredFunctionType(lowering, ctx, false);
  ASSERT_EQ(fnTy->getNumParams(), 1u);
  EXPECT_TRUE(fnTy->getParamType(0)->isPointerTy());
}

TEST_F(SysVABITest, all_scalar_signature_is_trivial) {
  llvm::Type* params[] = {i32(), f64(), ptr()};
  EXPECT_TRUE(sun::sysv::lowerCSignature(i32(), params, dl).isTrivial());
}

TEST_F(SysVABITest, signature_with_an_aggregate_is_not_trivial) {
  llvm::Type* params[] = {structOf({i32(), i32()})};
  EXPECT_FALSE(
      sun::sysv::lowerCSignature(llvm::Type::getVoidTy(ctx), params, dl)
          .isTrivial());
}
