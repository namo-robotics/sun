#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "driver/execution_utils.h"
#include "moon_bundling/moon_builder.h"
#include "support/error.h"

namespace {

/** Write independent source files for one import-scoping test. */
std::vector<std::string> writeImportSources(
    const std::vector<std::string>& sources) {
  const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
  auto dir = std::filesystem::current_path() / "tmp" / "file_import_tests" /
             test->name();
  std::filesystem::create_directories(dir);
  std::vector<std::string> paths;
  for (size_t i = 0; i < sources.size(); ++i) {
    auto path = dir / (std::to_string(i) + ".sun");
    std::ofstream(path) << sources[i];
    paths.push_back(path.string());
  }
  return paths;
}

/** Verify rejection is independent of source-file ordering. */
void rejectBothOrders(std::vector<std::string> paths) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileFiles(paths), "Unknown");
  std::reverse(paths.begin(), paths.end());
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileFiles(paths), "Unknown");
}

}  // namespace

TEST(Modules_FileImports, top_level_import_does_not_reach_another_file) {
  for (const auto& import : {"using lib;", "using lib.answer;"}) {
    auto paths =
        writeImportSources({std::string(import) + R"(
          public module lib {
            public function answer() i32 { return 42; }
          }
          function local() i32 { return answer(); }
        )",
                            "function main() i32 { return answer(); }"});
    rejectBothOrders(paths);
  }
}

TEST(Modules_FileImports, merged_module_import_does_not_reach_another_file) {
  auto paths = writeImportSources({R"(
    public module lib {
      public class Value { var n: i32; }
    }
    public module app {
      using lib;
      function local(v: ref Value) i32 { return 1; }
    }
  )",
                                   R"(
    public module app {
      module inner {
        class Holder { var value: Value; }
      }
    }
    function main() i32 { return 0; }
  )"});
  rejectBothOrders(paths);
}

TEST(Modules_FileImports,
     imports_reach_same_file_fields_signatures_and_bodies) {
  auto paths = writeImportSources({R"(
    public module lib {
      public class Value { var n: i32; }
      public function answer() i32 { return 42; }
    }
  )",
                                   R"(
    public module app {
      module inner {
        class Holder { var value: Value; }
        function take(v: ref Value) i32 { return answer(); }
      }
    }
    using lib;
    function main() i32 { return answer(); }
  )"});
  EXPECT_NO_THROW(compileFiles(paths));
  std::reverse(paths.begin(), paths.end());
  EXPECT_NO_THROW(compileFiles(paths));
}

TEST(Modules_FileImports, distinct_imports_do_not_create_cross_file_ambiguity) {
  auto paths = writeImportSources({R"(
    public module left {
      public class Value { var n: i32; }
    }
    using left;
    public module app {
      function first(v: ref Value) i32 { return 1; }
    }
  )",
                                   R"(
    public module right {
      public class Value { var n: i64; }
    }
    using right;
    public module app {
      function second(v: ref Value) i32 { return 2; }
    }
    function main() i32 { return 0; }
  )"});
  EXPECT_NO_THROW(compileFiles(paths));
  std::reverse(paths.begin(), paths.end());
  EXPECT_NO_THROW(compileFiles(paths));
}

TEST(Modules_FileImports, qualified_names_and_shared_declarations_still_work) {
  auto paths = writeImportSources({R"(
    public module lib { public function answer() i32 { return 42; } }
    public module app { function first() i32 { return lib.answer(); } }
  )",
                                   R"(
    public module app { public function second() i32 { return first(); } }
    function main() i32 { return app.second(); }
  )"});
  EXPECT_NO_THROW(compileFiles(paths));
}

TEST(Modules_FileImports, generic_definitions_keep_their_file_imports) {
  auto paths = writeImportSources({R"(
    public module lib {
      public class Value { var n: i32; }
      public function answer() i32 { return 42; }
    }
    using lib;
    public module app {
      public class Box<T> {
        var value: Value;
        public method get() i32 { return answer(); }
        public method generic<U>() i32 { return answer(); }
      }
      public enum Choice<T> { Empty, Item(Value) }
      public interface View<T> { method get(v: ref Value) i32; }
      public function result<T>() i32 { return answer(); }
      public function consume<T>(v: ref Box<T>) i32 { return v.get(); }
    }
  )",
                                   R"(
    public module other { public function answer() i32 { return 7; } }
    using other;
    function inspect(v: ref app.Box<i32>, c: app.Choice<i32>,
                     view: ref app.View<i32>) i32 {
      return v.generic<i64>() + app.consume<i32>(v);
    }
    function main() i32 { return app.result<i32>(); }
  )"});
  EXPECT_NO_THROW(compileFiles(paths));
  std::reverse(paths.begin(), paths.end());
  EXPECT_NO_THROW(compileFiles(paths));
}

