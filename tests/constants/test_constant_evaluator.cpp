// tests/constants/test_constant_evaluator.cpp
// Tests for compile-time evaluation of file-scope initializers.
//
// A value the compiler computes must be the value the same expression
// produces when the program runs. Most tests here therefore evaluate an
// expression twice: once as a file-scope constant, reading the result out of
// the analysis tables, and once inside `main` from run-time variables, where
// nothing can be folded. The run-time program compares its result, bit for
// bit, against the compile-time one.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "driver/execution_utils.h"
#include "semantic_analysis/constants/global_init_table.h"
#include "semantic_analysis/semantic_analyzer.h"

using sun::ast::ASTNodeType;
using sun::driver::Driver;
using sun::semantic_analysis::constants::ConstantValue;
using sun::semantic_analysis::constants::GlobalInitKind;
using sun::semantic_analysis::constants::GlobalInitRecord;

/** Keeps test fixtures and helpers local to this source file. */
namespace {

/** A named, typed operand: a constant at compile time, a variable at run time.
 */
struct Operand {
  std::string name;
  std::string type;
  std::string value;
};

/** Finds a file-scope variable by name, looking inside modules. */
const sun::ast::VariableCreationAST* findGlobal(
    const sun::ast::BlockExprAST& block, const std::string& name) {
  for (const auto& node : block.getBody()) {
    if (node->getType() == ASTNodeType::MODULE) {
      const auto& body =
          static_cast<const sun::ast::ModuleAST&>(*node).getBody();
      if (const auto* found = findGlobal(body, name)) return found;
    }
    if (node->getType() != ASTNodeType::VARIABLE_CREATION) continue;
    const auto& global =
        static_cast<const sun::ast::VariableCreationAST&>(*node);
    if (global.getName() == name) return &global;
  }
  return nullptr;
}

/** Thrown to end a compilation once analysis has produced its decisions. */
struct AnalysisFinished {};

/**
 * Analyzes a program and returns what was decided about one of its
 * file-scope variables. Fails the test when the variable has no decision.
 * Compilation stops after analysis, so a program is usable here even when
 * code generation could not yet handle it.
 */
GlobalInitRecord decideGlobal(const std::string& source,
                              const std::string& name) {
  GlobalInitRecord decision;
  bool found = false;
  auto driver = Driver::createForJIT("constant_evaluator_test");
  driver->setMetadataCallback(
      [&](const sun::ast::BlockExprAST& program,
          sun::semantic_analysis::SemanticAnalyzer& analyzer) {
        const auto* global = findGlobal(program, name);
        const auto* record =
            global ? analyzer.context().results().globalInits.find(
                         global->getDeclarationId())
                   : nullptr;
        if (record) {
          decision = *record;
          found = true;
        }
        throw AnalysisFinished{};
      });
  try {
    driver->executeString(source + "\nfunction main() i32 { return 0; }\n");
  } catch (const AnalysisFinished&) {
  }
  EXPECT_TRUE(found) << "no decision recorded for '" << name << "'";
  return decision;
}

/**
 * A Sun condition that is true exactly when the run-time variable `RESULT`
 * holds the same bits as the compile-time value.
 */
std::string buildBitComparison(const ConstantValue& value) {
  if (value.type->isBool())
    return std::string("RESULT == ") +
           (value.getInteger().isZero() ? "false" : "true");
  if (value.isInteger()) {
    // Widen both sides to 64 bits the same way, by the type's signedness.
    const auto& bits = value.getInteger();
    uint64_t wide = value.isUnsigned()
                        ? bits.getZExtValue()
                        : static_cast<uint64_t>(bits.getSExtValue());
    return "_convert<u64>(RESULT) == " + std::to_string(wide) + "u64";
  }
  uint64_t bits = value.getFloat().bitcastToAPInt().getZExtValue();
  return std::string("_bitcast<") + (value.type->isFloat32() ? "u32" : "u64") +
         ">(RESULT) == " + std::to_string(bits) +
         (value.type->isFloat32() ? "u32" : "u64");
}

/**
 * Evaluates `expression` over the operands at compile time and at run time
 * and expects the same result. The expression names the operands.
 */
void expectSameAtCompileTimeAndRunTime(const std::string& resultType,
                                       const std::vector<Operand>& operands,
                                       const std::string& expression) {
  SCOPED_TRACE(resultType + " = " + expression);
  std::string constants, variables;
  for (const auto& operand : operands) {
    SCOPED_TRACE(operand.name + ": " + operand.type + " = " + operand.value);
    constants += "const " + operand.name + ": " + operand.type + " = " +
                 operand.value + ";\n";
    variables += "  var " + operand.name + ": " + operand.type + " = " +
                 operand.value + ";\n";
  }

  GlobalInitRecord decision = decideGlobal(
      constants + "const RESULT: " + resultType + " = " + expression + ";\n",
      "RESULT");
  ASSERT_EQ(decision.kind, GlobalInitKind::Image)
      << "not evaluated at compile time: "
      << (decision.reason ? decision.reason->message : "");
  ASSERT_TRUE(decision.value.has_value());

  std::string program = "function main() i32 {\n" + variables +
                        "  var RESULT: " + resultType + " = " + expression +
                        ";\n  if (" + buildBitComparison(*decision.value) +
                        ") { return 0; }\n  return 1;\n}\n";
  EXPECT_EQ(sun::driver::executeString(program), 0)
      << "compile time gave " << decision.value->toDisplayString()
      << ", run time disagreed:\n"
      << program;
}

/** Expects a global to be left to the startup function, for a stated reason. */
void expectInitializedAtStartup(const std::string& source,
                                const std::string& name,
                                const std::string& reasonFragment) {
  SCOPED_TRACE(source);
  GlobalInitRecord decision = decideGlobal(source, name);
  EXPECT_EQ(decision.kind, GlobalInitKind::Startup);
  EXPECT_FALSE(decision.value.has_value());
  ASSERT_TRUE(decision.reason.has_value());
  EXPECT_NE(decision.reason->message.find(reasonFragment), std::string::npos)
      << "reason was: " << decision.reason->message;
}

/** The decision's value rendered as text, or the reason it has none. */
std::string describeDecision(const GlobalInitRecord& decision) {
  if (decision.value) return decision.value->toDisplayString();
  return "startup: " + (decision.reason ? decision.reason->message : "");
}

}  // namespace

