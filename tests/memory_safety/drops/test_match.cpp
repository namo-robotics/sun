// Match results and payloads transfer ownership without early or repeated
// drops.
#include <gtest/gtest.h>

#include "driver/execution_utils.h"

namespace {
const char* preamble = R"(
  var drops: i32 = 0;
  class Res {
    public var id: i32;
    init(id: i32) { this.id = id; }
    deinit() { drops = drops + 1; }
  }
  enum Maybe { Some(Res), None }
  enum Triple { Left(Res, Res, Res), Right(Res, Res, Res) }
)";

std::string program(const std::string& body) {
  return std::string(preamble) + body;
}
}  // namespace

TEST(MemorySafety_Drops_Match, constructed_result_survives_arm) {
  EXPECT_EQ(executeString(program(R"(
    function run(flag: bool) void {
      var r = match flag { true => Res(1), _ => Res(2) };
      if (drops != 0) { drops = 100; }
    }
    function main() i32 { run(true); return drops; }
  )")),
            1);
}

TEST(MemorySafety_Drops_Match, enum_constructed_result_survives_arm) {
  EXPECT_EQ(executeString(program(R"(
    function run() void {
      var r = match Maybe.None { Maybe.Some(x) => Res(1), Maybe.None => Res(2) };
      if (drops != 0 or r.id != 2) { drops = 100; }
    }
    function main() i32 { run(); return drops; }
  )")),
            1);
}

TEST(MemorySafety_Drops_Match, directly_returned_payload_and_fallback) {
  EXPECT_EQ(executeString(program(R"(
    function take(o: Maybe) Res {
      return match o { Maybe.Some(r) => r, Maybe.None => Res(99) };
    }
    function run() void {
      var a = take(Maybe.Some(Res(1)));
      var b = take(Maybe.None);
      if (drops != 0 or a.id != 1 or b.id != 99) { drops = 100; }
    }
    function main() i32 { run(); return drops; }
  )")),
            2);
}

TEST(MemorySafety_Drops_Match, three_payloads_follow_selected_arm) {
  EXPECT_EQ(executeString(program(R"(
    function run(t: Triple) void {
      var r = match t {
        Triple.Left(a, b, c) => b,
        Triple.Right(a, b, c) => c
      };
      if (drops != 2 or r.id != 2) { drops = 100; }
    }
    function main() i32 {
      run(Triple.Left(Res(1), Res(2), Res(3)));
      var first = drops;
      drops = 0;
      run(Triple.Right(Res(1), Res(3), Res(2)));
      return first * 10 + drops;
    }
  )")),
            33);
}

TEST(MemorySafety_Drops_Match, ignored_payloads_and_whole_variant_drop) {
  EXPECT_EQ(executeString(program(R"(
    function take(t: Triple) Res {
      return match t { Triple.Left(_, b, _) => b, _ => Res(4) };
    }
    function run() void {
      var a = take(Triple.Left(Res(1), Res(2), Res(3)));
      var b = take(Triple.Right(Res(1), Res(2), Res(3)));
      if (drops != 5) { drops = 100; }
    }
    function main() i32 { run(); return drops; }
  )")),
            7);
}

TEST(MemorySafety_Drops_Match, discarded_nested_and_block_results_drop_once) {
  EXPECT_EQ(executeString(program(R"(
    function run() void {
      match Maybe.Some(Res(1)) {
        Maybe.Some(r) => { match true { true => r, _ => Res(2) }; },
        Maybe.None => Res(3)
      };
    }
    function main() i32 { run(); return drops; }
  )")),
            1);
}

TEST(MemorySafety_Drops_Match, borrowing_keeps_enum_alive) {
  EXPECT_EQ(executeString(program(R"(
    function peek(o: ref Maybe) i32 {
      return match o { Maybe.Some(r) => r.id, Maybe.None => 0 };
    }
    function run() void {
      var o = Maybe.Some(Res(7));
      if (peek(o) != 7 or peek(o) != 7 or drops != 0) { drops = 100; }
    }
    function main() i32 { run(); return drops; }
  )")),
            1);
}

TEST(MemorySafety_Drops_Match, borrowed_payload_cannot_escape_as_owned_result) {
  EXPECT_THROW(executeString(program(R"(
    function take(o: ref Maybe) Res {
      return match o { Maybe.Some(r) => r, Maybe.None => Res(2) };
    }
    function main() i32 { return 0; }
  )")),
               std::exception);
}

TEST(MemorySafety_Drops_Match, consumed_enum_cannot_be_used_again) {
  EXPECT_THROW(executeString(program(R"(
    function main() i32 {
      var o = Maybe.Some(Res(1));
      var first = match o { Maybe.Some(r) => r, Maybe.None => Res(2) };
      return match o { Maybe.Some(r) => r.id, Maybe.None => 0 };
    }
  )")),
               std::exception);
}

TEST(MemorySafety_Drops_Match, duplicate_and_wildcard_arms_use_first_match) {
  EXPECT_EQ(executeString(program(R"(
    function main() i32 {
      var a = match Maybe.Some(Res(1)) {
        Maybe.Some(r) => r.id,
        Maybe.Some(r) => 100,
        _ => 200
      };
      var b = match Maybe.Some(Res(2)) {
        _ => 3,
        Maybe.Some(r) => 300
      };
      return a + b;
    }
  )")),
            4);
}

TEST(MemorySafety_Drops_Match, string_buffer_survives_match_and_return) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    using std;
    function make(alloc: const ref HeapAllocator, flag: bool) String {
      return match flag { true => String(alloc, "hello"), _ => String(alloc, "world") };
    }
    function main() i32 {
      var alloc = make_heap_allocator();
      var s = make(alloc, true);
      if (s.equals_literal("hello")) { return 1; }
      return 0;
    }
  )"),
            1);
}

TEST(MemorySafety_Drops_Match, assignment_argument_and_repeated_execution) {
  EXPECT_EQ(executeString(program(R"(
    function sink(r: Res) void { }
    function run() void {
      var r = Res(9);
      for (var i: i32 = 0; i < 3; i = i + 1) {
        r = match i { 0 => Res(1), _ => Res(2) };
        sink(match Maybe.Some(Res(3)) {
          Maybe.Some(x) => x, Maybe.None => Res(4)
        });
      }
    }
    function main() i32 { run(); return drops; }
  )")),
            7);
}

TEST(MemorySafety_Drops_Match, explicit_return_cleans_remaining_payloads) {
  EXPECT_EQ(executeString(program(R"(
    function take(t: Triple) Res {
      match t {
        Triple.Left(a, b, c) => { return b; },
        Triple.Right(a, b, c) => { return c; }
      };
    }
    function run() void {
      var r = take(Triple.Left(Res(1), Res(2), Res(3)));
      if (drops != 2 or r.id != 2) { drops = 100; }
    }
    function main() i32 { run(); return drops; }
  )")),
            3);
}

TEST(MemorySafety_Drops_Match, unwind_cleans_owned_payloads) {
  EXPECT_EQ(executeString(program(R"(
    class Error implements IError {
      init() {}
      method code() i32 { return 1; }
      method message() static_ptr<u8> { return "error"; }
    }
    function fail() void throws IError { throw Error(); }
    function sink(r: Res) void { }
    function run(t: Triple) void throws IError {
      match t {
        Triple.Left(a, b, c) => { sink(a); fail(); },
        Triple.Right(a, b, c) => { sink(a); fail(); }
      };
    }
    function main() i32 {
      try { run(Triple.Left(Res(1), Res(2), Res(3))); }
      catch (e: IError) { return drops; }
      return -1;
    }
  )")),
            3);
}

TEST(MemorySafety_Drops_Match, payload_enum_result_survives_return) {
  EXPECT_EQ(executeString(program(R"(
    function take(flag: bool) Maybe {
      return match flag { true => Maybe.Some(Res(1)), false => Maybe.None };
    }
    function main() i32 {
      var o = take(true);
      if (drops != 0) { return 100; }
      if (true) {
        var r = match o { Maybe.Some(r) => r, Maybe.None => Res(2) };
      }
      return drops;
    }
  )")),
            1);
}

TEST(MemorySafety_Drops_Match, nonexhaustive_owned_result_is_rejected) {
  EXPECT_THROW(executeString(program(R"(
    function main() i32 {
      var r = match 2 { 1 => Res(1) };
      return r.id;
    }
  )")),
               std::exception);
}

TEST(MemorySafety_Drops_Match, moved_payload_cannot_be_used_again) {
  EXPECT_THROW(executeString(program(R"(
    function sink(r: Res) void { }
    function main() i32 {
      return match Maybe.Some(Res(1)) {
        Maybe.Some(r) => { sink(r); r.id; },
        Maybe.None => 0
      };
    }
  )")),
               std::exception);
}

TEST(MemorySafety_Drops_Match, parenthesized_return_transfers_result) {
  EXPECT_EQ(executeString(program(R"(
    function take(o: Maybe) Res {
      return (match o { Maybe.Some(r) => r, Maybe.None => Res(2) });
    }
    function run() void {
      var r = take(Maybe.Some(Res(1)));
      if (drops != 0) { drops = 100; }
    }
    function main() i32 { run(); return drops; }
  )")),
            1);
}

TEST(MemorySafety_Drops_Match,
     owned_binding_shadowing_reference_cannot_escape) {
  EXPECT_THROW(executeString(program(R"(
    function take(r: ref Res, o: Maybe) ref Res {
      match o {
        Maybe.Some(r) => { var moved = r; return moved; },
        Maybe.None => { return r; }
      };
    }
    function main() i32 { return 0; }
  )")),
               std::exception);
}

TEST(MemorySafety_Drops_Match, borrowed_binding_can_share_discriminant_name) {
  EXPECT_EQ(executeString(program(R"(
    enum Only { Some(Res) }
    function peek(o: ref Only) ref Res {
      match o {
        Only.Some(o) => { return o; }
      };
    }
    function run() void {
      var o = Only.Some(Res(7));
      if (peek(o).id != 7 or drops != 0) { drops = 100; }
    }
    function main() i32 { run(); return drops; }
  )")),
            1);
}

TEST(MemorySafety_Drops_Match, owned_result_requires_a_value_from_each_arm) {
  EXPECT_THROW(executeString(program(R"(
    function main() i32 {
      var r = match Maybe.None {
        Maybe.Some(r) => r,
        Maybe.None => { }
      };
      return r.id;
    }
  )")),
               std::exception);
}

TEST(MemorySafety_Drops_Match, inspection_does_not_consume_owned_enum) {
  EXPECT_EQ(executeString(program(R"(
    function run() void {
      var o = Maybe.Some(Res(7));
      var first = match o { Maybe.Some(r) => r.id, Maybe.None => 0 };
      var second = match o { Maybe.Some(r) => r.id, Maybe.None => 0 };
      if (first != 7 or second != 7 or drops != 0) { drops = 100; }
      var r = match o { Maybe.Some(r) => r, Maybe.None => Res(2) };
      if (r.id != 7 or drops != 0) { drops = 100; }
    }
    function main() i32 { run(); return drops; }
  )")),
            1);
}

TEST(MemorySafety_Drops_Match,
     fresh_result_does_not_consume_inspected_payload) {
  EXPECT_EQ(executeString(program(R"(
    function run() void {
      var o = Maybe.Some(Res(7));
      if (true) {
        var result = match o { Maybe.Some(r) => Res(r.id), Maybe.None => Res(2) };
        if (drops != 0) { drops = 100; }
      }
      var id = match o { Maybe.Some(r) => r.id, Maybe.None => 0 };
      if (id != 7 or drops != 1) { drops = 100; }
    }
    function main() i32 { run(); return drops; }
  )")),
            2);
}

TEST(MemorySafety_Drops_Match,
     consuming_arm_makes_enum_unavailable_after_match) {
  EXPECT_THROW(executeString(program(R"(
    function sink(r: Res) void { }
    function main() i32 {
      var t = Triple.Right(Res(1), Res(2), Res(3));
      match t {
        Triple.Left(a, b, c) => { sink(b); },
        Triple.Right(a, b, c) => { }
      };
      return match t { Triple.Left(a, b, c) => a.id, Triple.Right(a, b, c) => b.id };
    }
  )")),
               std::exception);
}

TEST(MemorySafety_Drops_Match, thrown_payload_survives_unwind) {
  EXPECT_EQ(executeString(program(R"(
    class Error implements IError {
      var id: i32;
      init() { this.id = 7; }
      deinit() { this.id = 0; }
      method code() i32 { return this.id; }
      method message() static_ptr<u8> { return "error"; }
    }
    enum Errors { One(Error, Res) }
    function run() i32 throws IError {
      try {
        var errors = Errors.One(Error(), Res(1));
        match errors { Errors.One(e, _) => { throw e; } };
      } catch (e: IError) { return e.code(); }
      return -1;
    }
    function main() i32 throws IError {
      if (run() != 7) { return 100; }
      return drops;
    }
  )")),
            1);
}

TEST(MemorySafety_Drops_Match,
     unreachable_payload_move_does_not_consume_input) {
  EXPECT_EQ(executeString(program(R"(
    function run(o: Maybe) void {
      match o {
        Maybe.Some(r) => { return; },
        Maybe.Some(r) => r,
        Maybe.None => { }
      };
      match o { Maybe.Some(r) => r.id, Maybe.None => 0 };
    }
    function main() i32 {
      run(Maybe.Some(Res(1)));
      run(Maybe.None);
      return drops;
    }
  )")),
            1);
}
