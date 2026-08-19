// tests/stdlib/sys/test_process.cpp - process identity, pipes, wait status
//
// Nothing here forks or exits: these run inside the gtest process. Spawning
// is covered in test_command.cpp, which goes through Command's
// async-signal-safe child path.

#include <gtest/gtest.h>
#include <unistd.h>

#include <string>

#include "execution_utils.h"

TEST(Stdlib_Sys_Process, ids_match_the_host_process) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        return pid();
    }
  )");
  EXPECT_EQ(sun::toDouble(value), static_cast<double>(getpid()));
}

TEST(Stdlib_Sys_Process, parent_pid_and_uids_are_plausible) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        if (parent_pid() <= 0) { return 1; }
        if (pid() == parent_pid()) { return 2; }
        // uid is unsigned; only that it matches euid in a normal process
        if (uid() != euid()) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Sys_Process, pipe_round_trips_bytes) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var p = open_pipe(a);
            var writer = File();
            writer.adopt(p.write_fd());
            var msg = String(a, "through the pipe");
            if (writer.write(msg) != 16) { return 1; }
            writer.close();

            var reader = File();
            reader.adopt(p.read_fd());
            var got = reader.read_all(a);
            reader.close();
            if (not got.equals_literal("through the pipe")) { return 2; }
        } catch (e: IError) {
            return 3;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Sys_Process, wait_status_decoding) {
  // The bit layout is fixed by waitpid(2): low 7 bits the signal, next 8 the
  // exit code.
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.process;

    function main() i32 {
        // exit(3): status 0x0300
        if (not exited(768)) { return 1; }
        if (exit_status(768) != 3) { return 2; }
        if (signaled(768)) { return 3; }

        // killed by SIGKILL (9)
        if (exited(9)) { return 4; }
        if (not signaled(9)) { return 5; }
        if (term_signal(9) != 9) { return 6; }

        // exit(0)
        if (not exited(0)) { return 7; }
        if (exit_status(0) != 0) { return 8; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Sys_Process, dup_fd_redirects_a_descriptor) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;
    using sun.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var p = open_pipe(a);
            // Point a spare descriptor at the write end and write through it.
            dup_fd(p.write_fd(), 9);
            var writer = File();
            writer.adopt(9);
            var msg = String(a, "via dup");
            writer.write(msg);
            writer.close();
            p.close_write();

            var reader = File();
            reader.adopt(p.read_fd());
            var got = reader.read_all(a);
            reader.close();
            if (not got.equals_literal("via dup")) { return 1; }
        } catch (e: IError) {
            return 2;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}