// === Values ===

TEST(Constants_Evaluator, literals_and_reads_of_earlier_constants) {
  EXPECT_EQ(describeDecision(decideGlobal("const A: i64 = 4;", "A")), "4");
  EXPECT_EQ(describeDecision(decideGlobal("const A = 2;", "A")), "2");
  EXPECT_EQ(describeDecision(decideGlobal("const A: bool = true;", "A")),
            "true");
  EXPECT_EQ(describeDecision(decideGlobal("const A = \"sun\";", "A")),
            "\"sun\"");
  EXPECT_EQ(describeDecision(decideGlobal(
                "const A: i64 = 4;\nconst B: i64 = A * 2 + 1;", "B")),
            "9");
  // A narrower constant widens to the type it is stored as.
  EXPECT_EQ(describeDecision(
                decideGlobal("const N: i32 = -3;\nconst W: i64 = N;", "W")),
            "-3");
  EXPECT_EQ(describeDecision(
                decideGlobal("const N: u8 = 200u8;\nconst W: u64 = N;", "W")),
            "200");
}

TEST(Constants_Evaluator, a_var_gets_its_own_value_in_the_image) {
  GlobalInitRecord decision = decideGlobal("var LIMIT: i64 = 6 * 7;", "LIMIT");
  EXPECT_EQ(decision.kind, GlobalInitKind::Image);
  EXPECT_FALSE(decision.isConst);
  EXPECT_EQ(describeDecision(decision), "42");
}

TEST(Constants_Evaluator, module_constants_and_qualified_reads) {
  const std::string source = R"(
    module limits {
      public const BASE: i64 = 10;
      public const DOUBLE: i64 = BASE * 2;
    }
    const TRIPLE: i64 = limits.BASE * 3;
  )";
  EXPECT_EQ(describeDecision(decideGlobal(source, "DOUBLE")), "20");
  EXPECT_EQ(describeDecision(decideGlobal(source, "TRIPLE")), "30");
}

