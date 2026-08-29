// tests/classes/test_packed.cpp - Tests for packed_class layout
//
// A packed_class lowers to an LLVM packed struct: no padding between fields,
// struct alignment 1. These tests pin down the layout guarantee (via
// _sizeof<T>), that field reads/writes still round-trip through the unaligned
// offsets, and the restrictions that keep an unaligned field address from
// escaping.

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "driver/driver.h"
#include "driver/execution_utils.h"

// ============================================================================
// Layout
// ============================================================================

TEST(Classes_Packed, removes_trailing_and_interior_padding) {
  auto value = executeString(R"(
    packed_class P {
        var a: u8;
        var b: i32;
        var c: u8;
        init() {
            this.a = 0;
            this.b = 0;
            this.c = 0;
        }
    }
    function main() i64 {
        return _sizeof<P>();
    }
  )");
  EXPECT_EQ(value, 6);  // 1 + 4 + 1, no padding
}

TEST(Classes_Packed, unpacked_equivalent_still_pads) {
  auto value = executeString(R"(
    class U {
        var a: u8;
        var b: i32;
        var c: u8;
        init() {
            this.a = 0;
            this.b = 0;
            this.c = 0;
        }
    }
    function main() i64 {
        return _sizeof<U>();
    }
  )");
  EXPECT_EQ(value, 12);  // a, pad(3), b, c, pad(3)
}

TEST(Classes_Packed, all_primitive_widths) {
  auto value = executeString(R"(
    packed_class Wide {
        var a: u8;
        var b: i16;
        var c: i32;
        var d: i64;
        var e: f32;
        var f: f64;
        init() {
            this.a = 0;
            this.b = 0;
            this.c = 0;
            this.d = 0;
            this.e = 0.0;
            this.f = 0.0;
        }
    }
    function main() i64 {
        return _sizeof<Wide>();
    }
  )");
  EXPECT_EQ(value, 27);  // 1 + 2 + 4 + 8 + 4 + 8
}

TEST(Classes_Packed, single_field_matches_unpacked) {
  auto value = executeString(R"(
    packed_class One {
        var a: i32;
        init() {
            this.a = 0;
        }
    }
    function main() i64 {
        return _sizeof<One>();
    }
  )");
  EXPECT_EQ(value, 4);
}

// ============================================================================
// Field access round-trips through unaligned offsets
// ============================================================================

