// tests/test_file_io.cpp - File I/O tests

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

class FileIOTest : public ::testing::Test {
 protected:
  std::string testDir;

  void SetUp() override {
    // Create a temp directory for test files
    testDir = "/tmp/sun_file_io_tests";
    std::filesystem::create_directories(testDir);
  }

  void TearDown() override {
    // Clean up test files
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

TEST_F(FileIOTest, file_open_write_close) {
  std::string path = testFile("write_test.txt");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = file_open(")" +
                             path + R"(", 1);
        file_write(fd, "Hello, File!");
        file_close(fd);
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);

  // Verify the file was written correctly
  std::string contents = readFileContents(path);
  EXPECT_EQ(contents, "Hello, File!");
}

TEST_F(FileIOTest, file_open_read_close) {
  std::string path = testFile("read_test.txt");
  writeFileContents(path, "Test content");

  // We can't easily test the read contents from Sun code without string
  // comparison So we just verify the program runs successfully
  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = file_open(")" +
                             path + R"(", 0);
        var content: raw_ptr<i8> = file_read(fd, 1024);
        file_close(fd);
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
}

TEST_F(FileIOTest, file_write_read_roundtrip) {
  std::string path = testFile("roundtrip_test.txt");

  auto value = executeString(R"(
    function main() i32 {
        // Write to file
        var fd1: i32 = file_open(")" +
                             path + R"(", 1);
        file_write(fd1, "Roundtrip data");
        file_close(fd1);
        
        // Read from file
        var fd2: i32 = file_open(")" +
                             path + R"(", 0);
        var content: raw_ptr<i8> = file_read(fd2, 1024);
        file_close(fd2);
        
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);

  // Verify file contents via C++
  std::string contents = readFileContents(path);
  EXPECT_EQ(contents, "Roundtrip data");
}

TEST_F(FileIOTest, file_write_multiple_times) {
  std::string path = testFile("multi_write.txt");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = file_open(")" +
                             path + R"(", 1);
        file_write(fd, "Line 1");
        file_write(fd, "Line 2");
        file_write(fd, "Line 3");
        file_close(fd);
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);

  std::string contents = readFileContents(path);
  EXPECT_EQ(contents, "Line 1Line 2Line 3");
}

TEST_F(FileIOTest, file_append_mode) {
  std::string path = testFile("append_test.txt");

  // First write
  auto value1 = executeString(R"(
    function main() i32 {
        var fd: i32 = file_open(")" +
                              path + R"(", 1);
        file_write(fd, "First");
        file_close(fd);
        return 0;
    };
  )");
  EXPECT_EQ(value1, 0);

  // Append (mode 2)
  auto value2 = executeString(R"(
    function main() i32 {
        var fd: i32 = file_open(")" +
                              path + R"(", 2);
        file_write(fd, "Second");
        file_close(fd);
        return 0;
    };
  )");
  EXPECT_EQ(value2, 0);

  std::string contents = readFileContents(path);
  EXPECT_EQ(contents, "FirstSecond");
}

TEST_F(FileIOTest, file_overwrite_mode) {
  std::string path = testFile("overwrite_test.txt");
  writeFileContents(path, "Original content that is long");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = file_open(")" +
                             path + R"(", 1);
        file_write(fd, "New");
        file_close(fd);
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);

  std::string contents = readFileContents(path);
  EXPECT_EQ(contents, "New");
}

TEST_F(FileIOTest, file_read_empty_file) {
  std::string path = testFile("empty.txt");
  writeFileContents(path, "");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = file_open(")" +
                             path + R"(", 0);
        var content: raw_ptr<i8> = file_read(fd, 1024);
        file_close(fd);
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
}

TEST_F(FileIOTest, file_create_new) {
  std::string path = testFile("new_file.txt");

  // Ensure file doesn't exist
  std::filesystem::remove(path);
  EXPECT_FALSE(std::filesystem::exists(path));

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = file_open(")" +
                             path + R"(", 1);
        file_write(fd, "Created!");
        file_close(fd);
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_EQ(readFileContents(path), "Created!");
}

TEST_F(FileIOTest, file_operations_with_comments) {
  std::string path = testFile("comments_test.txt");

  auto value = executeString(R"(
    function main() i32 {
        // Open file for writing
        var fd: i32 = file_open(")" +
                             path + R"(", 1);
        // Write some data
        file_write(fd, "With comments");
        // Close the file
        file_close(fd);
        // Return success
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(path), "With comments");
}

TEST_F(FileIOTest, file_read_returns_content) {
  std::string path = testFile("return_content.txt");

  auto value = executeString(R"(
    function main() raw_ptr<i8> {
        // Write test string
        var fd1: i32 = file_open(")" +
                             path + R"(", 1);
        file_write(fd1, "Hello from file!");
        file_close(fd1);
        
        // Read it back and return
        var fd2: i32 = file_open(")" +
                             path + R"(", 0);
        var content: raw_ptr<i8> = file_read(fd2, 1024);
        file_close(fd2);
        
        return content;
    };
  )");
  ;

  EXPECT_EQ(value, std::string("Hello from file!"));
}
