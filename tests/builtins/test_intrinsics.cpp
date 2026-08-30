// tests/builtins/test_intrinsics.cpp - Tests for compiler intrinsics

#include <gtest/gtest.h>

#include "driver/execution_utils.h"

// ============================================================================
// _is<T> Intrinsic Tests - Type Trait Checks
// ============================================================================

TEST(Builtins_IsIntrinsic, is_integer_with_i32) {
  auto value = executeString(R"(
    function check(x: i32) bool {
        return _is<_Integer>(x);
    }
    function main() i32 {
        if (check(42)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_integer_with_i64) {
  auto value = executeString(R"(
    function check(x: i64) bool {
        return _is<_Integer>(x);
    }
    function main() i32 {
        if (check(42)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_integer_with_u8) {
  auto value = executeString(R"(
    function check(x: u8) bool {
        return _is<_Integer>(x);
    }
    function main() i32 {
        var x: u8 = 42;
        if (check(x)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_integer_with_f64_returns_false) {
  auto value = executeString(R"(
    function check(x: f64) bool {
        return _is<_Integer>(x);
    }
    function main() i32 {
        if (check(3.14)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Builtins_IsIntrinsic, is_signed_with_i32) {
  auto value = executeString(R"(
    function check(x: i32) bool {
        return _is<_Signed>(x);
    }
    function main() i32 {
        if (check(42)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_signed_with_u32_returns_false) {
  auto value = executeString(R"(
    function check(x: u32) bool {
        return _is<_Signed>(x);
    }
    function main() i32 {
        var x: u32 = 42;
        if (check(x)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Builtins_IsIntrinsic, is_unsigned_with_u64) {
  auto value = executeString(R"(
    function check(x: u64) bool {
        return _is<_Unsigned>(x);
    }
    function main() i32 {
        var x: u64 = 42;
        if (check(x)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_unsigned_with_i64_returns_false) {
  auto value = executeString(R"(
    function check(x: i64) bool {
        return _is<_Unsigned>(x);
    }
    function main() i32 {
        if (check(42)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Builtins_IsIntrinsic, is_float_with_f64) {
  auto value = executeString(R"(
    function check(x: f64) bool {
        return _is<_Float>(x);
    }
    function main() i32 {
        if (check(3.14)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_float_with_f32) {
  auto value = executeString(R"(
    function check(x: f32) bool {
        return _is<_Float>(x);
    }
    function main() i32 {
        var x: f32 = 3.14;
        if (check(x)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_float_with_i32_returns_false) {
  auto value = executeString(R"(
    function check(x: i32) bool {
        return _is<_Float>(x);
    }
    function main() i32 {
        if (check(42)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Builtins_IsIntrinsic, is_numeric_with_i32) {
  auto value = executeString(R"(
    function check(x: i32) bool {
        return _is<_Numeric>(x);
    }
    function main() i32 {
        if (check(42)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_numeric_with_f64) {
  auto value = executeString(R"(
    function check(x: f64) bool {
        return _is<_Numeric>(x);
    }
    function main() i32 {
        if (check(3.14)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_numeric_with_bool_returns_false) {
  auto value = executeString(R"(
    function check(x: bool) bool {
        return _is<_Numeric>(x);
    }
    function main() i32 {
        if (check(true)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Builtins_IsIntrinsic, is_primitive_with_i32) {
  auto value = executeString(R"(
    function check(x: i32) bool {
        return _is<_Primitive>(x);
    }
    function main() i32 {
        if (check(42)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_primitive_with_bool) {
  auto value = executeString(R"(
    function check(x: bool) bool {
        return _is<_Primitive>(x);
    }
    function main() i32 {
        if (check(true)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// _is<T> Intrinsic Tests - Concrete Type Checks
// ============================================================================

TEST(Builtins_IsIntrinsic, is_concrete_type_i32) {
  auto value = executeString(R"(
    function check(x: i32) bool {
        return _is<i32>(x);
    }
    function main() i32 {
        if (check(42)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_concrete_type_i64_with_i32_returns_false) {
  auto value = executeString(R"(
    function check(x: i32) bool {
        return _is<i64>(x);
    }
    function main() i32 {
        if (check(42)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Builtins_IsIntrinsic, is_concrete_type_bool) {
  auto value = executeString(R"(
    function check(x: bool) bool {
        return _is<bool>(x);
    }
    function main() i32 {
        if (check(true)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// _is<T> Intrinsic Tests - Class Type Checks
// ============================================================================

TEST(Builtins_IsIntrinsic, is_class_type) {
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        init(px: i32, py: i32) {
            this.x = px;
            this.y = py;
        }
    }
    
    function check(p: ref Point) bool {
        return _is<Point>(p);
    }
    
    function main() i32 {
        var p = Point(1, 2);
        if (check(p)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

// ============================================================================
// _is<T> Intrinsic Tests - Interface Checks
// ============================================================================

TEST(Builtins_IsIntrinsic, is_interface_implemented) {
  auto value = executeString(R"(
    interface Printable {
        method print() void;
    }
    
    class Value implements Printable {
        var data: i32;
        init(d: i32) {
            this.data = d;
        }
        method print() void {
            // no-op
        }
    }
    
    function check(v: ref Value) bool {
        return _is<Printable>(v);
    }
    
    function main() i32 {
        var v = Value(42);
        if (check(v)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_interface_not_implemented) {
  auto value = executeString(R"(
    interface Printable {
        method print() void;
    }
    
    class Value {
        var data: i32;
        init(d: i32) {
            this.data = d;
        }
    }
    
    function check(v: ref Value) bool {
        return _is<Printable>(v);
    }
    
    function main() i32 {
        var v = Value(42);
        if (check(v)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// _is<T> Intrinsic Tests - Generic Context
// ============================================================================

TEST(Builtins_IsIntrinsic, is_in_generic_function_with_integer) {
  auto value = executeString(R"(
    function isInteger<T>(x: T) bool {
        return _is<_Integer>(x);
    }
    
    function main() i32 {
        if (isInteger<i32>(42)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_in_generic_function_with_float) {
  auto value = executeString(R"(
    function isInteger<T>(x: T) bool {
        return _is<_Integer>(x);
    }
    
    function main() i32 {
        if (isInteger<f64>(3.14)) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Builtins_IsIntrinsic, is_in_generic_class) {
  auto value = executeString(R"(
    class Container<T> {
        var value: T;
        
        init(v: T) {
            this.value = v;
        }
        
        method isNumeric() bool {
            return _is<_Numeric>(this.value);
        }
    }
    
    function main() i32 {
        var c = Container<i32>(42);
        if (c.isNumeric()) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, is_in_generic_class_with_non_numeric) {
  auto value = executeString(R"(
    class Container<T> {
        var value: T;
        
        init(v: T) {
            this.value = v;
        }
        
        method isNumeric() bool {
            return _is<_Numeric>(this.value);
        }
    }
    
    function main() i32 {
        var c = Container<bool>(true);
        if (c.isNumeric()) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

// ============================================================================
// _is<T> Intrinsic Tests - Branching Based on Type
// ============================================================================

TEST(Builtins_IsIntrinsic, branch_on_type_in_generic) {
  auto value = executeString(R"(
    function get_value<T>(x: T) i32 {
        if (_is<_Integer>(x)) {
            return 1;
        }
        if (_is<_Float>(x)) {
            return 2;
        }
        return 0;
    }
    
    function main() i32 {
        var a = get_value<i32>(42);
        var b = get_value<f64>(3.14);
        return a * 10 + b;
    }
  )");
  EXPECT_EQ(value, 12);  // 1*10 + 2
}

// ============================================================================
// _is<T> Type Narrowing Tests
// ============================================================================

TEST(Builtins_IsIntrinsic, minimal_narrowing) {
  auto value = executeString(R"(
    interface IFoo {
        method foo() i64;
    }
    
    class Foo implements IFoo {
        method foo() i64 {
            return 42;
        }
    }
    
    
    function main() i32 {
        var f = Foo();
        if (_is<IFoo>(f)) { return f.foo(); }
        return 0;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Builtins_IsIntrinsic, type_narrowing_with_interface) {
  auto value = executeString(R"(
    interface IHashable {
        method hash() i64;
    }
    
    class MyKey implements IHashable {
        var id: i64;
        
        init(x: i64) {
            this.id = x;
        }
        
        method hash() i64 {
            return this.id * 31;
        }
    }
    
    function getHash<T>(key: ref T) i64 {
        if (_is<IHashable>(key)) {
            return key.hash();
        }
        return 0;
    }
    
    function main() i32 {
        var k = MyKey(42);
        var h = getHash<MyKey>(k);
        if (h == 1302) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_IsIntrinsic, type_narrowing_method_call_in_generic) {
  auto value = executeString(R"(
    interface IValue {
        method get_value() i32;
    }
    
    class Box implements IValue {
        var val: i32;
        
        init(v: i32) {
            this.val = v;
        }
        
        method get_value() i32 {
            return this.val;
        }
    }
    
    function extractValue<T>(obj: ref T) i32 {
        if (_is<IValue>(obj)) {
            return obj.get_value();
        }
        return -1;
    }
    
    function main() i32 {
        var b = Box(99);
        return extractValue<Box>(b);
    }
  )");
  EXPECT_EQ(value, 99);
}

TEST(Builtins_IsIntrinsic, type_narrowing_else_branch_no_narrow) {
  // Verify else branch doesn't have narrowed type (compile-time check)
  // This just ensures analysis handles else correctly
  auto value = executeString(R"(
    interface IFoo {
        method foo() i32;
    }
    
    class Bar implements IFoo {
        init() {}
        method foo() i32 { return 42; }
    }
    
    function check<T>(x: ref T) i32 {
        if (_is<IFoo>(x)) {
            return x.foo();
        }
        return 0;
    }
    
    function main() i32 {
        var b = Bar();
        return check<Bar>(b);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Builtins_IsIntrinsic, type_narrowing_nested_if) {
  auto value = executeString(R"(
    interface IA {
        method a() i32;
    }
    
    interface IB {
        method b() i32;
    }
    
    class Both implements IA, IB {
        init() {}
        method a() i32 { return 10; }
        method b() i32 { return 20; }
    }
    
    function combine<T>(x: ref T) i32 {
        var result: i32 = 0;
        if (_is<IA>(x)) {
            result = result + x.a();
            if (_is<IB>(x)) {
                result = result + x.b();
            }
        }
        return result;
    }
    
    function main() i32 {
        var obj = Both();
        return combine<Both>(obj);
    }
  )");
  EXPECT_EQ(value, 30);
}

TEST(Builtins_IsIntrinsic, nested_generic_functions) {
  // Minimal: outer<T> calls inner<T>
  auto value = executeString(R"(
    function inner<T>(x: T) T { return x; }
    
    function outer<T>(x: T) T { return inner<T>(x); }
    
    function main() i32 { return outer<i32>(42); }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Builtins_IsIntrinsic, nested_generic_classes) {
  // Minimal: Outer<T> contains Inner<T>
  auto value = executeString(R"(

    public module Test {
        public class Inner<T> {
            public var val: T;
            init(v: T) { this.val = v; }
            public method get() T { return this.val; }
        }
    }
    
    class Outer<T> {
        public var inner: Test.Inner<T>;
        init(v: T) { this.inner = Test.Inner<T>(v); }
        public method get() T { return this.inner.get(); }
    }
    
    function main() i32 { var o = Outer<i32>(42); return o.get(); }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Builtins_IsIntrinsic, generic_function_with_captured_param) {
  // Generic function uses its parameter inside type narrowing block
  // The parameter is captured for use with interface method call
  auto value = executeString(R"(
    interface IMultiplier {
        method multiply(x: i32) i32;
    }
    
    class Doubler implements IMultiplier {
        init() {}
        method multiply(x: i32) i32 { return x * 2; }
    }
    
    function applyMultiplier<T>(m: ref T, value: i32) i32 {
        if (_is<IMultiplier>(m)) {
            // 'value' is captured from outer scope into the narrowed block
            return m.multiply(value);
        }
        return value;
    }
    
    function main() i32 {
        var d = Doubler();
        return applyMultiplier<Doubler>(d, 21);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Builtins_IsIntrinsic, generic_function_captures_and_type_narrowing) {
  // Generic function with captured variable and type narrowing
  auto value = executeString(R"(
    interface IScalable {
        method scale(factor: i32) i32;
    }
    
    class Number implements IScalable {
        var n: i32;
        init(v: i32) { this.n = v; }
        method scale(factor: i32) i32 { return this.n * factor; }
    }
    
    function processWithFactor<T>(x: ref T, factor: i32) i32 {
        // Capture factor for use inside type-narrowed block
        if (_is<IScalable>(x)) {
            return x.scale(factor);
        }
        return 0;
    }
    
    function main() i32 {
        var num = Number(7);
        return processWithFactor<Number>(num, 6);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Builtins_IsIntrinsic, chained_generic_calls_with_different_types) {
  // Multiple generic functions with different type parameters
  auto value = executeString(R"(
    interface IA {
        method getA() i32;
    }
    
    interface IB {
        method getB() i32;
    }
    
    class TypeA implements IA {
        var a: i32;
        init(v: i32) { this.a = v; }
        method getA() i32 { return this.a; }
    }
    
    class TypeB implements IB {
        var b: i32;
        init(v: i32) { this.b = v; }
        method getB() i32 { return this.b; }
    }
    
    function extractA<T>(x: ref T) i32 {
        if (_is<IA>(x)) {
            return x.getA();
        }
        return 0;
    }
    
    function extractB<U>(y: ref U) i32 {
        if (_is<IB>(y)) {
            return y.getB();
        }
        return 0;
    }
    
    function combine<T, U>(a: ref T, b: ref U) i32 {
        return extractA<T>(a) + extractB<U>(b);
    }
    
    function main() i32 {
        var ta = TypeA(20);
        var tb = TypeB(22);
        return combine<TypeA, TypeB>(ta, tb);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Builtins_IsIntrinsic, nested_generic_function_definition) {
  // Generic function defined inside another generic function
  auto value = executeString(R"(
    interface IValue {
        method get_value() i32;
    }
    
    class Wrapper implements IValue {
        var data: i32;
        init(d: i32) { this.data = d; }
        method get_value() i32 { return this.data; }
    }
    
    function outer<T>(x: ref T, multiplier: i32) i32 {
        // Define a generic function inside another generic function
        function inner<U>(y: ref U) i32 {
            if (_is<IValue>(y)) {
                return y.get_value();
            }
            return 0;
        }
        
        // Call the inner generic function with the outer's type parameter
        return inner<T>(x) * multiplier;
    }
    
    function main() i32 {
        var w = Wrapper(7);
        return outer<Wrapper>(w, 6);
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// _print_i64 — full 64-bit output
// ============================================================================
// _print_i64 used to sign-truncate to i32 and delegate to the i32 helper, so
// any value outside the i32 range printed as garbage.

static std::string capturePrintedOutput(const std::string& source) {
  testing::internal::CaptureStdout();
  executeString(source);
  return testing::internal::GetCapturedStdout();
}

TEST(Builtins_PrintI64, prints_value_above_i32_range) {
  EXPECT_EQ(capturePrintedOutput(R"(
    function main() i32 {
        var big: i64 = 5000000000;
        _print_i64(big);
        return 0;
    }
  )"),
            "5000000000");
}

TEST(Builtins_PrintI64, prints_negative_value_below_i32_range) {
  EXPECT_EQ(capturePrintedOutput(R"(
    function main() i32 {
        var big: i64 = 5000000000;
        _print_i64(0 - big);
        return 0;
    }
  )"),
            "-5000000000");
}

TEST(Builtins_PrintI64, prints_int64_max) {
  EXPECT_EQ(capturePrintedOutput(R"(
    function main() i32 {
        var v: i64 = 9223372036854775807;
        _print_i64(v);
        return 0;
    }
  )"),
            "9223372036854775807");
}

TEST(Builtins_PrintI64, prints_int64_min) {
  // Negating INT64_MIN overflows back to itself; digits are extracted with
  // unsigned div/rem so the magnitude still comes out right.
  EXPECT_EQ(capturePrintedOutput(R"(
    function main() i32 {
        var v: i64 = 9223372036854775807;
        _print_i64(0 - v - 1);
        return 0;
    }
  )"),
            "-9223372036854775808");
}

TEST(Builtins_PrintI64, prints_zero) {
  EXPECT_EQ(capturePrintedOutput(R"(
    function main() i32 {
        var v: i64 = 0;
        _print_i64(v);
        return 0;
    }
  )"),
            "0");
}

TEST(Builtins_PrintI64, prints_small_values_unchanged) {
  EXPECT_EQ(capturePrintedOutput(R"(
    function main() i32 {
        var v: i64 = 42;
        _print_i64(v);
        _print_i64(0 - v);
        return 0;
    }
  )"),
            "42-42");
}

// ============================================================================
// _target_is Intrinsic Tests - Compile-Time Target Checks
// ============================================================================

TEST(Builtins_TargetIsIntrinsic, host_jit_matches_exactly_one_os) {
  // The JIT compiles for the host, so exactly one of the known names holds.
  auto value = executeString(R"(
    function main() i32 {
        var count: i32 = 0;
        if (_target_is("linux")) { count = count + 1; }
        if (_target_is("macos")) { count = count + 1; }
        if (_target_is("windows")) { count = count + 1; }
        return count;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST(Builtins_TargetIsIntrinsic, folded_if_keeps_only_the_live_branch) {
  // "windows" is known but never the test host, so the else side must run.
  auto value = executeString(R"(
    function main() i32 {
        if (_target_is("windows")) {
            return 1;
        } else {
            return 42;
        }
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Builtins_TargetIsIntrinsic, folds_in_a_module_level_initializer) {
  // A module-level var initializer must be a constant; the ternary over a
  // folded condition has to collapse with no control flow at all.
  auto value = executeString(R"(
    var PICKED: i32 = _target_is("windows") ? 1 : 42;
    function main() i32 {
        return PICKED;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Builtins_TargetIsIntrinsic, unknown_target_name_is_an_error) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
        if (_target_is("maos")) { return 1; }
        return 0;
    }
  )"),
               SunError);
}

TEST(Builtins_TargetIsIntrinsic, non_literal_argument_is_an_error) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
        var name = "macos";
        if (_target_is(name)) { return 1; }
        return 0;
    }
  )"),
               SunError);
}

// ============================================================================
// _bitcast<T> Intrinsic Tests - Same-Size Reinterpretation
// ============================================================================

TEST(Builtins_BitcastIntrinsic, round_trips_a_float_through_its_bits) {
  auto value = executeString(R"(
    function main() i32 {
        var bits: u64 = _bitcast<u64>(1.0);
        var back: f64 = _bitcast<f64>(bits);
        if (back == 1.0) { return 42; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Builtins_BitcastIntrinsic, retypes_a_raw_pointer_and_back) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 42;
        var p: raw_ptr<i32> = _address_of<i32>(x);
        var bytes: raw_ptr<u8> = _bitcast<raw_ptr<u8>>(p);
        var back: raw_ptr<i32> = _bitcast<raw_ptr<i32>>(bytes);
        unsafe { return _load<i32>(back, 0); };
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(Builtins_BitcastIntrinsic, reads_a_class_back_out_of_a_byte_pointer) {
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        init(a: i32, b: i32) { this.x = a; this.y = b; }
    }
    function main() i32 {
        var p = Point(3, 4);
        var bytes = _bitcast<raw_ptr<u8>>(_address_of<Point>(p));
        var back = _bitcast<raw_ptr<Point>>(bytes);
        var r: ref Point = unsafe { _to_ref<Point>(back); };
        return r.x * 10 + r.y;
    }
  )");
  EXPECT_EQ(value, 34);
}

TEST(Builtins_BitcastIntrinsic, pointer_to_number_is_an_error) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
        var x: i32 = 1;
        var n: u64 = _bitcast<u64>(_address_of<i32>(x));
        return 0;
    }
  )"),
               SunError);
}

TEST(Builtins_BitcastIntrinsic, number_to_pointer_is_an_error) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
        var n: u64 = 4096;
        var p = _bitcast<raw_ptr<u8>>(n);
        return 0;
    }
  )"),
               SunError);
}

TEST(Builtins_BitcastIntrinsic, a_class_target_is_an_error) {
  EXPECT_THROW(executeString(R"(
    class Point { var x: i32; init(a: i32) { this.x = a; } }
    function main() i32 {
        var n: i64 = 1;
        var p = _bitcast<Point>(n);
        return 0;
    }
  )"),
               SunError);
}

// ============================================================================
// _malloc Intrinsic Tests
// ============================================================================

// malloc takes a 64-bit size, but an untyped integer literal is i32, so the
// argument has to be widened before the call.
TEST(Builtins_MallocIntrinsic, integer_literal_size) {
  auto value = executeString(R"(
    function main() i32 {
        var mem = unsafe { _malloc(1024); };
        unsafe { _store_i64(mem, 0, 21); };
        var v: i64 = unsafe { _load_i64(mem, 0); };
        unsafe { _free(mem); };
        return _convert<i32>(v);
    }
  )");
  EXPECT_EQ(value, 21);
}

TEST(Builtins_MallocIntrinsic, i64_variable_size) {
  auto value = executeString(R"(
    function main() i32 {
        var size: i64 = 16;
        var mem = unsafe { _malloc(size); };
        unsafe { _store_i64(mem, 0, 22); };
        var v: i64 = unsafe { _load_i64(mem, 0); };
        unsafe { _free(mem); };
        return _convert<i32>(v);
    }
  )");
  EXPECT_EQ(value, 22);
}

// ============================================================================
// Unsafe Block Requirement
// ============================================================================

// An intrinsic that reads or writes unchecked memory is rejected outside an
// unsafe block, generic and non-generic alike.
TEST(Builtins_UnsafeRequirement, generic_load_outside_unsafe_is_an_error) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
        var n: i64 = 8;
        var mem = unsafe { _malloc(n); };
        var v: i32 = _load<i32>(mem, 0);
        unsafe { _free(mem); };
        return v;
    }
  )"),
               SunError);
}

TEST(Builtins_UnsafeRequirement, generic_store_outside_unsafe_is_an_error) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
        var n: i64 = 8;
        var mem = unsafe { _malloc(n); };
        _store<i32>(mem, 0, 1);
        return 0;
    }
  )"),
               SunError);
}

TEST(Builtins_UnsafeRequirement, to_ref_outside_unsafe_is_an_error) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
        var x: i32 = 1;
        var p: raw_ptr<i32> = _address_of<i32>(x);
        var r: ref i32 = _to_ref<i32>(p);
        return r;
    }
  )"),
               SunError);
}

TEST(Builtins_UnsafeRequirement, ptr_offset_outside_unsafe_is_an_error) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
        var n: i64 = 8;
        var mem = unsafe { _malloc(n); };
        var p = _ptr_offset(mem, 4);
        return 0;
    }
  )"),
               SunError);
}

// Intrinsics that only compute stay usable anywhere.
TEST(Builtins_UnsafeRequirement, computing_intrinsics_need_no_block) {
  auto value = executeString(R"(
    function main() i32 {
        var x: i32 = 7;
        var size: i64 = _sizeof<i32>();
        var p: raw_ptr<i32> = _address_of<i32>(x);
        var ok: bool = _is<_Integer>(x);
        if (not ok) { return 0; }
        return _convert<i32>(size) + x;
    }
  )");
  EXPECT_EQ(value, 11);
}

// The block is lexical: a safe wrapper's body is checked on its own, so
// calling it from outside a block is fine.
TEST(Builtins_UnsafeRequirement, safe_wrapper_is_callable_without_a_block) {
  auto value = executeString(R"(
    function read_first(p: raw_ptr<i32>) i32 {
        return unsafe { _load<i32>(p, 0); };
    }
    function main() i32 {
        var x: i32 = 9;
        return read_first(_address_of<i32>(x));
    }
  )");
  EXPECT_EQ(value, 9);
}

// ============================================================================
// Unsafe Block Scoping
// ============================================================================

// The body is a scope like any other block body: names declared inside stay
// inside.
TEST(Builtins_UnsafeScope, name_declared_inside_does_not_escape) {
  EXPECT_THROW(executeString(R"(
    function main() i32 {
        unsafe { var inner: i32 = 7; };
        return inner;
    }
  )"),
               std::exception);
}

// But the block's value does leave, and ownership leaves with it — the value
// is not dropped on the way out.
TEST(Builtins_UnsafeScope, owning_value_survives_the_block) {
  auto value = executeString(R"(
    var drops: i32 = 0;

    class Owner {
      var p: raw_ptr<i8>;
      init() { var n: i64 = 8; this.p = unsafe { _malloc(n); }; }
      deinit() { unsafe { _free(this.p); }; drops = drops + 1; }
      method tag() i32 { return 5; }
    }

    function make() Owner { return Owner(); }

    function use() i32 {
      var o = unsafe { make(); };
      return o.tag();     // still alive here
    }

    function main() i32 {
      var t: i32 = use();
      return t + drops;   // 5 + dropped exactly once
    }
  )");
  EXPECT_EQ(value, 6);
}

// A discarded block value behaves exactly like any other unconsumed call
// temporary: it lives to the end of the enclosing scope and is dropped there
// exactly once — not leaked, not dropped twice, not dropped early.
TEST(Builtins_UnsafeScope, discarded_value_drops_once_at_scope_end) {
  auto value = executeString(R"(
    var drops: i32 = 0;

    class Owner {
      var p: raw_ptr<i8>;
      init() { var n: i64 = 8; this.p = unsafe { _malloc(n); }; }
      deinit() { unsafe { _free(this.p); }; drops = drops + 1; }
    }

    function make() Owner { return Owner(); }

    function discard_block() void { unsafe { make(); }; }
    function discard_bare() void { make(); }

    function main() i32 {
      discard_block();
      var a: i32 = drops;   // 1: dropped when discard_block's scope ended
      drops = 0;
      discard_bare();
      return a * 10 + drops;   // same timing as a bare discarded call
    }
  )");
  EXPECT_EQ(value, 11);
}

// ============================================================================
// No Implicit Returns
// ============================================================================

// A function whose signature promises a value must leave through an explicit
// return (or throw) on every path; the trailing expression is not a return.
TEST(Builtins_NoImplicitReturns, trailing_expression_is_not_a_return) {
  EXPECT_THROW(executeString(R"(
    function f() i32 { 42; }
    function main() i32 { return f(); }
  )"),
               SunError);
}

TEST(Builtins_NoImplicitReturns, if_without_else_can_fall_through) {
  EXPECT_THROW(executeString(R"(
    function f(c: bool) i32 {
        if (c) { return 1; }
    }
    function main() i32 { return f(true); }
  )"),
               SunError);
}

TEST(Builtins_NoImplicitReturns, if_else_returning_on_both_paths_is_enough) {
  auto value = executeString(R"(
    function f(c: bool) i32 {
        if (c) { return 1; } else { return 2; }
    }
    function main() i32 { return f(false); }
  )");
  EXPECT_EQ(value, 2);
}

TEST(Builtins_NoImplicitReturns, exhaustive_match_returning_everywhere) {
  auto value = executeString(R"(
    enum Kind { A, B }
    function f(k: Kind) i32 {
        match k {
            Kind.A => { return 1; },
            Kind.B => { return 2; }
        };
    }
    function main() i32 { return f(Kind.B); }
  )");
  EXPECT_EQ(value, 2);
}

// Binding a block that always leaves the function is rejected: the block
// never produces a value, so the binding would be dead code.
TEST(Builtins_NoImplicitReturns, binding_an_always_exiting_block_is_an_error) {
  EXPECT_THROW(executeString(R"(
    function f() i32 {
        var x = unsafe { return 0; };
        return 1;
    }
    function main() i32 { return f(); }
  )"),
               SunError);
}

// A conditional early exit inside the block is fine: on the other path the
// block still produces its value.
TEST(Builtins_NoImplicitReturns, conditional_exit_inside_bound_block_is_fine) {
  auto value = executeString(R"(
    extern "C" function c_abs(x: i32) i32 as "abs";
    function f(c: bool) i32 {
        var x = unsafe {
            if (c) { return 0; }
            c_abs(-7);
        };
        return x;
    }
    function main() i32 { return f(false); }
  )");
  EXPECT_EQ(value, 7);
}