TEST(Constants_Evaluator, arrays_of_constant_expressions) {
  EXPECT_EQ(
      describeDecision(decideGlobal(
          "const N: i32 = 5;\nconst A: array<i32, 3> = [N, N + 1, 0];", "A")),
      "[5, 6, 0]");
  EXPECT_EQ(describeDecision(decideGlobal(
                "const G: array<i32, 2, 2> = [[1, 2], [3, 4]];", "G")),
            "[[1, 2], [3, 4]]");
}

TEST(Constants_Evaluator, payload_free_enum_variants) {
  EXPECT_EQ(describeDecision(decideGlobal(
                "enum Color { Red, Green, Blue }\nconst C: Color = Color.Blue;",
                "C")),
            "2");
}

TEST(Constants_Evaluator, ternary_and_logic_evaluate_only_what_runs) {
  // The side not taken divides by zero, which would otherwise be refused.
  EXPECT_EQ(describeDecision(decideGlobal(
                "const Z: i64 = 0;\nconst A: i64 = Z == 0 ? 7 : 1 / Z;", "A")),
            "7");
  EXPECT_EQ(
      describeDecision(decideGlobal(
          "const Z: i64 = 0;\nconst A: bool = Z != 0 and 1 / Z > 0;", "A")),
      "false");
  EXPECT_EQ(
      describeDecision(decideGlobal(
          "const Z: i64 = 0;\nconst A: bool = Z == 0 or 1 / Z > 0;", "A")),
      "true");
}

// === Same result at compile time and at run time ===

TEST(Constants_Evaluator, integer_arithmetic_matches_run_time) {
  for (const char* type : {"i8", "i16", "i32", "i64"}) {
    for (const char* op : {"+", "-", "*", "/", "%", "&", "|", "^"}) {
      expectSameAtCompileTimeAndRunTime(type,
                                        {{"A", type, "-7"}, {"B", type, "3"}},
                                        std::string("A ") + op + " B");
      expectSameAtCompileTimeAndRunTime(type,
                                        {{"A", type, "100"}, {"B", type, "-9"}},
                                        std::string("A ") + op + " B");
    }
  }
  for (const char* type : {"u8", "u16", "u32", "u64"}) {
    for (const char* op : {"+", "-", "*", "/", "%", "&", "|", "^"}) {
      expectSameAtCompileTimeAndRunTime(type,
                                        {{"A", type, "200"}, {"B", type, "7"}},
                                        std::string("A ") + op + " B");
    }
  }
}

TEST(Constants_Evaluator, integer_overflow_wraps_as_at_run_time) {
  expectSameAtCompileTimeAndRunTime(
      "i8", {{"A", "i8", "127"}, {"B", "i8", "1"}}, "A + B");
  expectSameAtCompileTimeAndRunTime(
      "i8", {{"A", "i8", "-128"}, {"B", "i8", "1"}}, "A - B");
  expectSameAtCompileTimeAndRunTime("u8", {{"A", "u8", "0"}, {"B", "u8", "1"}},
                                    "A - B");
  expectSameAtCompileTimeAndRunTime(
      "i32", {{"A", "i32", "2147483647"}, {"B", "i32", "2"}}, "A * B");
  expectSameAtCompileTimeAndRunTime(
      "i64", {{"A", "i64", "9223372036854775807"}, {"B", "i64", "1"}}, "A + B");
  expectSameAtCompileTimeAndRunTime(
      "u64", {{"A", "u64", "18446744073709551615"}, {"B", "u64", "2"}},
      "A * B");
  expectSameAtCompileTimeAndRunTime("i8", {{"A", "i8", "-128"}}, "-A");
}

