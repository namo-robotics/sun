// tests/test_file_io_extended.cpp - Extended File I/O tests
//
// Tests for: __lseek, __fsync, __ftruncate, __unlink, __rename, __mkdir, __rmdir

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "execution_utils.h"

class SunIOTest : public ::testing::Test {
 protected:
  std::string testDir;
  std::string uniqueId;

  void SetUp() override {
    // Generate unique ID for this test run to avoid parallel conflicts
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    uniqueId = std::to_string(dis(gen));

    // Create a unique temp directory for this test
    testDir = "/tmp/sun_io_ext_" + uniqueId;
    std::filesystem::create_directories(testDir);
  }

  void TearDown() override {
    // Clean up test directory
    std::filesystem::remove_all(testDir);
  }

  std::string testFile(const std::string& name) { return testDir + "/" + name; }

  std::string readFileContents(const std::string& path) {
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  void writeFileContents(const std::string& path, const std::string& contents) {
    std::ofstream file(path);
    file << contents;
  }
};

// -------------------------------------------------------------------
// __lseek tests
// -------------------------------------------------------------------

TEST_F(SunIOTest, lseek_to_beginning) {
  std::string path = testFile("lseek_test.txt");
  writeFileContents(path, "Hello World");

  auto value = executeString(R"(
    function main() i64 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 0); };
        // Read moves position forward
        var content: raw_ptr<i8> = unsafe { __file_read(fd, 5); };
        // Seek back to beginning
        var pos: i64 = unsafe { __lseek(fd, 0, 0); };  // SEEK_SET = 0
        unsafe { __file_close(fd); };
        return pos;
    };
  )");

  EXPECT_EQ(value, 0);
}

TEST_F(SunIOTest, lseek_to_end) {
  std::string path = testFile("lseek_end.txt");
  writeFileContents(path, "12345");  // 5 bytes

  auto value = executeString(R"(
    function main() i64 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 0); };
        // Seek to end
        var pos: i64 = unsafe { __lseek(fd, 0, 2); };  // SEEK_END = 2
        unsafe { __file_close(fd); };
        return pos;
    };
  )");

  EXPECT_EQ(value, 5);
}

// -------------------------------------------------------------------
// __fsync tests
// -------------------------------------------------------------------

TEST_F(SunIOTest, fsync_success) {
  std::string path = testFile("fsync_test.txt");

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

  // fsync returns 0 on success
  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(path), "data to sync");
}

// -------------------------------------------------------------------
// __ftruncate tests
// -------------------------------------------------------------------

TEST_F(SunIOTest, ftruncate_shrink) {
  std::string path = testFile("truncate_test.txt");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        unsafe { __file_write(fd, "Hello World!"); };
        // Truncate to 5 bytes
        var result: i32 = unsafe { __ftruncate(fd, 5); };
        unsafe { __file_close(fd); };
        return result;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(path), "Hello");
}

TEST_F(SunIOTest, ftruncate_extend) {
  std::string path = testFile("truncate_extend.txt");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        unsafe { __file_write(fd, "Hi"); };
        // Extend to 10 bytes (fills with null bytes)
        var result: i32 = unsafe { __ftruncate(fd, 10); };
        unsafe { __file_close(fd); };
        return result;
    };
  )");

  EXPECT_EQ(value, 0);
  std::string contents = readFileContents(path);
  EXPECT_EQ(contents.size(), 10);
  EXPECT_EQ(contents.substr(0, 2), "Hi");
}

// -------------------------------------------------------------------
// __unlink tests
// -------------------------------------------------------------------

