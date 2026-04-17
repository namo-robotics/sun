// tests/test_intrinsics.cpp - Tests for compiler intrinsics

#include <gtest/gtest.h>

#include "execution_utils.h"

// ============================================================================
// _is<T> Intrinsic Tests - Type Trait Checks
// ============================================================================

TEST(IsIntrinsicTest, is_integer_with_i32) {
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

TEST(IsIntrinsicTest, is_integer_with_i64) {
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

TEST(IsIntrinsicTest, is_integer_with_u8) {
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

TEST(IsIntrinsicTest, is_integer_with_f64_returns_false) {
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

TEST(IsIntrinsicTest, is_signed_with_i32) {
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

TEST(IsIntrinsicTest, is_signed_with_u32_returns_false) {
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

TEST(IsIntrinsicTest, is_unsigned_with_u64) {
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

TEST(IsIntrinsicTest, is_unsigned_with_i64_returns_false) {
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

TEST(IsIntrinsicTest, is_float_with_f64) {
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

TEST(IsIntrinsicTest, is_float_with_f32) {
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

TEST(IsIntrinsicTest, is_float_with_i32_returns_false) {
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

TEST(IsIntrinsicTest, is_numeric_with_i32) {
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

TEST(IsIntrinsicTest, is_numeric_with_f64) {
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

TEST(IsIntrinsicTest, is_numeric_with_bool_returns_false) {
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

TEST(IsIntrinsicTest, is_primitive_with_i32) {
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

TEST(IsIntrinsicTest, is_primitive_with_bool) {
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

TEST(IsIntrinsicTest, is_concrete_type_i32) {
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

TEST(IsIntrinsicTest, is_concrete_type_i64_with_i32_returns_false) {
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

TEST(IsIntrinsicTest, is_concrete_type_bool) {
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

TEST(IsIntrinsicTest, is_class_type) {
  auto value = executeString(R"(
    class Point {
        var x: i32;
        var y: i32;
        function init(px: i32, py: i32) {
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

TEST(IsIntrinsicTest, is_interface_implemented) {
  auto value = executeString(R"(
    interface Printable {
        function print() void;
    }
    
    class Value implements Printable {
        var data: i32;
        function init(d: i32) {
            this.data = d;
        }
        function print() void {
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

TEST(IsIntrinsicTest, is_interface_not_implemented) {
  auto value = executeString(R"(
    interface Printable {
        function print() void;
    }
    
    class Value {
        var data: i32;
        function init(d: i32) {
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

TEST(IsIntrinsicTest, is_in_generic_function_with_integer) {
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

TEST(IsIntrinsicTest, is_in_generic_function_with_float) {
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

TEST(IsIntrinsicTest, is_in_generic_class) {
  auto value = executeString(R"(
    class Container<T> {
        var value: T;
        
        function init(v: T) {
            this.value = v;
        }
        
        function isNumeric() bool {
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

TEST(IsIntrinsicTest, is_in_generic_class_with_non_numeric) {
  auto value = executeString(R"(
    class Container<T> {
        var value: T;
        
        function init(v: T) {
            this.value = v;
        }
        
        function isNumeric() bool {
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

TEST(IsIntrinsicTest, branch_on_type_in_generic) {
  auto value = executeString(R"(
    function getValue<T>(x: T) i32 {
        if (_is<_Integer>(x)) {
            return 1;
        }
        if (_is<_Float>(x)) {
            return 2;
        }
        return 0;
    }
    
    function main() i32 {
        var a = getValue<i32>(42);
        var b = getValue<f64>(3.14);
        return a * 10 + b;
    }
  )");
  EXPECT_EQ(value, 12);  // 1*10 + 2
}

// ============================================================================
// _is<T> Type Narrowing Tests
// ============================================================================

TEST(IsIntrinsicTest, minimal_narrowing) {
  auto value = executeString(R"(
    interface IFoo {
        function foo() i64;
    }
    
    class Foo implements IFoo {
        function foo() i64 {
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

TEST(IsIntrinsicTest, type_narrowing_with_interface) {
  auto value = executeString(R"(
    interface IHashable {
        function hash() i64;
    }
    
    class MyKey implements IHashable {
        var id: i64;
        
        function init(x: i64) {
            this.id = x;
        }
        
        function hash() i64 {
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

TEST(IsIntrinsicTest, type_narrowing_method_call_in_generic) {
  auto value = executeString(R"(
    interface IValue {
        function getValue() i32;
    }
    
    class Box implements IValue {
        var val: i32;
        
        function init(v: i32) {
            this.val = v;
        }
        
        function getValue() i32 {
            return this.val;
        }
    }
    
    function extractValue<T>(obj: ref T) i32 {
        if (_is<IValue>(obj)) {
            return obj.getValue();
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

TEST(IsIntrinsicTest, type_narrowing_else_branch_no_narrow) {
  // Verify else branch doesn't have narrowed type (compile-time check)
  // This just ensures analysis handles else correctly
  auto value = executeString(R"(
    interface IFoo {
        function foo() i32;
    }
    
    class Bar implements IFoo {
        function init() {}
        function foo() i32 { return 42; }
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

TEST(IsIntrinsicTest, type_narrowing_nested_if) {
  auto value = executeString(R"(
    interface IA {
        function a() i32;
    }
    
    interface IB {
        function b() i32;
    }
    
    class Both implements IA, IB {
        function init() {}
        function a() i32 { return 10; }
        function b() i32 { return 20; }
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

TEST(IsIntrinsicTest, nested_generic_functions) {
  // Generic function calling another generic function
  auto value = executeString(R"(
    interface IValue {
        function getValue() i32;
    }
    
    class Container implements IValue {
        var data: i32;
        function init(d: i32) { this.data = d; }
        function getValue() i32 { return this.data; }
    }
    
    function inner<T>(x: ref T) i32 {
        if (_is<IValue>(x)) {
            return x.getValue();
        }
        return 0;
    }
    
    function outer<T>(x: ref T) i32 {
        // Call another generic function with the same type parameter
        return inner<T>(x) * 2;
    }
    
    function main() i32 {
        var c = Container(21);
        return outer<Container>(c);
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST(IsIntrinsicTest, generic_function_with_captured_param) {
  // Generic function uses its parameter inside type narrowing block
  // The parameter is captured for use with interface method call
  auto value = executeString(R"(
    interface IMultiplier {
        function multiply(x: i32) i32;
    }
    
    class Doubler implements IMultiplier {
        function init() {}
        function multiply(x: i32) i32 { return x * 2; }
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

TEST(IsIntrinsicTest, generic_function_captures_and_type_narrowing) {
  // Generic function with captured variable and type narrowing
  auto value = executeString(R"(
    interface IScalable {
        function scale(factor: i32) i32;
    }
    
    class Number implements IScalable {
        var n: i32;
        function init(v: i32) { this.n = v; }
        function scale(factor: i32) i32 { return this.n * factor; }
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

TEST(IsIntrinsicTest, chained_generic_calls_with_different_types) {
  // Multiple generic functions with different type parameters
  auto value = executeString(R"(
    interface IA {
        function getA() i32;
    }
    
    interface IB {
        function getB() i32;
    }
    
    class TypeA implements IA {
        var a: i32;
        function init(v: i32) { this.a = v; }
        function getA() i32 { return this.a; }
    }
    
    class TypeB implements IB {
        var b: i32;
        function init(v: i32) { this.b = v; }
        function getB() i32 { return this.b; }
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

TEST(IsIntrinsicTest, nested_generic_function_definition) {
  // Generic function defined inside another generic function
  auto value = executeString(R"(
    interface IValue {
        function getValue() i32;
    }
    
    class Wrapper implements IValue {
        var data: i32;
        function init(d: i32) { this.data = d; }
        function getValue() i32 { return this.data; }
    }
    
    function outer<T>(x: ref T, multiplier: i32) i32 {
        // Define a generic function inside another generic function
        function inner<U>(y: ref U) i32 {
            if (_is<IValue>(y)) {
                return y.getValue();
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
