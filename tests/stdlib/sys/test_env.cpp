// tests/stdlib/sys/test_env.cpp - sun.env
//
// These run in the gtest process, so anything they change about the
// environment or the working directory is this process's. The fixture puts it
// all back: getStdlibMoonImports() resolves build/stdlib.moon relative to the
// working directory, so a leaked chdir would break every later test.

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

TEST_F(Stdlib_Sys_Env, get_returns_none_when_unset) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.env;

    function main() i32 {
        var a = make_heap_allocator();
        return match get(a, ")" + varName +
                                       R"(") {
            Option.Some(v) => 1,
            Option.None => 0
        };
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Env, get_reads_a_variable_set_from_outside) {
  setenv(varName.c_str(), "from-outside", 1);

  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.env;

    function main() i32 {
        var a = make_heap_allocator();
        var v = match get(a, ")" + varName +
                                       R"(") {
            Option.Some(s) => s,
            Option.None => String(a, "")
        };
        if (not v.equals_literal("from-outside")) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Env, set_get_remove_round_trip) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.env;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            if (has(")" + varName + R"(")) { return 1; }
            set(")" + varName + R"(", "value-one");
            if (not has(")" + varName + R"(")) { return 2; }

            var v = match get(a, ")" + varName +
                                       R"(") {
                Option.Some(s) => s,
                Option.None => String(a, "")
            };
            if (not v.equals_literal("value-one")) { return 3; }

            // Setting again replaces
            set(")" + varName + R"(", "value-two");
            var w = match get(a, ")" + varName +
                                       R"(") {
                Option.Some(s) => s,
                Option.None => String(a, "")
            };
            if (not w.equals_literal("value-two")) { return 4; }

            remove(")" + varName + R"(");
            if (has(")" + varName + R"(")) { return 5; }
        } catch (e: IError) {
            return 6;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
  EXPECT_EQ(getenv(varName.c_str()), nullptr);
}

TEST_F(Stdlib_Sys_Env, cwd_reports_the_working_directory) {
  auto value =
      executeStringWithStdlib(R"(
    using sun;
    using sun.env;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var dir = get_cwd(a);
            if (dir.is_empty()) { return 1; }
            // An absolute path, so it starts with '/'
            if (dir.at(0) != 47) { return 2; }
            if (not dir.equals_literal(")" +
                              originalCwd.string() + R"(")) { return 3; }
        } catch (e: IError) {
            return 4;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Env, set_cwd_changes_and_cwd_reflects_it) {
  // "/" rather than "/tmp": on macOS /tmp is a symlink to /private/tmp, so
  // the kernel's idea of the directory would not match the literal.
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.env;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            set_cwd("/");
            var dir = get_cwd(a);
            if (not dir.equals_literal("/")) { return 1; }
        } catch (e: IError) {
            return 2;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
  // TearDown restores it, but check the change really happened.
  EXPECT_EQ(std::filesystem::current_path(), std::filesystem::path("/"));
}

TEST_F(Stdlib_Sys_Env, set_cwd_to_a_missing_directory_throws) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.env;

    function main() i32 {
        try {
            set_cwd("/definitely/not/a/directory");
        } catch (e: IError) {
            return 42;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST_F(Stdlib_Sys_Env, args_reads_argc_and_argv_from_main) {
  const char* argv[] = {"prog", "alpha", "beta", nullptr};
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.env;

    function main(argc: i32, argv: raw_ptr<raw_ptr<i8>>) i32 {
        var a = make_heap_allocator();
        var cli = collect_args(a, argc, argv);
        if (cli.size() != 3) { return 1; }
        // Each borrow is used within its own statement - named bindings
        // would hold three borrows of cli at once
        if (not cli.get_unchecked(0).equals_literal("prog")) { return 2; }
        if (not cli.get_unchecked(1).equals_literal("alpha")) { return 3; }
        if (not cli.get_unchecked(2).equals_literal("beta")) { return 4; }
        return 0;
    }
  )",
                                       3, const_cast<char**>(argv));
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Env, args_with_no_arguments_is_empty) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.env;

    function main(argc: i32, argv: raw_ptr<raw_ptr<i8>>) i32 {
        var a = make_heap_allocator();
        var cli = collect_args(a, argc, argv);
        return _convert<i32>(cli.size());
    }
  )");
  EXPECT_EQ(value, 0);
}
