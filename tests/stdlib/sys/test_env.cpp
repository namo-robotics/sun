// tests/stdlib/sys/test_env.cpp - std.env.Env

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "driver/execution_utils.h"

class Stdlib_Sys_Env : public ::testing::Test {
 protected:
  std::string varName;
  std::filesystem::path originalCwd;

  void SetUp() override {
    varName = "SUN_ENV_TEST_" + std::to_string(getpid());
    originalCwd = std::filesystem::current_path();
    unsetenv(varName.c_str());
  }

  void TearDown() override {
    std::filesystem::current_path(originalCwd);
    unsetenv(varName.c_str());
  }
};

TEST_F(Stdlib_Sys_Env, captures_lookup_empty_and_equals_values) {
  setenv(varName.c_str(), "left=right", 1);
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.env;

    function main() i32 {
      var alloc = make_heap_allocator();
      var environment = Env(alloc);
      var value_matches: bool = match environment.get(")" + varName + R"(") {
        Option.Some(found) => found.equals_literal("left=right"),
        Option.None => false
      };
      if (not value_matches) { return 1; }
      try {
        environment.set(")" + varName + R"(", "");
      } catch (e: IError) {
        return 2;
      }
      var empty: bool = match environment.get(")" + varName + R"(") {
        Option.Some(found) => found.is_empty(),
        Option.None => false
      };
      return empty ? 0 : 3;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Env, refresh_observes_external_mutation) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.env;

    extern "C" function native_setenv(
        name: raw_ptr<u8>, value: raw_ptr<u8>, overwrite: i32) i32 as "setenv";

    function main() i32 {
      var alloc = make_heap_allocator();
      var environment = Env(alloc);
      if (environment.has(")" + varName + R"(")) { return 1; }
      unsafe { native_setenv(")" + varName + R"(", "outside", 1); };
      if (environment.has(")" + varName + R"(")) { return 2; }
      environment.refresh();
      if (not environment.has(")" + varName + R"(")) { return 3; }
      return match environment.get(")" + varName + R"(") {
        Option.Some(found) =>
            found.equals_literal("outside") ? 0 : 4,
        Option.None => 5
      };
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Env, set_remove_and_iteration_stay_synchronized) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.env;

    function main() i32 {
      var alloc = make_heap_allocator();
      var environment = Env(alloc);
      try {
        environment.set(")" + varName + R"(", "one=two");
        if (not environment.has(")" + varName + R"(")) { return 1; }
        var found: i32 = 0;
        for (var entry: ref EnvEntry in environment) {
          if (entry.name().equals_literal(")" + varName + R"(")) {
            if (not entry.value().equals_literal("one=two")) { return 2; }
            found = found + 1;
          }
        }
        if (found != 1) { return 3; }
        environment.remove(")" + varName + R"(");
        if (environment.has(")" + varName + R"(")) { return 4; }
      } catch (e: IError) {
        return 5;
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
  EXPECT_EQ(getenv(varName.c_str()), nullptr);
}

TEST_F(Stdlib_Sys_Env, string_name_and_value_overloads) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.env;

    function main() i32 {
      var alloc = make_heap_allocator();
      var environment = Env(alloc);
      var name = String(alloc, ")" + varName + R"(");
      var value = String(alloc, "runtime");
      try {
        environment.set(name, value);
        if (not environment.has(name)) { return 1; }
        var value_matches: bool = match environment.get(name) {
          Option.Some(found) => found.equals_literal("runtime"),
          Option.None => false
        };
        if (not value_matches) { return 2; }
        environment.remove(name);
      } catch (e: IError) {
        return 3;
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Env, cwd_and_set_cwd_are_methods) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.env;

    function main() i32 {
      var alloc = make_heap_allocator();
      var environment = Env(alloc);
      try {
        var before = environment.cwd();
        if (not before.equals_literal(")" + originalCwd.string() + R"(")) {
          return 1;
        }
        environment.set_cwd("/");
        var after = environment.cwd();
        if (not after.equals_literal("/")) { return 2; }
      } catch (e: IError) {
        return 3;
      }
      return 0;
    }
  )");
  EXPECT_EQ(value, 0);
  EXPECT_EQ(std::filesystem::current_path(), std::filesystem::path("/"));
}

TEST_F(Stdlib_Sys_Env, args_copies_argv) {
  const char* argv[] = {"prog", "alpha", "beta", nullptr};
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.env;

    function main(argc: i32, argv: raw_ptr<raw_ptr<i8>>) i32 {
      var alloc = make_heap_allocator();
      var environment = Env(alloc);
      var cli = environment.args(argc, argv);
      if (cli.size() != 3) { return 1; }
      if (not cli.get_unchecked(0).equals_literal("prog")) { return 2; }
      if (not cli.get_unchecked(1).equals_literal("alpha")) { return 3; }
      if (not cli.get_unchecked(2).equals_literal("beta")) { return 4; }
      return 0;
    }
  )",
                                       3, const_cast<char**>(argv));
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Env, removed_free_function_api_is_rejected) {
  EXPECT_THROW(executeStringWithStdlib(R"(
    using std;
    using std.env;
    function main() i32 {
      var alloc = make_heap_allocator();
      return match get(alloc, "HOME") {
        Option.Some(value) => 1,
        Option.None => 0
      };
    }
  )"),
               std::exception);
}
