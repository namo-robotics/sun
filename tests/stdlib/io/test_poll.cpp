// tests/stdlib/io/test_poll.cpp - Poller
//
// The point of Poller is that nothing here builds a struct pollfd by hand.

#include <gtest/gtest.h>

#include <string>

#include "driver/execution_utils.h"

TEST(Stdlib_Io_Poll, empty_poller_returns_zero) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.io;

    function main() i32 {
        var a = make_heap_allocator();
        var p = Poller(a);
        if (p.count() != 0) { return 1; }
        try { return p.wait(0); } catch (e: IError) { return -1; }
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Io_Poll, pipe_becomes_readable_after_a_write) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.io;
    using std.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var pipe = open_pipe(a);
            var p = Poller(a);
            p.add_read(pipe.read_fd());

            // Nothing written yet, so a zero timeout finds nothing ready.
            if (p.wait(0) != 0) { return 1; }
            if (p.is_readable(0)) { return 2; }

            var msg = String(a, "hi");
            var writer = File();
            writer.adopt(pipe.write_fd());
            writer.write(msg);

            if (p.wait(1000) != 1) { return 3; }
            if (not p.is_readable(0)) { return 4; }
            if (p.fd_at(0) != pipe.read_fd()) { return 5; }
            pipe.close_read();
            return 0;
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Io_Poll, hangup_is_reported_when_the_writer_closes) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.io;
    using std.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var pipe = open_pipe(a);
            pipe.close_write();

            var p = Poller(a);
            p.add_read(pipe.read_fd());
            if (p.wait(1000) < 1) { return 1; }
            if (not p.is_hup(0)) { return 2; }
            pipe.close_read();
            return 0;
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Io_Poll, watches_several_descriptors_by_position) {
  auto value = executeStringWithStdlib(R"(
    using std;
    using std.io;
    using std.process;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var first = open_pipe(a);
            var second = open_pipe(a);

            var p = Poller(a);
            p.add_read(first.read_fd());
            p.add_read(second.read_fd());
            if (p.count() != 2) { return 1; }

            // Write to the second one only.
            var msg = String(a, "x");
            var writer = File();
            writer.adopt(second.write_fd());
            writer.write(msg);

            if (p.wait(1000) != 1) { return 2; }
            if (p.is_readable(0)) { return 3; }
            if (not p.is_readable(1)) { return 4; }

            first.close_read();
            first.close_write();
            second.close_read();
            return 0;
        } catch (e: IError) {
            return -1;
        }
    }
  )");
  EXPECT_EQ(value, 0);
}