TEST(Constants_Evaluator, shifts_follow_the_left_operands_signedness) {
  expectSameAtCompileTimeAndRunTime(
      "i32", {{"A", "i32", "-16"}, {"B", "i32", "2"}}, "A >> B");
  expectSameAtCompileTimeAndRunTime(
      "u32", {{"A", "u32", "4294967280"}, {"B", "u32", "2"}}, "A >> B");
  expectSameAtCompileTimeAndRunTime(
      "i32", {{"A", "i32", "1"}, {"B", "i32", "31"}}, "A << B");
  expectSameAtCompileTimeAndRunTime(
      "u8", {{"A", "u8", "255"}, {"B", "u8", "7"}}, "A << B");
  expectSameAtCompileTimeAndRunTime(
      "i64", {{"A", "i64", "-1"}, {"B", "i64", "63"}}, "A >> B");
}

TEST(Constants_Evaluator, comparisons_follow_the_left_operands_signedness) {
  for (const char* op : {"<", "<=", ">", ">=", "==", "!="}) {
    expectSameAtCompileTimeAndRunTime("bool",
                                      {{"A", "i32", "-1"}, {"B", "i32", "1"}},
                                      std::string("A ") + op + " B");
    expectSameAtCompileTimeAndRunTime(
        "bool", {{"A", "u32", "4294967295"}, {"B", "u32", "1"}},
        std::string("A ") + op + " B");
    expectSameAtCompileTimeAndRunTime("bool",
                                      {{"A", "i64", "5"}, {"B", "i64", "5"}},
                                      std::string("A ") + op + " B");
  }
}

TEST(Constants_Evaluator, mixed_widths_widen_by_each_operands_signedness) {
  expectSameAtCompileTimeAndRunTime(
      "i64", {{"A", "i32", "-7"}, {"B", "i64", "5000000000"}}, "A + B");
  expectSameAtCompileTimeAndRunTime(
      "i64", {{"A", "i64", "5000000000"}, {"B", "i8", "-3"}}, "A * B");
  expectSameAtCompileTimeAndRunTime(
      "u64", {{"A", "u8", "250"}, {"B", "u64", "10"}}, "A + B");
  expectSameAtCompileTimeAndRunTime(
      "bool", {{"A", "i8", "-1"}, {"B", "i64", "-1"}}, "A == B");
  expectSameAtCompileTimeAndRunTime(
      "bool", {{"A", "i16", "-2"}, {"B", "i64", "1"}}, "A < B");
}

TEST(Constants_Evaluator, unary_operators_match_run_time) {
  expectSameAtCompileTimeAndRunTime("i32", {{"A", "i32", "42"}}, "-A");
  expectSameAtCompileTimeAndRunTime("i64", {{"A", "i64", "-42"}}, "-A");
  expectSameAtCompileTimeAndRunTime("u8", {{"A", "u8", "15"}}, "~A");
  expectSameAtCompileTimeAndRunTime("i32", {{"A", "i32", "0"}}, "~A");
  expectSameAtCompileTimeAndRunTime("bool", {{"A", "bool", "true"}}, "not A");
  expectSameAtCompileTimeAndRunTime("f64", {{"A", "f64", "1.5"}}, "-A");
}

TEST(Constants_Evaluator, float_arithmetic_matches_run_time) {
  for (const char* op : {"+", "-", "*", "/"}) {
    expectSameAtCompileTimeAndRunTime(
        "f64", {{"A", "f64", "0.1"}, {"B", "f64", "0.2"}},
        std::string("A ") + op + " B");
    expectSameAtCompileTimeAndRunTime(
        "f32", {{"A", "f32", "0.1"}, {"B", "f32", "3.0"}},
        std::string("A ") + op + " B");
    expectSameAtCompileTimeAndRunTime(
        "f64", {{"A", "f32", "0.1"}, {"B", "f64", "3.0"}},
        std::string("A ") + op + " B");
  }
  // Dividing by zero is defined for floats: it gives an infinity.
  expectSameAtCompileTimeAndRunTime(
      "f64", {{"A", "f64", "1.0"}, {"B", "f64", "0.0"}}, "A / B");
  expectSameAtCompileTimeAndRunTime(
      "f64", {{"A", "f64", "-1.0"}, {"B", "f64", "0.0"}}, "A / B");
}

