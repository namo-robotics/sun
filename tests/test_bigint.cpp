// tests/test_bigint.cpp - stdlib/bigint.sun (BigUint) and the bit intrinsics
// it builds on (_mul_hi_u64, _ctlz_u64, _cttz_u64)

#include <gtest/gtest.h>

#include "execution_utils.h"

TEST(BitIntrinsicsTest, mul_hi_ctlz_cttz) {
  auto value = executeString(R"(
    function main() i32 {
        var one: u64 = 1;
        var all_ones: u64 = 0 - one;
        var top: u64 = one << 63;
        var two32: u64 = one << 32;
        // (2^64-1)^2 >> 64 == 2^64-2
        if (_mul_hi_u64(all_ones, all_ones) != all_ones - 1) { return 1; }
        if (_mul_hi_u64(top, 4) != 2) { return 2; }
        if (_mul_hi_u64(two32 * 4, two32) != 4) { return 3; }
        if (_mul_hi_u64(3, 4) != 0) { return 4; }
        if (_ctlz_u64(1) != 63) { return 5; }
        if (_ctlz_u64(top) != 0) { return 6; }
        if (_ctlz_u64(0) != 64) { return 7; }
        if (_cttz_u64(8) != 3) { return 8; }
        if (_cttz_u64(top) != 63) { return 9; }
        if (_cttz_u64(0) != 64) { return 10; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(BigUintTest, factorial_and_decimal_round_trip) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32, IError {
        var alloc = make_heap_allocator();
        var n = BigUint(alloc, 1);
        for (var i: u64 = 2; i <= 30; i = i + 1) { n.mul_small(i); }
        var text = String(alloc, "");
        n.append_decimal(text);
        if (text.equals_literal("265252859812191058636308480000000") == false) { return 1; }
        if (n.bit_length() != 108) { return 2; }
        if (n.limb_count() != 2) { return 3; }
        var back = BigUint(alloc);
        if (back.set_decimal(text) == false) { return 4; }
        if (back.equals(n) == false) { return 5; }
        var square = n.clone();
        square.mul(n);
        var sq_text = String(alloc, "");
        square.append_decimal(sq_text);
        if (sq_text.equals_literal("70359079638545882374689246780656119576032161719910400000000000000") == false) { return 6; }
        var bad = String(alloc, "12x");
        if (back.set_decimal(bad)) { return 7; }
        if (back.is_zero() == false) { return 8; }
        var zero = BigUint(alloc);
        var zt = String(alloc, "");
        zero.append_decimal(zt);
        if (zt.equals_literal("0") == false) { return 9; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(BigUintTest, multi_limb_arithmetic) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function of(alloc: ref HeapAllocator, text: static_ptr<u8>) BigUint {
        var s = String(alloc, text);
        var n = BigUint(alloc);
        n.set_decimal(s);
        return n;
    }

    function is(alloc: ref HeapAllocator, n: ref BigUint, text: static_ptr<u8>) bool {
        var s = String(alloc, "");
        n.append_decimal(s);
        return s.equals_literal(text);
    }

    function main() i32, IError {
        var alloc = make_heap_allocator();
        // a = 2^200 + 12345, b = 3^150 + 7
        var a = of(alloc, "1606938044258990275541962092341162602522202993782792835313721");
        var b = of(alloc, "369988485035126972924700782451696644186473100389722973815184405301748256");
        var sum = a.clone();
        sum.add(b);
        if (is(alloc, sum, "369988485036733910968959772727238606278814262992245176808967198137061977") == false) { return 1; }
        var diff = b.clone();
        diff.sub(a);
        if (is(alloc, diff, "369988485033520034880441792176154682094131937787200770821401612466434535") == false) { return 2; }
        var prod = a.clone();
        prod.mul(b);
        if (is(alloc, prod, "594548572540693628849860287507659082019984411858745114670193959411490296520390392243035028084963886526203367157948222586215524620576") == false) { return 3; }
        prod.shr(77);
        if (is(alloc, prod, "3934392419413913323894585204914305526281585972813365664542781201694709080391154784167284198954246014261865110") == false) { return 4; }
        var scaled = a.clone();
        scaled.mul_pow10(40);
        if (is(alloc, scaled, "16069380442589902755419620923411626025222029937827928353137210000000000000000000000000000000000000000") == false) { return 5; }
        if (a.compare(b) != -1 or b.compare(a) != 1 or a.compare(a) != 0) { return 6; }
        // Subtracting a larger number throws
        var small = a.clone();
        var threw: bool = false;
        try { small.sub(b); } catch (e: IError) { threw = true; }
        if (threw == false) { return 7; }
        // 2^100 via shift, back down via shift
        var p = BigUint(alloc, 1);
        p.shl(100);
        if (is(alloc, p, "1267650600228229401496703205376") == false) { return 8; }
        if (p.bit_length() != 101) { return 9; }
        p.shr(99);
        if (p.compare_u64(2) != 0) { return 10; }
        p.shr(5);
        if (p.is_zero() == false) { return 11; }
        // divmod peels digits from the bottom
        var d = of(alloc, "123456789012345678901234567890");
        var rem: u32 = d.divmod_small(1000);
        if (rem != 890) { return 12; }
        if (is(alloc, d, "123456789012345678901234567") == false) { return 13; }
        d.mul_small(1000);
        d.add_small(890);
        if (is(alloc, d, "123456789012345678901234567890") == false) { return 14; }
        // Carries across a limb boundary
        var c = BigUint(alloc, 0 - _convert<u64>(1));
        c.add_small(1);
        if (c.limb_count() != 2 or c.limb(0) != 0 or c.limb(1) != 1) { return 15; }
        var fits: bool = match c.to_u64() { Option.Some(v) => true, Option.None => false };
        if (fits) { return 16; }
        c.set_u64(7);
        var v: u64 = match c.to_u64() { Option.Some(x) => x, Option.None => 0 };
        if (v != 7) { return 17; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}
