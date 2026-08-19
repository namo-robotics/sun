// tests/stdlib/io/test_file.cpp - File I/O tests

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "execution_utils.h"

class Stdlib_Io_File : public ::testing::Test {
 protected:
  std::string testDir;

  void SetUp() override {
    // Create a temp directory for test files. Unique per process so tests
    // can run under parallel ctest without deleting each other's files.
    testDir = "/tmp/sun_file_io_tests_" + std::to_string(getpid());
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

TEST_F(Stdlib_Io_File, file_open_write_close) {
  std::string path = testFile("write_test.txt");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        unsafe { __file_write(fd, "Hello, File!"); };
        unsafe { __file_close(fd); };
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);

  // Verify the file was written correctly
  std::string contents = readFileContents(path);
  EXPECT_EQ(contents, "Hello, File!");
}

TEST_F(Stdlib_Io_File, file_open_read_close) {
  std::string path = testFile("read_test.txt");
  writeFileContents(path, "Test content");

  // We can't easily test the read contents from Sun code without string
  // comparison So we just verify the program runs successfully
  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 0); };
        var content: raw_ptr<i8> = unsafe { __file_read(fd, 1024); };
        unsafe { __file_close(fd); };
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Io_File, file_write_read_roundtrip) {
  std::string path = testFile("roundtrip_test.txt");

  auto value = executeString(R"(
    function main() i32 {
        // Write to file
        var fd1: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        unsafe { __file_write(fd1, "Roundtrip data"); };
        unsafe { __file_close(fd1); };
        
        // Read from file
        var fd2: i32 = unsafe { __file_open(")" +
                             path + R"(", 0); };
        var content: raw_ptr<i8> = unsafe { __file_read(fd2, 1024); };
        unsafe { __file_close(fd2); };
        
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);

  // Verify file contents via C++
  std::string contents = readFileContents(path);
  EXPECT_EQ(contents, "Roundtrip data");
}

TEST_F(Stdlib_Io_File, file_write_multiple_times) {
  std::string path = testFile("multi_write.txt");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        unsafe { __file_write(fd, "Line 1"); };
        unsafe { __file_write(fd, "Line 2"); };
        unsafe { __file_write(fd, "Line 3"); };
        unsafe { __file_close(fd); };
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);

  std::string contents = readFileContents(path);
  EXPECT_EQ(contents, "Line 1Line 2Line 3");
}

TEST_F(Stdlib_Io_File, file_append_mode) {
  std::string path = testFile("append_test.txt");

  // First write
  auto value1 = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                              path + R"(", 1); };
        unsafe { __file_write(fd, "First"); };
        unsafe { __file_close(fd); };
        return 0;
    };
  )");
  EXPECT_EQ(value1, 0);

  // Append (mode 2)
  auto value2 = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                              path + R"(", 2); };
        unsafe { __file_write(fd, "Second"); };
        unsafe { __file_close(fd); };
        return 0;
    };
  )");
  EXPECT_EQ(value2, 0);

  std::string contents = readFileContents(path);
  EXPECT_EQ(contents, "FirstSecond");
}

TEST_F(Stdlib_Io_File, file_overwrite_mode) {
  std::string path = testFile("overwrite_test.txt");
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

  std::string contents = readFileContents(path);
  EXPECT_EQ(contents, "New");
}

