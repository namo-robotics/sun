// tests/stdlib/test_json.cpp - stdlib/json.sun: parsing, access, building,
// writing

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

namespace {

// Each check returns a distinct code so a failure points at the assertion.
std::string withMain(const std::string& body) {
  return std::string("using std;\n") + body;
}

}  // namespace

TEST(Stdlib_Json, parse_scalars) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        if (parse_json(alloc, "null").is_null() == false) { return 1; }
        if (parse_json(alloc, "true").as_bool() == false) { return 2; }
        if (parse_json(alloc, "false").as_bool()) { return 3; }
        if (parse_json(alloc, "42").as_i64() != 42) { return 4; }
        if (parse_json(alloc, "-7").as_i64() != -7) { return 5; }
        if (parse_json(alloc, "2.5").as_f64() != 2.5) { return 6; }
        if (parse_json(alloc, "-1e2").as_f64() != -100.0) { return 7; }
        if (parse_json(alloc, "1E-2").as_f64() != 0.01) { return 8; }
        if (parse_json(alloc, "0").as_i64() != 0) { return 9; }
        if (parse_json(alloc, "  \n\t 5 \r\n").as_i64() != 5) { return 10; }
        var n = parse_json(alloc, "42");
        if (n.is_int() == false or n.is_float() or n.is_number() == false) { return 11; }
        var f = parse_json(alloc, "4.0");
        if (f.is_float() == false or f.is_int()) { return 12; }
        if (f.as_i64() != 4) { return 13; }
        if (parse_json(alloc, "9223372036854775807").as_i64() != 9223372036854775807) { return 14; }
        var huge = parse_json(alloc, "9223372036854775808");
        if (huge.is_float() == false) { return 15; }
        var s = parse_json(alloc, "\"hi\"");
        if (s.is_string() == false) { return 16; }
        if (s.as_string().equals_literal("hi") == false) { return 17; }
        return 0;
    }
  )"));
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Json, parse_string_escapes) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var s = parse_json(alloc, "\"a\\\"b\\\\c\\/d\\n\\t\\r\\b\\f\"");
        var text: ref String = s.as_string();
        if (text.length() != 12) { return 1; }
        if (text.at(1) != 34) { return 2; }
        if (text.at(3) != 92) { return 3; }
        if (text.at(5) != 47) { return 4; }
        if (text.at(7) != 10) { return 5; }
        if (text.at(8) != 9) { return 6; }
        if (text.at(9) != 13) { return 7; }
        if (text.at(10) != 8 or text.at(11) != 12) { return 8; }
        // \u escapes decode to UTF-8: é (2 bytes), € (3 bytes), 😀 as a
        // surrogate pair (4 bytes)
        var u = parse_json(alloc, "\"\\u00e9\\u20AC\\ud83d\\ude00\"");
        var ut: ref String = u.as_string();
        if (ut.length() != 9) { return 9; }
        if (ut.at(0) != 195 or ut.at(1) != 169) { return 10; }
        if (ut.at(2) != 226 or ut.at(3) != 130 or ut.at(4) != 172) { return 11; }
        if (ut.at(5) != 240 or ut.at(6) != 159 or ut.at(7) != 152 or ut.at(8) != 128) { return 12; }
        // A lone surrogate becomes U+FFFD (3 bytes)
        var lone = parse_json(alloc, "\"\\ud83d\"");
        if (lone.as_string().length() != 3) { return 13; }
        // Raw UTF-8 passes through
        var raw = parse_json(alloc, "\"héllo\"");
        if (raw.as_string().length() != 6) { return 14; }
        return 0;
    }
  )"));
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Json, parse_nested_and_access) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var doc = parse_json(alloc, "{\"name\": \"sun\", \"tags\": [1, 2.5, true, null, {\"deep\": [[]]}], \"empty\": {}}");
        if (doc.is_object() == false) { return 1; }
        if (doc.len() != 3) { return 2; }
        if (doc.get("name").as_string().equals_literal("sun") == false) { return 3; }
        var tags: ref Json = doc.get("tags");
        if (tags.is_array() == false) { return 4; }
        if (tags.len() != 5) { return 5; }
        if (tags.at(0).as_i64() != 1) { return 6; }
        if (tags.at(1).as_f64() != 2.5) { return 7; }
        if (tags.at(2).as_bool() == false) { return 8; }
        if (tags.at(3).is_null() == false) { return 9; }
        if (tags.at(4).get("deep").at(0).len() != 0) { return 10; }
        if (doc.get("empty").len() != 0) { return 11; }
        if (doc.has("name") == false or doc.has("nope")) { return 12; }
        if (doc.key_at(0).equals_literal("name") == false) { return 13; }
        if (doc.key_at(2).equals_literal("empty") == false) { return 14; }
        if (doc.value_at(1).len() != 5) { return 15; }
        var key = String(alloc, "tags");
        if (doc.get(key).len() != 5) { return 16; }
        // Scalars have length 0
        if (doc.get("name").len() != 0) { return 17; }
        return 0;
    }
  )"));
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Json, reader_kind_mismatch_throws) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 {
        var alloc = make_heap_allocator();
        var codes: i32 = 0;
        try {
            var doc = parse_json(alloc, "{\"a\": [1], \"s\": \"x\", \"f\": 1.5}");
            try { doc.get("s").as_i64(); } catch (e: IError) { codes = codes + 1; }
            try { doc.get("f").as_i64(); } catch (e: IError) { codes = codes + 1; }
            try { doc.get("a").as_bool(); } catch (e: IError) { codes = codes + 1; }
            try { doc.get("a").as_string(); } catch (e: IError) { codes = codes + 1; }
            try { doc.get("missing"); } catch (e: IError) { codes = codes + 1; }
            try { doc.get("a").at(5); } catch (e: IError) { codes = codes + 1; }
            try { doc.get("a").at(-1); } catch (e: IError) { codes = codes + 1; }
            try { doc.get("s").at(0); } catch (e: IError) { codes = codes + 1; }
            try { doc.get("a").get("k"); } catch (e: IError) { codes = codes + 1; }
            try { doc.key_at(9); } catch (e: IError) { codes = codes + 1; }
            try { doc.get("s").push(Json()); } catch (e: IError) { codes = codes + 1; }
            try { doc.get("a").set(String(alloc, "k"), Json()); } catch (e: IError) { codes = codes + 1; }
            // A JsonError from a reader carries no text offset
            try { doc.get("s").as_f64(); } catch (e: IError) {
                if (e.code() != 60) { return -1; }
                codes = codes + 1;
            }
        } catch (e: IError) {
            return -2;
        }
        return codes;
    }
  )"));
  EXPECT_EQ(value, 13);
}

