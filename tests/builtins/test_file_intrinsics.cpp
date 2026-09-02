// tests/builtins/test_file_intrinsics.cpp - The raw file intrinsics
//
// Drives __file_open / __file_write / __file_read / __file_close, __lseek,
// __fsync, __ftruncate, __unlink, __rename, __mkdir and __rmdir with no
// stdlib loaded, and checks the effect on disk from C++. The std.io File
// class built on top of them is tested in stdlib/io_tests.sun.

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "driver/execution_utils.h"

/*
 * A scratch directory per test process, removed with its files afterwards.
 * Unique per process so parallel ctest runs never delete each other's files.
 */
class Builtins_FileIntrinsics : public ::testing::Test {
 protected:
  std::string testDir;

  void SetUp() override {
    testDir = "/tmp/sun_file_intrinsics_" + std::to_string(getpid());
    std::filesystem::create_directories(testDir);
  }

  void TearDown() override { std::filesystem::remove_all(testDir); }

  /*
   * The path of a file named `name` inside the scratch directory.
   */
  std::string testFile(const std::string& name) { return testDir + "/" + name; }

  /*
   * Reads a whole file back as a string.
   */
  std::string readFileContents(const std::string& path) {
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  /*
   * Creates or overwrites a file with the given contents.
   */
  void writeFileContents(const std::string& path, const std::string& contents) {
    std::ofstream file(path);
    file << contents;
  }
};

// Open modes: 0 = read, 1 = write (create or truncate), 2 = append.

TEST_F(Builtins_FileIntrinsics, write_then_read_round_trip) {
  std::string path = testFile("round_trip.txt");

  auto value = executeString(R"(
    function main() raw_ptr<i8> {
        var fd1: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        unsafe { __file_write(fd1, "Hello from file!"); };
        unsafe { __file_close(fd1); };

        var fd2: i32 = unsafe { __file_open(")" +
                             path + R"(", 0); };
        var content: raw_ptr<i8> = unsafe { __file_read(fd2, 1024); };
        unsafe { __file_close(fd2); };
        return content;
    };
  )");

  EXPECT_EQ(value, std::string("Hello from file!"));
  EXPECT_EQ(readFileContents(path), "Hello from file!");
}

TEST_F(Builtins_FileIntrinsics, write_mode_truncates_existing) {
  std::string path = testFile("overwrite.txt");
  writeFileContents(path, "Original content that is long");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        unsafe { __file_write(fd, "New"); };
        unsafe { __file_close(fd); };
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(path), "New");
}

TEST_F(Builtins_FileIntrinsics, append_mode_keeps_existing) {
  std::string path = testFile("append.txt");
  writeFileContents(path, "First");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 2); };
        unsafe { __file_write(fd, "Second"); };
        unsafe { __file_close(fd); };
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(path), "FirstSecond");
}

TEST_F(Builtins_FileIntrinsics, read_empty_file) {
  std::string path = testFile("empty.txt");
  writeFileContents(path, "");

  auto value = executeString(R"(
    function main() raw_ptr<i8> {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 0); };
        var content: raw_ptr<i8> = unsafe { __file_read(fd, 1024); };
        unsafe { __file_close(fd); };
        return content;
    };
  )");

  EXPECT_EQ(value, std::string(""));
}

// whence: 0 = SEEK_SET, 2 = SEEK_END
TEST_F(Builtins_FileIntrinsics, lseek_set_and_end) {
  std::string path = testFile("lseek.txt");
  writeFileContents(path, "12345");

  auto value = executeString(R"(
    function main() i64 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 0); };
        var content: raw_ptr<i8> = unsafe { __file_read(fd, 3); };
        var back: i64 = unsafe { __lseek(fd, 0, 0); };
        if (back != 0) { return -1; }
        var at_two: i64 = unsafe { __lseek(fd, 2, 0); };
        if (at_two != 2) { return -2; }
        var size: i64 = unsafe { __lseek(fd, 0, 2); };
        unsafe { __file_close(fd); };
        return size;
    };
  )");

  EXPECT_EQ(value, 5);
}

TEST_F(Builtins_FileIntrinsics, fsync_returns_zero) {
  std::string path = testFile("fsync.txt");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        unsafe { __file_write(fd, "data to sync"); };
        var result: i32 = unsafe { __fsync(fd); };
        unsafe { __file_close(fd); };
        return result;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(path), "data to sync");
}

TEST_F(Builtins_FileIntrinsics, ftruncate_shrinks_and_extends) {
  std::string shrunk = testFile("shrink.txt");
  std::string extended = testFile("extend.txt");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             shrunk + R"(", 1); };
        unsafe { __file_write(fd, "Hello World!"); };
        if (unsafe { __ftruncate(fd, 5); } != 0) { return 1; }
        unsafe { __file_close(fd); };

        var fd2: i32 = unsafe { __file_open(")" +
                             extended + R"(", 1); };
        unsafe { __file_write(fd2, "Hi"); };
        if (unsafe { __ftruncate(fd2, 10); } != 0) { return 2; }
        unsafe { __file_close(fd2); };
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(shrunk), "Hello");
  std::string contents = readFileContents(extended);
  EXPECT_EQ(contents.size(), 10u);
  EXPECT_EQ(contents.substr(0, 2), "Hi");
}

TEST_F(Builtins_FileIntrinsics, unlink_removes_file_and_reports_missing) {
  std::string path = testFile("to_delete.txt");
  writeFileContents(path, "delete me");
  ASSERT_TRUE(std::filesystem::exists(path));

  auto value = executeString(R"(
    function main() i32 {
        if (unsafe { __unlink(")" +
                             path + R"("); } != 0) { return 1; }
        // Gone now, so a second unlink fails with a negative code
        if (unsafe { __unlink(")" +
                             path + R"("); } >= 0) { return 2; }
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(Builtins_FileIntrinsics, rename_moves_file) {
  std::string oldPath = testFile("old_name.txt");
  std::string newPath = testFile("new_name.txt");
  writeFileContents(oldPath, "rename test");

  auto value = executeString(R"(
    function main() i32 {
        return unsafe { __rename(")" +
                             oldPath + R"(", ")" + newPath + R"("); };
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_FALSE(std::filesystem::exists(oldPath));
  EXPECT_EQ(readFileContents(newPath), "rename test");
}

TEST_F(Builtins_FileIntrinsics, mkdir_rmdir_and_nonempty_rmdir_fails) {
  std::string emptyDir = testFile("empty_dir");
  std::string fullDir = testFile("full_dir");
  std::filesystem::create_directory(fullDir);
  writeFileContents(fullDir + "/file.txt", "content");

  // 493 is 0755
  auto value = executeString(R"(
    function main() i32 {
        if (unsafe { __mkdir(")" +
                             emptyDir + R"(", 493); } != 0) { return 1; }
        if (unsafe { __rmdir(")" +
                             emptyDir + R"("); } != 0) { return 2; }
        // A directory with something in it cannot be removed
        if (unsafe { __rmdir(")" +
                             fullDir + R"("); } >= 0) { return 3; }
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_FALSE(std::filesystem::exists(emptyDir));
  EXPECT_TRUE(std::filesystem::exists(fullDir));
}
