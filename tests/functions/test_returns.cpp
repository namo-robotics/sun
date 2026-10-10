/** Checks return diagnostics before any LLVM code is generated. */
#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "parsing/parser.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "support/error.h"

/** Keeps semantic-only test helpers local to this file. */
namespace {
/** Parse and analyze a complete program without invoking code generation. */
void analyzeReturns(const std::string& source) {
  std::istringstream input;
  sun::parsing::Parser parser(input);
  auto tree = parser.parseString(source);
  auto results = std::make_shared<sun::semantic_analysis::AnalysisResults>();
  sun::semantic_analysis::SemanticAnalyzer analyzer(results);
  analyzer.pipeline().run(*tree);
}

/** Require a missing-value diagnostic at the bare return's source location. */
void expectMissingReturnValue(const std::string& source,
                              const std::string& type) {
  const auto offset = source.find("return;");
  ASSERT_NE(offset, std::string::npos);
  int line = 1;
  int column = 1;
  for (size_t i = 0; i < offset; ++i) {
    if (source[i] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  }
  try {
    analyzeReturns(source);
    FAIL() << "Expected semantic analysis to reject a bare return";
  } catch (const sun::support::SunError& error) {
    EXPECT_EQ(error.getMessage(), "Return requires a value of type '" + type + "'");
    ASSERT_TRUE(error.getLocation().has_value());
    EXPECT_EQ(error.getLocation()->line, line);
    EXPECT_EQ(error.getLocation()->column, column);
  }
}
}  // namespace

/** Direct and nested returns must supply the declared Boolean result. */
TEST(Functions_Returns, RejectBareReturnsInControlFlow) {
  for (const std::string body : {
           "return;",
           "if (condition) { if (condition) { return; } } return true;",
           "if (condition) { return true; } else { return; }",
           "while (condition) { return; } return true;",
           "unsafe { return; };",
           "if (condition) { unsafe { return; }; } return true;"}) {
    SCOPED_TRACE(body);
    expectMissingReturnValue(
        "/** Exercise a reachable bare return. */\n"
        "function invalid(condition: bool) bool {\n  " + body + "\n}",
        "bool");
  }
}

/** Integer results receive the same check and report their declared type. */
TEST(Functions_Returns, RejectBareIntegerReturn) {
  expectMissingReturnValue(R"(
    /** Must provide an integer. */
    function invalid() i32 { return; }
  )", "i32");
}

/** Method bodies use the enclosing method's return type. */
TEST(Functions_Returns, RejectBareMethodReturn) {
  expectMissingReturnValue(R"(
    /** Owns a method with an invalid return. */
    class Example {
      /** Must provide a Boolean. */
      method invalid() bool { return; }
    }
  )", "bool");
}

/** Generic bodies must supply a value of their declared type parameter. */
TEST(Functions_Returns, RejectBareGenericReturn) {
  expectMissingReturnValue(R"(
    /** Must provide the declared result type. */
    function invalid<T>(value: T) T { return; }
    /** Instantiate the invalid body. */
    function main() i32 { return invalid(1); }
  )", "T");
}

/** A nested lambda checks its own result rather than the outer function's. */
TEST(Functions_Returns, RejectBareLambdaReturn) {
  expectMissingReturnValue(R"(
    /** Creates a lambda whose Boolean result is missing. */
    function outer() void {
      /** Must provide a Boolean. */
      var invalid = []() => bool { return; };
    }
  )", "bool");
}

/** Void returns remain valid, including inside a value-returning function. */
TEST(Functions_Returns, AcceptVoidAndExplicitValueReturns) {
  EXPECT_NO_THROW(analyzeReturns(R"(
    /** Allows early exits without values. */
    function empty(condition: bool) void {
      if (condition) { return; }
      unsafe { return; };
    }
    /** Returns an explicit Boolean from either branch. */
    function valid(condition: bool) bool {
      /** Uses its own void result type. */
      var empty_lambda = []() => void { return; };
      empty_lambda();
      if (condition) { return true; }
      unsafe { return false; };
    }
  )"));
}
