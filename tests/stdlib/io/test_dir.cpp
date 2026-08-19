// tests/stdlib/io/test_dir.cpp - Directory listing and file metadata
//
// read_dir answers "what is in here, and what kind is it" from one syscall
// per entry, so applications never touch getdents64 records themselves.

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "execution_utils.h"

class Stdlib_Io_Dir : public ::testing::Test {
 protected:
  std::string testDir;

  void SetUp() override {
    // Unique per process so parallel ctest runs stay isolated.
    testDir = "/tmp/sun_dir_tests_" + std::to_string(getpid());
    std::filesystem::remove_all(testDir);
    std::filesystem::create_directories(testDir);
  }

  void TearDown() override { std::filesystem::remove_all(testDir); }

  void writeFile(const std::string& name, const std::string& contents) {
    std::ofstream out(testDir + "/" + name);
    out << contents;
  }
};

TEST_F(Stdlib_Io_Dir, read_dir_lists_entries_without_dot_and_dotdot) {
  writeFile("a.txt", "aaa");
  writeFile("b.txt", "bb");
  std::filesystem::create_directories(testDir + "/sub");

  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function count(a: ref HeapAllocator) i32, IError {
        var entries = read_dir(a, ")" + testDir + R"(");
        var files: i32 = 0;
        var dirs: i32 = 0;
        for (var e: DirEntry in entries) {
            // "." and ".." are excluded, so nothing starts with a dot here
            if (e.name().at(0) == 46) { return -100; }
            if (e.is_dir()) { dirs = dirs + 1; }
            if (e.is_file()) { files = files + 1; }
        }
        return dirs * 100 + files;
    }

    function main() i32 {
        var a = make_heap_allocator();
        try { return count(a); } catch (e: IError) { return -1; }
    }
  )");
  EXPECT_EQ(value, 102);  // one directory, two files
}

TEST_F(Stdlib_Io_Dir, read_dir_finds_a_named_entry) {
  writeFile("needle.txt", "x");

  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function find(a: ref HeapAllocator) bool, IError {
        var entries = read_dir(a, ")" + testDir + R"(");
        for (var e: DirEntry in entries) {
            if (e.name().equals_literal("needle.txt")) { return true; }
        }
        return false;
    }

    function main() i32 {
        var a = make_heap_allocator();
        try { if (find(a)) { return 1; } } catch (e: IError) { return -1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 1);
}

TEST_F(Stdlib_Io_Dir, read_dir_on_empty_directory) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function size_of(a: ref HeapAllocator) i64, IError {
        var entries = read_dir(a, ")" + testDir + R"(");
        return entries.size();
    }

    function main() i32 {
        var a = make_heap_allocator();
        try { return _convert<i32>(size_of(a)); } catch (e: IError) { return -1; }
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Io_Dir, read_dir_on_missing_directory_throws) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var entries = read_dir(a, ")" + testDir + R"(/nope");
            return _convert<i32>(entries.size());
        } catch (e: IError) {
            return 99;
        }
    }
  )");
  EXPECT_EQ(value, 99);
}

TEST_F(Stdlib_Io_Dir, metadata_predicates) {
  writeFile("file.txt", "12345");
  std::filesystem::create_directories(testDir + "/adir");

  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function main() i32 {
        if (not exists(")" + testDir + R"(/file.txt")) { return 1; }
        if (exists(")" + testDir + R"(/missing")) { return 2; }
        if (not is_dir(")" + testDir + R"(/adir")) { return 3; }
        if (is_dir(")" + testDir + R"(/file.txt")) { return 4; }
        if (not is_file(")" + testDir + R"(/file.txt")) { return 5; }
        if (is_file(")" + testDir + R"(/adir")) { return 6; }
        try {
            if (file_size(")" + testDir + R"(/file.txt") != 5) { return 7; }
        } catch (e: IError) { return 8; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Io_Dir, read_dir_accepts_a_runtime_string_path) {
  writeFile("only.txt", "x");

  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function count(a: ref HeapAllocator) i64, IError {
        // A path built at runtime, not a literal
        var dir = String(a, ")" + testDir + R"(");
        var entries = read_dir(a, dir.c_str());
        return entries.size();
    }

    function main() i32 {
        var a = make_heap_allocator();
        try { return _convert<i32>(count(a)); } catch (e: IError) { return -1; }
    }
  )");
  EXPECT_EQ(value, 1);
}