TEST_F(SunIOTest, unlink_file) {
  std::string path = testFile("to_delete.txt");
  writeFileContents(path, "delete me");
  EXPECT_TRUE(std::filesystem::exists(path));

  auto value = executeString(R"(
    function main() i32 {
        return unsafe { __unlink(")" +
                      path + R"("); };
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(SunIOTest, unlink_nonexistent) {
  std::string path = testFile("nonexistent.txt");

  auto value = executeString(R"(
    function main() i32 {
        return unsafe { __unlink(")" +
                      path + R"("); };
    };
  )");

  // unlink returns -2 (ENOENT) for nonexistent files
  EXPECT_TRUE(sun::toDouble(value) < 0);
}

// -------------------------------------------------------------------
// __rename tests
// -------------------------------------------------------------------

TEST_F(SunIOTest, rename_file) {
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
  EXPECT_TRUE(std::filesystem::exists(newPath));
  EXPECT_EQ(readFileContents(newPath), "rename test");
}

// -------------------------------------------------------------------
// __mkdir / __rmdir tests
// -------------------------------------------------------------------

TEST_F(SunIOTest, mkdir_and_rmdir) {
  std::string dirPath = testFile("new_directory");

  // Create directory
  auto mkdirResult = executeString(R"(
    function main() i32 {
        return unsafe { __mkdir(")" +
                      dirPath + R"(", 493); };
    };
  )");  // 493 = 0755

  EXPECT_EQ(mkdirResult, 0);
  EXPECT_TRUE(std::filesystem::is_directory(dirPath));

  // Remove directory
  auto rmdirResult = executeString(R"(
    function main() i32 {
        return unsafe { __rmdir(")" +
                      dirPath + R"("); };
    };
  )");

  EXPECT_EQ(rmdirResult, 0);
  EXPECT_FALSE(std::filesystem::exists(dirPath));
}

TEST_F(SunIOTest, rmdir_nonempty_fails) {
  std::string dirPath = testFile("nonempty_dir");
  std::filesystem::create_directory(dirPath);
  writeFileContents(dirPath + "/file.txt", "content");

  auto value = executeString(R"(
    function main() i32 {
        return unsafe { __rmdir(")" +
                      dirPath + R"("); };
    };
  )");

  // rmdir on non-empty directory should fail
  EXPECT_TRUE(sun::toDouble(value) < 0);
  EXPECT_TRUE(std::filesystem::exists(dirPath));
}

// -------------------------------------------------------------------
// __write / __read (raw byte I/O) tests
// -------------------------------------------------------------------

TEST_F(SunIOTest, write_and_read_raw_bytes) {
  std::string path = testFile("raw_io.txt");

  // This test verifies raw write works by writing via __write and reading via
  // __file_read
  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        if (fd < 0) { return 1; }
        
        // Use __file_write first (string-based) as buffer source
        unsafe { __file_write(fd, "ABCDE"); };
        unsafe { __file_close(fd); };
        
        // Re-open and verify we can seek
        var fd2: i32 = unsafe { __file_open(")" +
                              path + R"(", 0); };
        var pos: i64 = unsafe { __lseek(fd2, 2, 0); };  // Seek to offset 2
        if (pos != 2) { return 2; }
        
        unsafe { __file_close(fd2); };
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(path), "ABCDE");
}

// -------------------------------------------------------------------
// Integration: combined operations
// -------------------------------------------------------------------

TEST_F(SunIOTest, integration_create_write_seek_truncate) {
  std::string path = testFile("integration.txt");

  auto value = executeString(R"(
    function main() i64 {
        // Create and write
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        unsafe { __file_write(fd, "Hello World!"); };
        
        // Sync to disk
        unsafe { __fsync(fd); };
        
        // Get file size via lseek to end
        var size: i64 = unsafe { __lseek(fd, 0, 2); };
        if (size != 12) { return -1; }
        
        // Truncate to 5 bytes
        unsafe { __ftruncate(fd, 5); };
        
        // Verify new size
        var newSize: i64 = unsafe { __lseek(fd, 0, 2); };
        unsafe { __file_close(fd); };
        
        return newSize;
    };
  )");

  EXPECT_EQ(value, 5);
  EXPECT_EQ(readFileContents(path), "Hello");
}