TEST(Modules_FileImports, caller_import_cannot_repair_a_generic_definition) {
  auto paths = writeImportSources({R"(
    public module lib { public function answer() i32 { return 42; } }
    public module app {
      public function result<T>() i32 { return answer(); }
    }
  )",
                                   R"(
    using lib;
    function main() i32 { return app.result<i32>(); }
  )"});
  rejectBothOrders(paths);
}

TEST(Modules_FileImports, imports_preserve_lexical_scope) {
  EXPECT_THROW(executeString(R"(
    public module lib { public function answer() i32 { return 42; } }
    module app { using lib; function local() i32 { return answer(); } }
    function main() i32 { return answer(); }
  )"),
               SunError);
  EXPECT_THROW(executeString(R"(
    public module lib { public function answer() i32 { return 42; } }
    function local() i32 { using lib; return answer(); }
    function main() i32 { return answer(); }
  )"),
               SunError);
}

TEST(Modules_FileImports,
     bundle_keeps_file_imports_and_generic_definition_context) {
  auto paths = writeImportSources({R"(
    manifest { source_files: ["1.sun", "2.sun"] }
  )",
                                   R"(
    public module left {
      public function answer() i32 { return 40; }
      public class Value { var n: i32; }
    }
    using left.answer;
    using left.Value;
    public module api {
      public class Holder { var value: Value; }
      public function first<T>() i32 { return answer(); }
    }
  )",
                                   R"(
    public module right { public function answer() i32 { return 2; } }
    using right;
    public module api {
      public function second<T>() i32 { return answer(); }
    }
  )"});
  initTestEnvironment();
  auto moon = std::filesystem::path(paths[0]).parent_path() / "library.moon";
  ASSERT_NO_THROW(sun::MoonBuilder::build(paths[0], moon));
  auto driver = Driver::createForJIT("file_import_bundle");
  driver->setMoonImports({sun::MoonImport(moon.string())});
  EXPECT_EQ(driver->executeString(R"(
    public module caller { public function answer() i32 { return 99; } }
    using caller;
    function main() i32 { return api.first<i32>() + api.second<i32>(); }
  )"),
            42);
}

TEST(Modules_FileImports, interpolation_keeps_enclosing_file_imports) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    using std;
    public module lib { public function answer() i32 { return 42; } }
    using lib;
    function main() i32 {
      var text = `${answer()}`;
      return text.length();
    }
  )"),
            2);
}

TEST(Modules_FileImports, different_bundles_remap_overlapping_file_ids) {
  auto paths = writeImportSources({R"(
    public module left_impl { public function answer() i32 { return 40; } }
    using left_impl;
    public module left_api {
      public function result<T>() i32 { return answer(); }
    }
  )",
                                   R"(
    public module right_impl { public function answer() i32 { return 2; } }
    using right_impl;
    public module right_api {
      public function result<T>() i32 { return answer(); }
    }
  )"});
  initTestEnvironment();
  auto dir = std::filesystem::path(paths[0]).parent_path();
  auto left = dir / "left.moon";
  auto right = dir / "right.moon";
  ASSERT_NO_THROW(sun::MoonBuilder::build(paths[0], left));
  ASSERT_NO_THROW(sun::MoonBuilder::build(paths[1], right));
  auto driver = Driver::createForJIT("separate_bundle_file_ids");
  driver->setMoonImports(
      {sun::MoonImport(left.string()), sun::MoonImport(right.string())});
  EXPECT_EQ(driver->executeString(R"(
    function main() i32 {
      return left_api.result<i32>() + right_api.result<i32>();
    }
  )"),
            42);
}

TEST(Modules_FileImports, same_file_ambiguity_is_still_reported) {
  EXPECT_THROW(executeString(R"(
    public module left { public class Value { var n: i32; } }
    public module right { public class Value { var n: i64; } }
    using left;
    using right;
    function take(v: ref Value) i32 { return 0; }
    function main() i32 { return 0; }
  )"),
               SunError);
}

TEST(Modules_FileImports, targeted_import_does_not_import_other_symbols) {
  EXPECT_THROW(executeString(R"(
    public module lib {
      public function first() i32 { return 1; }
      public function second() i32 { return 2; }
    }
    using lib.first;
    function main() i32 { return second(); }
  )"),
               SunError);
}
