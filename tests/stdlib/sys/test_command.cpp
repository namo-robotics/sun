// tests/stdlib/sys/test_command.cpp - Command / Child / Output
//
// These fork from inside the gtest process, which hosts the JIT and is
// multithreaded. That is only safe because Command's child path touches
// nothing but async-signal-safe calls between fork and exec. Every test that
// spawns also waits, so no zombies are left for a later test's waitpid.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "execution_utils.h"

class Stdlib_Sys_Command : public ::testing::Test {
 protected:
  void SetUp() override {
    // /bin/sh is on every Linux worth testing on, but a minimal image may
    // surprise us; degrade rather than fail.
    if (!std::filesystem::exists("/bin/sh")) {
      GTEST_SKIP() << "/bin/sh not present";
    }
  }
};

TEST_F(Stdlib_Sys_Command, output_captures_stdout_and_status) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var cmd = Command(a, "/bin/sh");
            cmd.arg("-c");
            cmd.arg("printf hello");
            var out = cmd.output();
            if (out.status() != 0) { return 1; }
            if (not out.stdout().equals_literal("hello")) { return 2; }
            if (not out.stderr().isEmpty()) { return 3; }
        } catch (e: IError) {
            return 4;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Command, non_zero_exit_is_reported) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var cmd = Command(a, "/bin/sh");
            cmd.arg("-c");
            cmd.arg("exit 3");
            return cmd.output().status();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  EXPECT_EQ(value, 3);
}

TEST_F(Stdlib_Sys_Command, stderr_is_captured_separately) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var cmd = Command(a, "/bin/sh");
            cmd.arg("-c");
            cmd.arg("printf out; printf err >&2");
            var res = cmd.output();
            if (not res.stdout().equals_literal("out")) { return 1; }
            if (not res.stderr().equals_literal("err")) { return 2; }
        } catch (e: IError) {
            return 3;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Command, large_output_does_not_deadlock) {
  // Well past one pipe buffer (64 KiB on Linux). Draining stdout to the end
  // before touching stderr would wedge here.
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var cmd = Command(a, "/bin/sh");
            cmd.arg("-c");
            cmd.arg("yes abcdefgh | head -c 200000; printf e >&2");
            var res = cmd.output();
            if (res.stdout().length() != 200000) { return 1; }
            if (not res.stderr().equals_literal("e")) { return 2; }
        } catch (e: IError) {
            return 3;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Command, arguments_are_passed_separately_not_reparsed) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            // A space inside one argument stays inside it.
            var cmd = Command(a, "/bin/sh");
            cmd.arg("-c");
            cmd.arg("printf '%s|%s' \"$1\" \"$2\"");
            cmd.arg("sh");
            cmd.arg("one two");
            cmd.arg("three");
            var res = cmd.output();
            if (not res.stdout().equals_literal("one two|three")) { return 1; }
        } catch (e: IError) {
            return 2;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Command, string_arguments_are_accepted) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var script = String(a, "printf built-at-runtime");
            var cmd = Command(a, "/bin/sh");
            cmd.arg("-c");
            cmd.arg(script);
            var res = cmd.output();
            if (not res.stdout().equals_literal("built-at-runtime")) { return 1; }
            // arg(ref String) borrows; the caller still owns it
            if (script.length() != 23) { return 2; }
        } catch (e: IError) {
            return 3;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Command, status_runs_with_inherited_streams) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var cmd = Command(a, "/bin/sh");
            cmd.arg("-c");
            cmd.arg("exit 7");
            return cmd.status();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  EXPECT_EQ(value, 7);
}

TEST_F(Stdlib_Sys_Command, start_then_wait_gives_the_exit_code) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var cmd = Command(a, "/bin/sh");
            cmd.arg("-c");
            cmd.arg("exit 5");
            var child = cmd.start();
            if (child.id() <= 0) { return 1; }
            return child.wait();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  EXPECT_EQ(value, 5);
}

TEST_F(Stdlib_Sys_Command, child_stdin_can_be_written) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var cmd = Command(a, "/bin/sh");
            cmd.arg("-c");
            cmd.arg("cat");
            cmd.stdin(Stdio.Piped);
            cmd.stdout(Stdio.Piped);
            var child = cmd.start();
            var payload = String(a, "echoed back");
            child.write_stdin(payload);
            child.close_stdin();
            var res = child.collect(a);
            if (not res.stdout().equals_literal("echoed back")) { return 1; }
            if (res.status() != 0) { return 2; }
        } catch (e: IError) {
            return 3;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Sys_Command, a_signalled_child_reports_128_plus_the_signal) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var cmd = Command(a, "/bin/sh");
            cmd.arg("-c");
            cmd.arg("kill -9 $$");
            cmd.stdout(Stdio.Null);
            cmd.stderr(Stdio.Null);
            return cmd.status();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  EXPECT_EQ(value, 137);  // 128 + SIGKILL
}

TEST_F(Stdlib_Sys_Command, missing_program_exits_127) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var cmd = Command(a, "/no/such/program/anywhere");
            cmd.stderr(Stdio.Null);
            return cmd.status();
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  EXPECT_EQ(value, 127);
}