TEST(Classes_Packed, field_write_read_roundtrip) {
  auto value = executeString(R"(
    packed_class P {
        var a: u8;
        var b: i32;
        var c: u8;
        init() {
            this.a = 0;
            this.b = 0;
            this.c = 0;
        }
    }
    function main() i32 {
        var p: P = P();
        p.a = 7;
        p.b = 1000000;
        p.c = 9;
        // Each field must survive the others being written around it
        if (p.a != 7) { return 1; }
        if (p.b != 1000000) { return 2; }
        if (p.c != 9) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Classes_Packed, compound_assignment_on_unaligned_field) {
  auto value = executeString(R"(
    packed_class P {
        var a: u8;
        var b: i32;
        init() {
            this.a = 0;
            this.b = 0;
        }
    }
    function main() i32 {
        var p: P = P();
        p.b = 100;
        p.b += 23;
        p.b *= 2;
        return p.b;
    }
  )");
  EXPECT_EQ(value, 246);
}

TEST(Classes_Packed, constructor_initializes_unaligned_fields) {
  auto value = executeString(R"(
    packed_class P {
        var a: u8;
        var b: i32;
        init(y: i32) {
            this.a = 3;
            this.b = y;
        }
    }
    function main() i32 {
        var p: P = P(40000);
        return p.b + p.a;
    }
  )");
  EXPECT_EQ(value, 40003);
}

TEST(Classes_Packed, methods_read_own_unaligned_fields) {
  auto value = executeString(R"(
    packed_class P {
        var a: u8;
        var b: i32;
        init() {
            this.a = 0;
            this.b = 0;
        }
        function sum() i32 {
            return this.b + this.a;
        }
    }
    function main() i32 {
        var p: P = P();
        p.a = 5;
        p.b = 70000;
        return p.sum();
    }
  )");
  EXPECT_EQ(value, 70005);
}

TEST(Classes_Packed, passed_by_ref_whole_object) {
  // The object itself may still be borrowed - only its fields may not
  auto value = executeString(R"(
    packed_class P {
        var a: u8;
        var b: i32;
        init() {
            this.a = 0;
            this.b = 0;
        }
    }
    function bump(p: ref P) void {
        p.b = p.b + 1;
    }
    function main() i32 {
        var p: P = P();
        p.b = 41;
        bump(p);
        return p.b;
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Nesting
// ============================================================================

TEST(Classes_Packed, nested_packed_class_field) {
  auto value = executeString(R"(
    packed_class Inner {
        var x: u8;
        var y: i32;
        init() {
            this.x = 0;
            this.y = 0;
        }
    }
    packed_class Outer {
        var tag: u8;
        var inner: Inner;
        init() {
            this.tag = 0;
            this.inner = Inner();
        }
    }
    function main() i64 {
        return _sizeof<Outer>();
    }
  )");
  EXPECT_EQ(value, 6);  // 1 + (1 + 4), exact sum
}

TEST(Classes_Packed, nested_packed_field_roundtrip) {
  auto value = executeString(R"(
    packed_class Inner {
        var x: u8;
        var y: i32;
        init() {
            this.x = 0;
            this.y = 0;
        }
    }
    packed_class Outer {
        var tag: u8;
        var inner: Inner;
        init() {
            this.tag = 0;
            this.inner = Inner();
        }
    }
    function main() i32 {
        var o: Outer = Outer();
        o.tag = 1;
        o.inner.x = 2;
        o.inner.y = 300000;
        if (o.tag != 1) { return 1; }
        if (o.inner.x != 2) { return 2; }
        if (o.inner.y != 300000) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Classes_Packed, packed_field_inside_unpacked_class_is_allowed) {
  auto value = executeString(R"(
    packed_class Inner {
        var x: u8;
        var y: i32;
        init() {
            this.x = 0;
            this.y = 0;
        }
    }
    class Outer {
        var tag: u8;
        var inner: Inner;
        init() {
            this.tag = 0;
            this.inner = Inner();
        }
    }
    function main() i64 {
        return _sizeof<Inner>();
    }
  )");
  EXPECT_EQ(value, 5);
}

// ============================================================================
// Generics
// ============================================================================

TEST(Classes_Packed, generic_specialization_stays_packed) {
  auto value = executeString(R"(
    packed_class Box<T> {
        var tag: u8;
        var value: T;
        init(value: T) {
            this.tag = 0;
            this.value = value;
        }
    }
    function main() i64 {
        return _sizeof<Box<i32>>();
    }
  )");
  EXPECT_EQ(value, 5);  // 1 + 4, no padding
}

TEST(Classes_Packed, generic_specializations_differ_by_type_arg) {
  auto value = executeString(R"(
    packed_class Box<T> {
        var tag: u8;
        var value: T;
        init(value: T) {
            this.tag = 0;
            this.value = value;
        }
    }
    function main() i64 {
        return _sizeof<Box<i64>>() * 100 + _sizeof<Box<u8>>();
    }
  )");
  EXPECT_EQ(value, 902);  // (1+8)=9 -> 900, (1+1)=2
}

// ============================================================================
// Restrictions
// ============================================================================

TEST(Classes_Packed, cannot_create_ref_to_packed_field) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    packed_class P {
        var a: u8;
        var b: i32;
        init() {
            this.a = 0;
            this.b = 0;
        }
    }
    function main() i32 {
        var p: P = P();
        ref r = p.b;
        return r;
    }
  )"),
                                "not aligned enough to be borrowed");
}

TEST(Classes_Packed, cannot_pass_packed_field_to_ref_param) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    packed_class P {
        var a: u8;
        var b: i32;
        init() {
            this.a = 0;
            this.b = 0;
        }
    }
    function bump(x: ref i32) void { x = x + 1; }
    function main() i32 {
        var p: P = P();
        bump(p.b);
        return p.b;
    }
  )"),
                                "not aligned enough to be borrowed");
}

TEST(Classes_Packed, cannot_ref_field_reached_through_packed_owner) {
  // `inner` is packed too, but the diagnostic must fire for the whole chain
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    packed_class Inner {
        var x: i32;
        init() {
            this.x = 0;
        }
    }
    packed_class Outer {
        var tag: u8;
        var inner: Inner;
        init() {
            this.tag = 0;
            this.inner = Inner();
        }
    }
    function main() i32 {
        var o: Outer = Outer();
        ref r = o.inner.x;
        return r;
    }
  )"),
                                "not aligned enough to be borrowed");
}