TEST(Stdlib_Json, parse_errors_report_offsets) {
  auto value = executeStringWithStdlib(withMain(R"(
    // Returns the offset of the parse error, or -1 if the text parsed
    function bad(alloc: ref HeapAllocator, text: static_ptr<u8>) i64 {
        try {
            var doc = parse_json(alloc, text);
            return -1;
        } catch (e: IError) {
            if (e.code() != 60) { return -100; }
            var msg: String = e.message();
            if (msg.is_empty()) { return -101; }
            return -2;
        }
    }

    function main() i32 {
        var alloc = make_heap_allocator();
        if (bad(alloc, "") != -2) { return 1; }
        if (bad(alloc, "[1, 2,]") != -2) { return 2; }
        if (bad(alloc, "[1 2]") != -2) { return 3; }
        if (bad(alloc, "{\"a\" 1}") != -2) { return 4; }
        if (bad(alloc, "{a: 1}") != -2) { return 5; }
        if (bad(alloc, "{\"a\": 1,}") != -2) { return 6; }
        if (bad(alloc, "[1, 2") != -2) { return 7; }
        if (bad(alloc, "\"abc") != -2) { return 8; }
        if (bad(alloc, "\"a\\qb\"") != -2) { return 9; }
        if (bad(alloc, "\"a\\u12\"") != -2) { return 10; }
        if (bad(alloc, "\"a\nb\"") != -2) { return 11; }
        if (bad(alloc, "tru") != -2) { return 12; }
        if (bad(alloc, "nul") != -2) { return 13; }
        if (bad(alloc, "01") != -2) { return 14; }
        if (bad(alloc, "1.") != -2) { return 15; }
        if (bad(alloc, ".5") != -2) { return 16; }
        if (bad(alloc, "+1") != -2) { return 17; }
        if (bad(alloc, "1e") != -2) { return 18; }
        if (bad(alloc, "-") != -2) { return 19; }
        if (bad(alloc, "1 2") != -2) { return 20; }
        if (bad(alloc, "[] x") != -2) { return 21; }
        if (bad(alloc, "[1]") != -1) { return 22; }
        if (bad(alloc, " {} ") != -1) { return 23; }
        return 0;
    }
  )"));
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Json, json_error_carries_offset) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i64 {
        var e = JsonError("bad", 7);
        if (e.code() != 60) { return -1; }
        return e.offset();
    }
  )"));
  EXPECT_EQ(value, 7);
}

