// Tests for payload enums that own heap resources: construction moves the
// payload in, the enum drops it exactly once at scope exit / overwrite, match
// bindings borrow the payload in place, and the borrow checker rejects moves
// out of bindings and out of matched discriminants.

#include <gtest/gtest.h>

#include "execution_utils.h"

namespace {

// Owner models a resource holder: deinit is a no-op on moved-from (zeroed)
// storage, exactly like Unique<T> in the stdlib.
const char* kOwnerPreamble = R"(
    var counter: i32 = 0;

    class Owner {
      var id: i32;
      function init(id: i32) {
        this.id = id;
      }
      function deinit() void {
        if (this.id != 0) {
          counter = counter + 1;
          this.id = 0;
        }
      }
      function get_id() i32 {
        return this.id;
      }
    }

    enum Holder { Hold(Owner), Nothing }
)";

std::string withPreamble(const std::string& body) {
  return std::string(kOwnerPreamble) + body;
}

}  // namespace

TEST(EnumDropTest, owning_payload_dropped_at_scope_exit) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      var h = Holder.Hold(Owner(1));
      return 0;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  EXPECT_EQ(value, 1);
}

TEST(EnumDropTest, unit_variant_drops_nothing) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      var h = Holder.Nothing;
      return 0;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  EXPECT_EQ(value, 0);
}

TEST(EnumDropTest, construction_moves_argument_single_drop) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      var o = Owner(1);
      var h = Holder.Hold(o);
      return 0;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  // o moved into h: exactly one drop total, not two
  EXPECT_EQ(value, 1);
}

TEST(EnumDropTest, construction_use_after_move_is_error) {
  EXPECT_THROW(executeString(withPreamble(R"(
    function main() i32 {
      var o = Owner(1);
      var h = Holder.Hold(o);
      return o.get_id();
    }
  )")),
               std::exception);
}

TEST(EnumDropTest, enum_var_creation_moves_source) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      var a = Holder.Hold(Owner(1));
      var b = a;
      return 0;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  // a moved into b: one payload, one drop
  EXPECT_EQ(value, 1);
}