TEST(Classes_Packed, rejects_array_field) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    packed_class P {
        var a: u8;
        var data: array<i32>;
        init() {}
    }
    function main() i32 { return 0; }
  )"),
                                "Arrays are fat pointers and cannot be packed");
}

TEST(Classes_Packed, rejects_non_packed_class_field) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    class Q {
        var x: i32;
        init() {
            this.x = 0;
        }
    }
    packed_class P {
        var a: u8;
        var q: Q;
        init() {
            this.a = 0;
            this.q = Q();
        }
    }
    function main() i32 { return 0; }
  )"),
                                "has non-packed class type");
}

TEST(Classes_Packed, rejects_partial_combined_with_packed) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(executeString(R"(
    partial packed_class P {
        function f() i32 { return 0; }
    }
    function main() i32 { return 0; }
  )"),
                                "cannot be combined");
}

// ============================================================================
// Emitted IR
// ============================================================================

namespace {
// Compile `source` in debug mode and return the generated LLVM IR.
std::string irFor(const std::string& source) {
  initTestEnvironment();
  std::string debugName = "test_packed_" + std::to_string(getpid());
  auto driver = Driver::createForJIT();
  driver->setDebugMode(true, debugName);
  driver->executeString(source);

  std::string folder = debugName + "_debug";
  std::ifstream in(folder + "/ir.ll");
  std::stringstream buffer;
  buffer << in.rdbuf();
  in.close();
  if (std::filesystem::exists(folder)) std::filesystem::remove_all(folder);
  return buffer.str();
}
}  // namespace

// Without explicit alignment, IRBuilder tags these with the field type's ABI
// alignment (align 4 for an i32 at offset 1) - a false claim the optimizer may
// exploit into a miscompile on x86 or a fault on strict-alignment targets.
TEST(Classes_Packed, field_accesses_are_emitted_with_align_1) {
  std::string ir = irFor(R"(
    packed_class P {
        var a: u8;
        var b: i32;
        init() {
            this.a = 0;
            this.b = 0;
        }
    }
    function main() i32 {
        var p: P = P();
        p.b = 1234;
        p.b += 1;
        return p.b;
    }
  )");

  ASSERT_FALSE(ir.empty()) << "expected debug IR to be generated";
  EXPECT_NE(ir.find("store i32 1234, ptr %b.ptr, align 1"), std::string::npos)
      << ir;
  EXPECT_EQ(ir.find("align 4"), std::string::npos)
      << "no access to a packed field may claim natural alignment\n"
      << ir;
}

TEST(Classes_Packed, unpacked_field_accesses_keep_natural_alignment) {
  std::string ir = irFor(R"(
    class U {
        var a: u8;
        var b: i32;
        init() {
            this.a = 0;
            this.b = 0;
        }
    }
    function main() i32 {
        var u: U = U();
        u.b = 1234;
        return u.b;
    }
  )");

  ASSERT_FALSE(ir.empty()) << "expected debug IR to be generated";
  EXPECT_NE(ir.find("align 4"), std::string::npos)
      << "packing must not weaken alignment for ordinary classes\n"
      << ir;
}

// ============================================================================
// The keyword must not steal `packed` as an identifier
// ============================================================================

TEST(Classes_Packed, packed_remains_a_usable_identifier) {
  auto value = executeString(R"(
    function main() i32 {
        var packed: i32 = 40;
        var packed_value: i32 = 2;
        return packed + packed_value;
    }
  )");
  EXPECT_EQ(value, 42);
}