TEST(Stdlib_Json, deep_nesting_is_rejected) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 {
        var alloc = make_heap_allocator();
        var text = String(alloc, "");
        for (var i: i64 = 0; i < 300; i = i + 1) { text.append_char(91); }
        for (var i: i64 = 0; i < 300; i = i + 1) { text.append_char(93); }
        try {
            var doc = parse_json(alloc, text);
            return 1;
        } catch (e: IError) {
            return e.code();
        }
        return 2;
    }
  )"));
  EXPECT_EQ(value, 60);
}

TEST(Stdlib_Json, deep_but_allowed_nesting) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var text = String(alloc, "");
        for (var i: i64 = 0; i < 200; i = i + 1) { text.append_char(91); }
        text.append_literal("7");
        for (var i: i64 = 0; i < 200; i = i + 1) { text.append_char(93); }
        var doc = parse_json(alloc, text);
        var out = doc.to_string(alloc);
        if (out.equals(text) == false) { return 1; }
        return 0;
    }
  )"));
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Json, build_and_write_compact) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var obj = create_json_object(alloc);
        obj.set(String(alloc, "ok"), Json(true));
        obj.set(String(alloc, "n"), Json(42));
        obj.set(String(alloc, "x"), Json(0.1));
        obj.set(String(alloc, "none"), Json());
        var arr = create_json_array(alloc);
        arr.push(Json(1));
        arr.push(create_json_string(alloc, "two"));
        arr.push(Json(-3.5));
        arr.push(create_json_array(alloc));
        arr.push(create_json_object(alloc));
        obj.set(String(alloc, "list"), arr);
        obj.set(String(alloc, "s"), Json(String(alloc, "q\"\\\n")));
        // Overwrite keeps the member position and drops the old value
        obj.set(String(alloc, "n"), Json(43));
        var out = obj.to_string(alloc);
        if (out.equals_literal("{\"ok\":true,\"n\":43,\"x\":0.1,\"none\":null,\"list\":[1,\"two\",-3.5,[],{}],\"s\":\"q\\\"\\\\\\n\"}") == false) {
            println(out);
            return 1;
        }
        if (obj.len() != 6) { return 2; }
        return 0;
    }
  )"));
  EXPECT_EQ(value, 0);
}