TEST(EnumDropTest, enum_var_creation_use_after_move_is_error) {
  EXPECT_THROW(executeString(withPreamble(R"(
    function main() i32 {
      var a = Holder.Hold(Owner(1));
      var b = a;
      return match a { Holder.Hold(o) => 1, Holder.Nothing => 0 };
    }
  )")),
               std::exception);
}

TEST(EnumDropTest, assignment_drops_old_value_then_moves) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      var h = Holder.Hold(Owner(1));
      h = Holder.Hold(Owner(2));
      // Owner(1) dropped by the overwrite; Owner(2) dropped at exit
      return counter;
    }

    function main() i32 {
      var mid = helper();
      if (mid != 1) {
        return -1;
      }
      return counter;
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(EnumDropTest, assignment_of_unit_variant_drops_old_payload) {
  auto value = executeString(withPreamble(R"(
    function main() i32 {
      var h = Holder.Hold(Owner(1));
      h = Holder.Nothing;
      return counter;
    }
  )"));
  EXPECT_EQ(value, 1);
}

TEST(EnumDropTest, match_binding_borrows_payload_no_extra_drop) {
  auto value = executeString(withPreamble(R"(
    function helper() i32 {
      var h = Holder.Hold(Owner(7));
      var seen: i32 = match h {
        Holder.Hold(o) => o.get_id(),
        Holder.Nothing => -1
      };
      if (counter != 0) {
        return -100;
      }
      return seen;
    }

    function main() i32 {
      var seen = helper();
      if (seen != 7) {
        return seen;
      }
      // Payload dropped once, at helper's exit
      return counter;
    }
  )"));
  EXPECT_EQ(value, 1);
}

TEST(EnumDropTest, moving_out_of_match_binding_is_error) {
  EXPECT_THROW(executeString(withPreamble(R"(
    function main() i32 {
      var h = Holder.Hold(Owner(1));
      match h {
        Holder.Hold(o) => {
          var stolen = o;
        },
        Holder.Nothing => { }
      };
      return 0;
    }
  )")),
               std::exception);
}

TEST(EnumDropTest, passing_match_binding_by_value_is_error) {
  EXPECT_THROW(executeString(withPreamble(R"(
    function consume(o: Owner) i32 { return o.get_id(); }
    function main() i32 {
      var h = Holder.Hold(Owner(1));
      return match h {
        Holder.Hold(o) => consume(o),
        Holder.Nothing => 0
      };
    }
  )")),
               std::exception);
}

TEST(EnumDropTest, passing_match_binding_by_ref_is_ok) {
  auto value = executeString(withPreamble(R"(
    function peek(o: ref Owner) i32 { return o.get_id(); }
    function main() i32 {
      var h = Holder.Hold(Owner(5));
      return match h {
        Holder.Hold(o) => peek(o),
        Holder.Nothing => 0
      };
    }
  )"));
  EXPECT_EQ(value, 5);
}

TEST(EnumDropTest, reassigning_discriminant_inside_arm_is_error) {
  EXPECT_THROW(executeString(withPreamble(R"(
    function main() i32 {
      var h = Holder.Hold(Owner(1));
      match h {
        Holder.Hold(o) => {
          h = Holder.Nothing;
        },
        Holder.Nothing => { }
      };
      return 0;
    }
  )")),
               std::exception);
}

TEST(EnumDropTest, moves_in_one_arm_do_not_poison_other_arms) {
  auto value = executeString(withPreamble(R"(
    function consume(o: Owner) i32 { return o.get_id(); }
    function main() i32 {
      var spare = Owner(9);
      var h = Holder.Nothing;
      var r: i32 = match h {
        Holder.Hold(o) => consume(spare),
        Holder.Nothing => consume(spare)
      };
      return r;
    }
  )"));
  // Each arm is checked against the pre-match state; using `spare` in both
  // arms is fine because only one arm runs
  EXPECT_EQ(value, 9);
}

TEST(EnumDropTest, enum_field_in_class_dropped_with_class) {
  auto value = executeString(withPreamble(R"(
    class Box {
      var slot: Holder;
      function init() {
        this.slot = Holder.Hold(Owner(3));
      }
    }

    function helper() i32 {
      var b = Box();
      return 0;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  EXPECT_EQ(value, 1);
}

TEST(EnumDropTest, enum_field_assignment_drops_old_payload) {
  auto value = executeString(withPreamble(R"(
    class Box {
      var slot: Holder;
      function init() {
        this.slot = Holder.Hold(Owner(3));
      }
    }

    function main() i32 {
      var b = Box();
      b.slot = Holder.Hold(Owner(4));
      return counter;
    }
  )"));
  // Owner(3) dropped by the field overwrite; Owner(4) still owned by b
  EXPECT_EQ(value, 1);
}

TEST(EnumDropTest, nested_owning_enum_drops_recursively) {
  auto value = executeString(withPreamble(R"(
    enum Outer { Wrap(Holder), Empty }

    function helper() i32 {
      var o = Outer.Wrap(Holder.Hold(Owner(1)));
      return 0;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  EXPECT_EQ(value, 1);
}

TEST(EnumDropTest, block_scoped_owning_enum_dropped_at_block_exit) {
  auto value = executeString(withPreamble(R"(
    function main() i32 {
      if (true) {
        var h = Holder.Hold(Owner(1));
      }
      return counter;
    }
  )"));
  EXPECT_EQ(value, 1);
}

TEST(EnumDropTest, owning_enum_dropped_on_unwind) {
  auto value = executeString(withPreamble(R"(
    class TestError implements IError {
      function init() {}
      function code() i32 { return 1; }
      function message() static_ptr<u8> { return "test error"; }
    }

    function thrower() void, IError {
      throw TestError();
    }

    function middle() void, IError {
      var h = Holder.Hold(Owner(1));
      thrower();
    }

    function main() i32 {
      try {
        middle();
      } catch (e: IError) {
        return counter;
      }
      return -1;
    }
  )"));
  EXPECT_EQ(value, 1);
}

TEST(EnumDropTest, returned_owning_enum_moves_to_caller) {
  auto value = executeString(withPreamble(R"(
    function make() Holder {
      var h = Holder.Hold(Owner(6));
      return h;
    }

    function helper() i32 {
      var got = make();
      var id: i32 = match got {
        Holder.Hold(o) => o.get_id(),
        Holder.Nothing => -1
      };
      if (counter != 0) {
        return -100;
      }
      return id;
    }

    function main() i32 {
      var id = helper();
      if (id != 6) {
        return id;
      }
      return counter;
    }
  )"));
  // The payload survives the return (moved, not dropped in make) and is
  // dropped once when the caller's variable dies
  EXPECT_EQ(value, 1);
}

TEST(EnumDropTest, option_of_string_via_stdlib) {
  auto value = executeStringWithStdlib(R"(
    using sun;

    function main() i32 {
      var alloc = make_heap_allocator();
      var maybe = Option.Some(String(alloc, "hello"));
      var len: i64 = match maybe {
        Option.Some(s) => s.length(),
        Option.None => -1
      };
      var maybe2 = maybe;
      var none: Option<String> = Option.None;
      return len;
    }
  )");
  // Option<String> compiles, matches, moves, and drops without crashing
  EXPECT_EQ(value, 5);
}

TEST(EnumDropTest, vec_of_option_of_owner_drops_elements) {
  auto value = executeStringWithStdlib(withPreamble(R"(
    using sun;

    function helper() i32 {
      var alloc = make_heap_allocator();
      var v = Vec<Holder>(alloc, 4);
      v.push(Holder.Hold(Owner(1)));
      v.push(Holder.Nothing);
      v.push(Holder.Hold(Owner(2)));
      return 0;
    }

    function main() i32 {
      helper();
      return counter;
    }
  )"));
  EXPECT_EQ(value, 2);
}

TEST(EnumDropTest, non_owning_payload_enum_still_copies_scalars_in_match) {
  auto value = executeString(R"(
    enum Opt { Some(i32), None }
    function main() i32 {
      var o = Opt.Some(41);
      var r: i32 = match o {
        Opt.Some(v) => v + 1,
        Opt.None => 0
      };
      return r;
    }
  )");
  EXPECT_EQ(value, 42);
}

// ============================================================================
// Cross-module: owning payload enum exported through a .moon bundle. The
// drop function for the shared specialization is emitted in both the library
// and the importing program (LinkOnceODR) and must merge cleanly.
// ============================================================================

#include <filesystem>
#include <fstream>

#include "metadata_extractor.h"
#include "moon/moon.h"
#include "moon_import.h"

TEST(EnumDropTest, cross_moon_owning_enum_drops_once) {
  namespace fs = std::filesystem;
  initTestEnvironment();

  fs::path dir = fs::temp_directory_path() / "sun_enum_drop_moon_test";
  fs::create_directories(dir);
  fs::path libSrc = dir / "droplib.sun";
  {
    std::ofstream out(libSrc);
    // Drops are counted through a heap cell the resource points at (module
    // bodies cannot see file-level globals).
    out << R"(
      module droplib {
          class Res {
              var cell: raw_ptr<i32>;
              var id: i32;
              function init(cell: raw_ptr<i32>, id: i32) {
                  this.cell = cell;
                  this.id = id;
              }
              function deinit() void {
                  if (this.id != 0) {
                      unsafe { _store<i32>(this.cell, 0, _load<i32>(this.cell, 0) + 1); };
                      this.id = 0;
                  }
              }
              function get_id() i32 { return this.id; }
          }

          enum Maybe { Got(Res), Nope }

          function make(cell: raw_ptr<i32>, id: i32) Maybe {
              return Maybe.Got(Res(cell, id));
          }

          function consume_in_lib(cell: raw_ptr<i32>, id: i32) i32 {
              var m = make(cell, id);
              return 0;
          }
      }
    )";
  }

  auto metadata = sun::extractMetadataFromFile(libSrc.string());
  ASSERT_TRUE(metadata.has_value());
  auto libDriver = Driver::createForAOT("moon_module");
  libDriver->compileFiles({libSrc.string()}, {});
  sun::SunLibWriter writer;
  writer.addModule(libDriver->getModule(), *metadata);
  fs::path moonPath = dir / "droplib.moon";
  ASSERT_TRUE(writer.write(moonPath));

  auto driver = Driver::createForJIT("moon_main");
  driver->setMoonImports({sun::MoonImport(moonPath.string())});
  auto value = driver->executeString(R"(
    using droplib;

    function use_here(cell: raw_ptr<i32>) i32 {
        var m = make(cell, 5);
        return match m {
            Maybe.Got(r) => r.get_id(),
            Maybe.Nope => -1
        };
    }

    function main() i32 {
        var drops: i32 = 0;
        var cell: raw_ptr<i32> = unsafe { _address_of<i32>(drops); };
        // One owning value dropped inside the library ...
        consume_in_lib(cell, 1);
        // ... and one dropped here after use
        var id = use_here(cell);
        if (id != 5) { return -100; }
        return drops;
    }
  )");
  EXPECT_EQ(value, 2);
}

// Small scalar payload next to a large, 8-byte-aligned payload: the scalar
// must not live in the tag's alignment padding, or aggregate moves lose it.
TEST(EnumDropTest, small_payload_survives_move_next_to_owning_payload) {
  auto value = executeString(withPreamble(R"(
    class Wide {
      var a: i64;
      var b: i64;
      var o: Owner;
      function init() { this.a = 1; this.b = 2; this.o = Owner(3); }
    }
    enum P { NotSet, Pct(i32), Big(Wide) }
    class Box { var p: P; function init() { this.p = P.NotSet; } }
    function main() i32 {
      var local = P.NotSet;
      local = P.Pct(55);
      var l: i32 = match local { P.Pct(v) => v, P.Big(w) => -2, P.NotSet => -3 };
      var box = Box();
      box.p = P.Pct(66);
      var f: i32 = match box.p { P.Pct(v) => v, P.Big(w) => -2, P.NotSet => -3 };
      return l + f;
    }
  )"));
  EXPECT_EQ(value, 121);
}