TEST_F(Stdlib_Io_File, file_read_empty_file) {
  std::string path = testFile("empty.txt");
  writeFileContents(path, "");

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 0); };
        var content: raw_ptr<i8> = unsafe { __file_read(fd, 1024); };
        unsafe { __file_close(fd); };
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Io_File, file_create_new) {
  std::string path = testFile("new_file.txt");

  // Ensure file doesn't exist
  std::filesystem::remove(path);
  EXPECT_FALSE(std::filesystem::exists(path));

  auto value = executeString(R"(
    function main() i32 {
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        unsafe { __file_write(fd, "Created!"); };
        unsafe { __file_close(fd); };
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_EQ(readFileContents(path), "Created!");
}

TEST_F(Stdlib_Io_File, file_operations_with_comments) {
  std::string path = testFile("comments_test.txt");

  auto value = executeString(R"(
    function main() i32 {
        // Open file for writing
        var fd: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        // Write some data
        unsafe { __file_write(fd, "With comments"); };
        // Close the file
        unsafe { __file_close(fd); };
        // Return success
        return 0;
    };
  )");

  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(path), "With comments");
}

TEST_F(Stdlib_Io_File, file_read_returns_content) {
  std::string path = testFile("return_content.txt");

  auto value = executeString(R"(
    function main() raw_ptr<i8> {
        // Write test string
        var fd1: i32 = unsafe { __file_open(")" +
                             path + R"(", 1); };
        unsafe { __file_write(fd1, "Hello from file!"); };
        unsafe { __file_close(fd1); };
        
        // Read it back and return
        var fd2: i32 = unsafe { __file_open(")" +
                             path + R"(", 0); };
        var content: raw_ptr<i8> = unsafe { __file_read(fd2, 1024); };
        unsafe { __file_close(fd2); };
        
        return content;
    };
  )");
  ;

  EXPECT_EQ(value, std::string("Hello from file!"));
}

// ============================================================================
// The sun.io File class
// ============================================================================
// The tests above drive the __file_* intrinsics directly. These go through
// stdlib/io.sun, where a path is a raw_ptr<u8> — a literal narrows to one, and
// a runtime String supplies one through c_str().

TEST_F(Stdlib_Io_File, sun_io_file_literal_path_round_trip) {
  std::string path = testFile("literal.txt");
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            var f = File();
            f.open(")" + path + R"(", FileMode.Write);
            f.write("written by File");
            f.close();

            var text = read_to_string(a, ")" + path + R"(");
            if (not text.equals_literal("written by File")) { return 1; }
        } catch (e: IError) {
            return 2;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(path), "written by File");
}

TEST_F(Stdlib_Io_File, sun_io_file_runtime_string_path) {
  std::string path = testFile("runtime.txt");
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function main() i32 {
        var a = make_heap_allocator();
        try {
            // The path is built at runtime, which the intrinsics cannot take.
            var dir = String(a, ")" + testDir + R"(");
            dir.append_literal("/runtime.txt");

            var body = String(a, "runtime path");
            write_string(dir.c_str(), body);

            var back = read_to_string(a, dir.c_str());
            if (not back.equals_literal("runtime path")) { return 1; }
            if (file_size(dir.c_str()) != 12) { return 2; }
        } catch (e: IError) {
            return 3;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(path), "runtime path");
}

TEST_F(Stdlib_Io_File, sun_io_file_seek_and_size) {
  std::string path = testFile("seek.txt");
  writeFileContents(path, "0123456789");

  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function main() i32 {
        try {
            var f = File();
            f.open(")" + path + R"(", FileMode.Read);
            if (f.size() != 10) { return 1; }
            if (f.tell() != 0) { return 2; }
            if (f.seek(4, Whence.Start) != 4) { return 3; }
            if (f.tell() != 4) { return 4; }
            if (f.seek(0, Whence.End) != 10) { return 5; }
            f.close();
            if (f.is_open()) { return 6; }
        } catch (e: IError) {
            return 7;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST_F(Stdlib_Io_File, sun_io_append_mode_keeps_existing_content) {
  std::string path = testFile("append.txt");
  writeFileContents(path, "first;");

  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function main() i32 {
        try {
            var f = File();
            f.open(")" + path + R"(", FileMode.Append);
            f.write("second");
            f.close();
        } catch (e: IError) {
            return 1;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
  EXPECT_EQ(readFileContents(path), "first;second");
}

TEST_F(Stdlib_Io_File, sun_io_open_missing_file_throws) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function main() i32 {
        var f = File();
        try {
            f.open(")" + testFile("nope.txt") + R"(", FileMode.Read);
        } catch (e: IError) {
            return 42;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 42);
}

TEST_F(Stdlib_Io_File, sun_io_remove_rename_and_directories) {
  std::string path = testFile("victim.txt");
  writeFileContents(path, "x");

  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.io;

    function main() i32 {
        try {
            rename_file(")" + path + R"(", ")" + testFile("renamed.txt") + R"(");
            if (exists(")" + path + R"(")) { return 1; }
            if (not exists(")" + testFile("renamed.txt") + R"(")) { return 2; }
            remove_file(")" + testFile("renamed.txt") + R"(");
            if (exists(")" + testFile("renamed.txt") + R"(")) { return 3; }

            make_dir(")" + testFile("newdir") + R"(", 493);
            if (not is_dir(")" + testFile("newdir") + R"(")) { return 4; }
            remove_dir(")" + testFile("newdir") + R"(");
            if (exists(")" + testFile("newdir") + R"(")) { return 5; }
        } catch (e: IError) {
            return 6;
        }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}
