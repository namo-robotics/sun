// tests/stdlib/io/test_paths.cpp - Paths built at runtime
//
// Every sun.io entry point that takes a path takes it either as a
// static_ptr<u8> (what a string literal is) or as a `ref String`, so a path
// assembled from an environment variable, a hash or a session id needs no
// escape hatch (issue #84).

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "driver/execution_utils.h"

class Stdlib_Io_Paths : public ::testing::Test {
 protected:
  std::string testDir;

  void SetUp() override {
    // Unique per process so parallel ctest runs stay isolated.
    testDir = "/tmp/sun_io_paths_" + std::to_string(getpid());
    std::filesystem::remove_all(testDir);
    std::filesystem::create_directories(testDir);
  }

  void TearDown() override { std::filesystem::remove_all(testDir); }
};

TEST_F(Stdlib_Io_Paths, open_and_write_a_computed_path) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function run(a: ref HeapAllocator) i32 throws IError {
        var path = String(a, ")" + testDir +
                                       R"(/");
        path.append("computed.txt");

        var f = File();
        f.open(path, FileMode.Write);
        var body = String(a, "written through a String path");
        var n: i64 = f.write(body);
        f.close();
        return _convert<i32>(n);
    }

    function main() i32 {
        var a = make_heap_allocator();
        try { return run(a); } catch (e: IError) { return -1; }
    }
  )");
  EXPECT_EQ(value, 29);
  EXPECT_TRUE(std::filesystem::exists(testDir + "/computed.txt"));
}

TEST_F(Stdlib_Io_Paths, whole_file_helpers_take_a_computed_path) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function run(a: ref HeapAllocator) i32 throws IError {
        var path = String(a, ")" + testDir +
                                       R"(/");
        path.append("round_trip.txt");
        var body = String(a, "round trip");

        write_string(path, body);
        var back: String = read_to_string(a, path);
        if (not back.equals(body)) { return -2; }
        return _convert<i32>(get_file_size(path));
    }

    function main() i32 {
        var a = make_heap_allocator();
        try { return run(a); } catch (e: IError) { return -1; }
    }
  )");
  EXPECT_EQ(value, 10);
}

TEST_F(Stdlib_Io_Paths, metadata_queries_take_a_computed_path) {
  std::ofstream(testDir + "/there.txt") << "x";

  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function main() i32 {
        var a = make_heap_allocator();
        var dir = String(a, ")" + testDir +
                                       R"(");
        var file = String(a, ")" + testDir +
                                       R"(/");
        file.append("there.txt");
        var missing = String(a, ")" + testDir +
                                       R"(/");
        missing.append("nope.txt");

        if (not exists(file)) { return -1; }
        if (exists(missing)) { return -2; }
        if (not is_dir(dir)) { return -3; }
        if (is_dir(file)) { return -4; }
        if (not is_file(file)) { return -5; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Io_Paths, directory_calls_take_a_computed_path) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function run(a: ref HeapAllocator) i32 throws IError {
        var dir = String(a, ")" + testDir +
                                       R"(/");
        dir.append("made");
        make_dir(dir, 493);
        if (not is_dir(dir)) { return -2; }

        var inner = String(a, ")" + testDir +
                                       R"(/made/");
        inner.append("one.txt");
        var body = String(a, "1");
        write_string(inner, body);

        var entries: Vec<DirEntry> = read_dir(a, dir);
        var count: i32 = _convert<i32>(entries.size());

        var renamed = String(a, ")" + testDir +
                                       R"(/made/");
        renamed.append("two.txt");
        rename_file(inner, renamed);
        if (exists(inner)) { return -3; }

        remove_file(renamed);
        remove_dir(dir);
        if (exists(dir)) { return -4; }
        return count;
    }

    function main() i32 {
        var a = make_heap_allocator();
        try { return run(a); } catch (e: IError) { return -1; }
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST_F(Stdlib_Io_Paths, literals_still_pick_the_static_ptr_overload) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function run(a: ref HeapAllocator) i32 throws IError {
        var body = String(a, "literal path");
        write_string(")" + testDir + R"(/literal.txt", body);
        if (not exists(")" + testDir + R"(/literal.txt")) { return -2; }
        var back: String = read_to_string(a, ")" +
                                       testDir + R"(/literal.txt");
        remove_file(")" + testDir + R"(/literal.txt");
        return _convert<i32>(back.length());
    }

    function main() i32 {
        var a = make_heap_allocator();
        try { return run(a); } catch (e: IError) { return -1; }
    }
  )");
  EXPECT_EQ(value, 12);
}
