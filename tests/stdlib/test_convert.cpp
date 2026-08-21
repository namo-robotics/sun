// tests/stdlib/test_convert.cpp - Tests for safe_convert<T>

#include <gtest/gtest.h>

#include <string>

#include "execution_utils.h"

// Runs `body` inside main with the stdlib loaded. `body` must return an i32:
// the converted value on success, or -1 if the conversion threw.
static sun::SunValue runConvert(const std::string& body) {
  return executeStringWithStdlib("using sun;\nfunction main() i32 {\n" + body +
                                 "\n}");
}

TEST(Stdlib_Convert, narrow_i64_to_i32_in_range) {
  auto value = runConvert(R"(
    var ms: i64 = 200;
    try { return safe_convert<i32>(ms); } catch (e: IError) { return -1; }
  )");
  EXPECT_EQ(value, 200);
}

TEST(Stdlib_Convert, both_type_arguments_may_be_written) {
  auto value = runConvert(R"(
    var ms: i64 = 200;
    try { return safe_convert<i32, i64>(ms); } catch (e: IError) { return -1; }
  )");
  EXPECT_EQ(value, 200);
}

TEST(Stdlib_Convert, narrow_i64_to_i32_out_of_range_throws) {
  auto value = runConvert(R"(
    var big: i64 = 4294967498;   // low 32 bits are 202, so _convert would give 202
    try { return safe_convert<i32>(big); } catch (e: IError) { return e.code(); }
  )");
  EXPECT_EQ(value, 9);
}

TEST(Stdlib_Convert, negative_to_unsigned_throws) {
  auto value = runConvert(R"(
    var neg: i64 = -1;
    try { var u: u64 = safe_convert<u64>(neg); return 0; } catch (e: IError) { return -1; }
  )");
  EXPECT_EQ(value, -1);
}

TEST(Stdlib_Convert, unsigned_max_to_signed_throws) {
  auto value = runConvert(R"(
    var umax: u64 = 0;
    umax = umax - 1;   // wraps to the largest u64; round-trips through i64 as -1
    try { var s: i64 = safe_convert<i64>(umax); return 0; } catch (e: IError) { return -1; }
  )");
  EXPECT_EQ(value, -1);
}

TEST(Stdlib_Convert, same_width_sign_change_in_range) {
  auto value = runConvert(R"(
    var s: i32 = 77;
    try { var u: u32 = safe_convert<u32>(s); return _convert<i32>(u); } catch (e: IError) { return -1; }
  )");
  EXPECT_EQ(value, 77);
}

TEST(Stdlib_Convert, narrow_to_i8_edges) {
  auto value = runConvert(R"(
    var lo: i64 = -128;
    var hi: i64 = 127;
    var over: i64 = 128;
    var sum: i32 = 0;
    try { sum = sum + _convert<i32>(safe_convert<i8>(lo)); } catch (e: IError) { return -1; }
    try { sum = sum + _convert<i32>(safe_convert<i8>(hi)); } catch (e: IError) { return -2; }
    try { var x: i8 = safe_convert<i8>(over); return -3; } catch (e: IError) { sum = sum + 1000; }
    return sum;   // -128 + 127 + 1000
  )");
  EXPECT_EQ(value, 999);
}

TEST(Stdlib_Convert, widening_always_succeeds) {
  auto value = runConvert(R"(
    var b: u8 = 250;
    try { var w: i64 = safe_convert<i64>(b); return _convert<i32>(w); } catch (e: IError) { return -1; }
  )");
  EXPECT_EQ(value, 250);
}

TEST(Stdlib_Convert, float_to_int_truncates_toward_zero) {
  auto value = runConvert(R"(
    var g: f64 = -7.9;
    try { return _convert<i32>(safe_convert<i8>(g)); } catch (e: IError) { return -1; }
  )");
  EXPECT_EQ(value, -7);
}

TEST(Stdlib_Convert, float_to_int_out_of_range_throws) {
  auto value = runConvert(R"(
    var f: f64 = 3000000000.0;
    var ok: i32 = 0;
    try { var t: i32 = safe_convert<i32>(f); return -1; } catch (e: IError) { ok = ok + 1; }
    var small: f64 = -1.0;
    try { var u: u8 = safe_convert<u8>(small); return -2; } catch (e: IError) { ok = ok + 1; }
    var top: f64 = 256.0;
    try { var u: u8 = safe_convert<u8>(top); return -3; } catch (e: IError) { ok = ok + 1; }
    var fits: f64 = 255.0;
    try { var u: u8 = safe_convert<u8>(fits); ok = ok + _convert<i32>(u); } catch (e: IError) { return -4; }
    return ok;
  )");
  EXPECT_EQ(value, 258);
}

TEST(Stdlib_Convert, nan_to_int_throws) {
  auto value = runConvert(R"(
    var z: f64 = 0.0;
    var nan: f64 = z / z;
    try { var t: i64 = safe_convert<i64>(nan); return 0; } catch (e: IError) { return -1; }
  )");
  EXPECT_EQ(value, -1);
}

TEST(Stdlib_Convert, f64_to_f32_overflow_throws) {
  auto value = runConvert(R"(
    var huge: f64 = 1.0e300;
    var ok: i32 = 0;
    try { var s: f32 = safe_convert<f32>(huge); return -1; } catch (e: IError) { ok = ok + 1; }
    var fine: f64 = 1.5;
    try { var s: f32 = safe_convert<f32>(fine); ok = ok + _convert<i32>(s * 2.0); } catch (e: IError) { return -2; }
    return ok;
  )");
  EXPECT_EQ(value, 4);
}

TEST(Stdlib_Convert, int_to_float_succeeds) {
  auto value = runConvert(R"(
    var n: i64 = 1234;
    try { var d: f64 = safe_convert<f64>(n); return _convert<i32>(d); } catch (e: IError) { return -1; }
  )");
  EXPECT_EQ(value, 1234);
}

TEST(Stdlib_Convert, error_message_is_readable) {
  auto value = runConvert(R"(
    var big: i64 = 4294967498;
    try { var t: i32 = safe_convert<i32>(big); return 0; }
    catch (e: IError) { var m: String = e.message(); return _convert<i32>(m.length()); }
  )");
  EXPECT_EQ(value, static_cast<int>(
                       std::string("value out of range for the target type")
                           .size()));
}
