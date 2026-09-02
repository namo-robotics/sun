// tests/tooling/lsp/test_tests.cpp — Listing test functions for the editor
//
// collectTests backs the sun/tests request the test explorer sends: it walks
// an analyzed program and reports each test_function in one document with
// the dotted name the runner's --test-filter matches and the range of the
// name token.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "ast.h"
#include "driver/driver.h"
#include "driver/execution_utils.h"
#include "lsp/tests.h"

namespace {

// The file never exists on disk; nodes carry the path exactly as given
const char* kPath = "/tests_test.sun";

struct Analysis {
  std::unique_ptr<Driver> driver;
  Driver::AnalyzedProgram program;
};

Analysis analyze(const std::string& source, bool withStdlib = false) {
  initTestEnvironment();
  Analysis analysis;
  analysis.driver = Driver::createForAOT("tests_test");
  if (withStdlib) analysis.driver->setMoonImports(getStdlibMoonImports());
  analysis.program = analysis.driver->analyzeString(source, kPath);
  return analysis;
}

}  // namespace

// A root-level test is reported under its bare name, and its range covers
// exactly the name token.
TEST(Tooling_Lsp_Tests, finds_a_root_level_test) {
  const std::string source = R"(test_function checksNothing() {
    return;
}
)";
  auto analysis = analyze(source);
  ASSERT_NE(analysis.program.ast, nullptr);
  auto tests = sun::lsp::collectTests(*analysis.program.ast, kPath, source);

  ASSERT_EQ(tests.size(), 1u);
  EXPECT_EQ(tests[0].id, "checksNothing");
  EXPECT_EQ(tests[0].label, "checksNothing");
  const std::string& name = "checksNothing";
  size_t at = source.find(name);
  EXPECT_EQ(tests[0].location.range.offset, static_cast<int>(at));
  EXPECT_EQ(tests[0].location.range.endOffset,
            static_cast<int>(at + name.size()));
}

// Tests inside modules get the dotted names the runner uses, walking nested
// modules the same way.
TEST(Tooling_Lsp_Tests, nested_modules_make_dotted_names) {
  const std::string source = R"(module outer {
    module inner {
        test_function deepTest() {
            return;
        }
    }

    test_function shallowTest() {
        return;
    }
}
)";
  auto analysis = analyze(source);
  ASSERT_NE(analysis.program.ast, nullptr);
  auto tests = sun::lsp::collectTests(*analysis.program.ast, kPath, source);

  ASSERT_EQ(tests.size(), 2u);
  EXPECT_EQ(tests[0].id, "outer.inner.deepTest");
  EXPECT_EQ(tests[0].label, "deepTest");
  EXPECT_EQ(tests[1].id, "outer.shallowTest");
  EXPECT_EQ(tests[1].label, "shallowTest");
}

// Ordinary functions and methods are not test items.
TEST(Tooling_Lsp_Tests, non_test_functions_are_excluded) {
  const std::string source = R"(function helper() i32 {
    return 1;
}

module m {
    function alsoNot() i32 {
        return 2;
    }

    test_function only() {
        return;
    }
}

function main() i32 {
    return 0;
}
)";
  auto analysis = analyze(source);
  ASSERT_NE(analysis.program.ast, nullptr);
  auto tests = sun::lsp::collectTests(*analysis.program.ast, kPath, source);

  ASSERT_EQ(tests.size(), 1u);
  EXPECT_EQ(tests[0].id, "m.only");
}

// The workspace-wide variant reports every test with its file, so the test
// explorer can build the whole tree from one analyzed entrypoint.
TEST(Tooling_Lsp_Tests, spans_carry_dotted_names_and_files) {
  const std::string source = R"(module outer {
    test_function nested() {
        return;
    }
}

test_function atRoot() {
    return;
}
)";
  auto analysis = analyze(source);
  ASSERT_NE(analysis.program.ast, nullptr);
  auto spans = sun::lsp::collectTestSpans(*analysis.program.ast);

  ASSERT_EQ(spans.size(), 2u);
  EXPECT_EQ(spans[0].id, "outer.nested");
  EXPECT_EQ(spans[0].label, "nested");
  EXPECT_EQ(spans[0].filePath, kPath);
  EXPECT_EQ(spans[1].id, "atRoot");
  EXPECT_EQ(spans[1].filePath, kPath);
}

// Only tests located in the requested document are returned: a manifest's
// analyzed tree spans every file, but each file's request answers for that
// file alone.
TEST(Tooling_Lsp_Tests, tests_from_other_files_are_filtered_out) {
  const std::string source = R"(test_function here() {
    return;
}
)";
  auto analysis = analyze(source);
  ASSERT_NE(analysis.program.ast, nullptr);
  auto tests = sun::lsp::collectTests(*analysis.program.ast,
                                      "/some_other_file.sun", source);
  EXPECT_EQ(tests.size(), 0u);
}