TEST(Constants_Evaluator, float_comparisons_with_not_a_number_match_run_time) {
  for (const char* op : {"<", "<=", ">", ">=", "==", "!="}) {
    // Z / Z is not a number.
    expectSameAtCompileTimeAndRunTime(
        "bool", {{"Z", "f64", "0.0"}, {"B", "f64", "1.0"}},
        std::string("Z / Z ") + op + " B");
    expectSameAtCompileTimeAndRunTime(
        "bool", {{"A", "f64", "1.5"}, {"B", "f64", "2.5"}},
        std::string("A ") + op + " B");
  }
}

TEST(Constants_Evaluator, numeric_conversions_match_run_time) {
  expectSameAtCompileTimeAndRunTime("i8", {{"A", "i32", "300"}},
                                    "_convert<i8>(A)");
  expectSameAtCompileTimeAndRunTime("u8", {{"A", "i32", "-1"}},
                                    "_convert<u8>(A)");
  expectSameAtCompileTimeAndRunTime("i64", {{"A", "i8", "-5"}},
                                    "_convert<i64>(A)");
  expectSameAtCompileTimeAndRunTime("i64", {{"A", "u8", "251"}},
                                    "_convert<i64>(A)");
  expectSameAtCompileTimeAndRunTime("u64", {{"A", "i32", "-5"}},
                                    "_convert<u64>(A)");
  expectSameAtCompileTimeAndRunTime("f64", {{"A", "i64", "-9007199254740993"}},
                                    "_convert<f64>(A)");
  expectSameAtCompileTimeAndRunTime("f32", {{"A", "u32", "4294967295"}},
                                    "_convert<f32>(A)");
  expectSameAtCompileTimeAndRunTime("i32", {{"A", "f64", "-2.9"}},
                                    "_convert<i32>(A)");
  expectSameAtCompileTimeAndRunTime("u8", {{"A", "f64", "200.7"}},
                                    "_convert<u8>(A)");
  expectSameAtCompileTimeAndRunTime("f32", {{"A", "f64", "0.1"}},
                                    "_convert<f32>(A)");
  expectSameAtCompileTimeAndRunTime("f64", {{"A", "f32", "0.1"}},
                                    "_convert<f64>(A)");
  expectSameAtCompileTimeAndRunTime("i32", {{"A", "bool", "true"}},
                                    "_convert<i32>(A)");
}

// === What is left to the startup function, and why ===

TEST(Constants_Evaluator, undefined_integer_operations_are_not_evaluated) {
  expectInitializedAtStartup("const Z: i64 = 0;\nconst A: i64 = 1 / Z;", "A",
                             "divides an integer by zero");
  expectInitializedAtStartup("const Z: i64 = 0;\nconst A: i64 = 1 % Z;", "A",
                             "divides an integer by zero");
  expectInitializedAtStartup(
      "const M: i32 = -2147483648;\nconst N: i32 = -1;\nconst A: i32 = M / N;",
      "A", "divides the most negative integer by -1");
  expectInitializedAtStartup("const S: i32 = 32;\nconst A: i32 = 1 << S;", "A",
                             "shifts an integer by its width or more");
  expectInitializedAtStartup(
      "const F: f64 = 1.0e40;\nconst A: i32 = _convert<i32>(F);", "A",
      "converts a float that the integer type cannot hold");
}

TEST(Constants_Evaluator, reading_a_var_is_left_to_startup) {
  expectInitializedAtStartup("var counter: i64 = 4;\nconst S: i64 = counter;",
                             "S", "reads 'counter', which is a 'var'");
}

TEST(Constants_Evaluator, reading_a_startup_constant_is_left_to_startup) {
  expectInitializedAtStartup(
      "var counter: i64 = 4;\nconst S: i64 = counter;\nconst T: i64 = S + 1;",
      "T", "reads 'S', which is initialized at startup");
}

TEST(Constants_Evaluator, class_construction_is_left_to_startup) {
  expectInitializedAtStartup(R"(
    class Point {
      var x: i64;
      init(x: i64) { this.x = x; }
    }
    var origin: Point = Point(0);
  )",
                             "origin", "constructs a 'Point' value");
}