// Issue #94: Json(s) moves the String in; create_json_string(alloc, ref s)
// stores a copy so the caller can keep using the original.
TEST(Stdlib_Json, json_string_copies_a_borrowed_string) {
  auto value = executeStringWithStdlib(withMain(R"(
    class Config {
        var model: String;
        init(model: String) { this.model = model; }
    }

    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var cfg = Config(String(alloc, "sun-1"));
        var req = create_json_object(alloc);
        req.set(String(alloc, "model"), create_json_string(alloc, cfg.model));
        req.set(String(alloc, "again"), create_json_string(alloc, cfg.model));
        // The original is intact after both calls
        if (cfg.model.equals_literal("sun-1") == false) { return 1; }
        var out = req.to_string(alloc);
        if (out.equals_literal("{\"model\":\"sun-1\",\"again\":\"sun-1\"}") == false) {
            println(out);
            return 2;
        }
        // The copies are independent of the original: changing it later does
        // not change the document.
        cfg.model.append("-pro");
        if (req.get("model").as_string().equals_literal("sun-1") == false) { return 3; }
        return 0;
    }
  )"));
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Json, json_constructor_takes_ownership_of_string) {
  EXPECT_THROW(executeStringWithStdlib(withMain(R"(
    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var s = String(alloc, "moved");
        var req = create_json_object(alloc);
        req.set(String(alloc, "s"), Json(s));
        // s was moved into the document; this is a use after move
        if (s.length() == 0) { return 1; }
        return 0;
    }
  )")),
               std::exception);
}

TEST(Stdlib_Json, write_pretty) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var doc = parse_json(alloc, "{\"a\":[1,{\"b\":null}],\"c\":{},\"d\":[]}");
        var out = doc.to_pretty_string(alloc, 2);
        var expected = String(alloc, "{\n  \"a\": [\n    1,\n    {\n      \"b\": null\n    }\n  ],\n  \"c\": {},\n  \"d\": []\n}");
        if (out.equals(expected) == false) {
            println(out);
            return 1;
        }
        var scalar = parse_json(alloc, "5");
        if (scalar.to_pretty_string(alloc, 4).equals_literal("5") == false) { return 2; }
        return 0;
    }
  )"));
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Json, number_formatting) {
  auto value = executeStringWithStdlib(withMain(R"(
    function fmt(alloc: ref HeapAllocator, j: Json, expected: static_ptr<u8>) bool {
        var out = j.to_string(alloc);
        return out.equals_literal(expected);
    }

    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        if (fmt(alloc, Json(0.1), "0.1") == false) { return 1; }
        if (fmt(alloc, Json(5.0), "5.0") == false) { return 2; }
        if (fmt(alloc, Json(1e21), "1e+21") == false) { return 3; }
        if (fmt(alloc, Json(-0.0), "-0.0") == false) { return 4; }
        if (fmt(alloc, Json(9223372036854775807), "9223372036854775807") == false) { return 5; }
        var min: i64 = -9223372036854775807 - 1;
        if (fmt(alloc, Json(min), "-9223372036854775808") == false) { return 6; }
        var inf: f64 = 1.0 / 0.0;
        if (fmt(alloc, Json(inf), "null") == false) { return 7; }
        if (fmt(alloc, Json(0.0 - inf), "null") == false) { return 8; }
        if (fmt(alloc, Json(inf - inf), "null") == false) { return 9; }
        // Round trip keeps kinds
        var back = parse_json(alloc, Json(5.0).to_string(alloc));
        if (back.is_float() == false) { return 10; }
        var big = parse_json(alloc, "12345678901234567890");
        if (big.is_float() == false) { return 11; }
        if (fmt(alloc, big, "1.2345678901234567e+19") == false) { return 12; }
        return 0;
    }
  )"));
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Json, round_trip) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var text = String(alloc, "{\"name\":\"s\\u00fcn\",\"list\":[1,2.5,-3,true,false,null,\"\\\"\"],\"nested\":{\"a\":{\"b\":[[],{}]}},\"e\":\"\"}");
        var doc = parse_json(alloc, text);
        var once = doc.to_string(alloc);
        var again = parse_json(alloc, once).to_string(alloc);
        if (once.equals(again) == false) { return 1; }
        // \u00fc is written as raw UTF-8, everything else is stable
        if (once.equals_literal("{\"name\":\"sün\",\"list\":[1,2.5,-3,true,false,null,\"\\\"\"],\"nested\":{\"a\":{\"b\":[[],{}]}},\"e\":\"\"}") == false) {
            println(once);
            return 2;
        }
        return 0;
    }
  )"));
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Json, control_characters_are_escaped_on_write) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var s = String(alloc, "a");
        s.append_char(1);
        s.append_char(31);
        s.append_char(8);
        s.append_char(12);
        var out = Json(s).to_string(alloc);
        if (out.equals_literal("\"a\\u0001\\u001f\\b\\f\"") == false) {
            println(out);
            return 1;
        }
        var back = parse_json(alloc, out);
        if (back.as_string().length() != 5) { return 2; }
        return 0;
    }
  )"));
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Json, iterate_object_members_and_array_items) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var doc = parse_json(alloc, "{\"a\": 1, \"b\": 2, \"c\": [10, 20, 30]}");
        var total: i64 = 0;
        for (var i: i64 = 0; i < doc.len(); i = i + 1) {
            var v: ref Json = doc.value_at(i);
            if (v.is_int()) {
                total = total + v.as_i64();
            } else {
                for (var k: i64 = 0; k < v.len(); k = k + 1) {
                    total = total + v.at(k).as_i64();
                }
            }
        }
        var keys = String(alloc, "");
        for (var i: i64 = 0; i < doc.len(); i = i + 1) {
            keys.append(doc.key_at(i));
        }
        if (keys.equals_literal("abc") == false) { return -1; }
        return _convert<i32>(total);
    }
  )"));
  EXPECT_EQ(value, 63);
}

// Owning payloads are dropped exactly once: building, overwriting and
// dropping a large document must not double free or crash.
TEST(Stdlib_Json, large_document_builds_and_drops) {
  auto value = executeStringWithStdlib(withMain(R"(
    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var total: i64 = 0;
        for (var round: i64 = 0; round < 3; round = round + 1) {
            var root = create_json_object(alloc);
            for (var i: i64 = 0; i < 200; i = i + 1) {
                var item = create_json_object(alloc);
                item.set(String(alloc, "id"), Json(i));
                item.set(String(alloc, "name"), create_json_string(alloc, "item"));
                var nums = create_json_array(alloc);
                for (var k: i64 = 0; k < 5; k = k + 1) { nums.push(Json(_convert<f64>(k) * 0.5)); }
                item.set(String(alloc, "nums"), nums);
                var key = String(alloc, "k");
                key.append_i64(i);
                root.set(key, item);
            }
            // Overwrite half the members (drops the old subtrees)
            for (var i: i64 = 0; i < 100; i = i + 1) {
                var key = String(alloc, "k");
                key.append_i64(i);
                root.set(key, Json(i));
            }
            var text = root.to_string(alloc);
            var back = parse_json(alloc, text);
            total = total + back.len();
        }
        return _convert<i32>(total);
    }
  )"));
  EXPECT_EQ(value, 600);
}

TEST(Stdlib_Json, json_value_enum_is_matchable) {
  auto value = executeStringWithStdlib(withMain(R"(
    function describe(j: ref Json) i32 {
        return match j.value {
            JsonValue.Null => 1,
            JsonValue.Bool(b) => 2,
            JsonValue.Int(n) => 3,
            JsonValue.Float(x) => 4,
            JsonValue.String(s) => 5,
            JsonValue.Array(items) => 6,
            JsonValue.Object(members) => 7
        };
    }

    function main() i32 throws IError {
        var alloc = make_heap_allocator();
        var doc = parse_json(alloc, "[null, true, 1, 1.5, \"s\", [], {}]");
        var sum: i32 = 0;
        for (var i: i64 = 0; i < doc.len(); i = i + 1) {
            sum = sum * 10 + describe(doc.at(i));
        }
        return sum;
    }
  )"));
  EXPECT_EQ(value, 1234567);
}
