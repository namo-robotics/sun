// tests/test_parser.cpp

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Include your public headers
#include "ast.h"     // Needed to inspect the parsed AST nodes
#include "lexer.h"   // If needed for Token checks
#include "parser.h"  // This should be in include/ (or include/SunCompiler/)

// Helper to parse a string and return the parsed AST (for top-level
// expressions)
std::unique_ptr<ExprAST> parseStringToExpr(const std::string& source) {
  std::istringstream ss(source);
  Parser parser(ss);

  // Advance to first token
  parser.getNextToken();

  // Parse a single expression (useful for most tests)
  return parser.parseExpression();
}

// Helper to parse a string and return the parsed AST (for top-level
// expressions)
std::unique_ptr<BlockExprAST> parseString(const std::string& source) {
  std::istringstream ss(source);
  Parser parser(ss);

  // Advance to first token
  return parser.parseString(source);
}

// ------------------------------------------------------------------
// Basic number literal tests
// ------------------------------------------------------------------
TEST(ParserTest, ParseIntegerLiteral) {
  auto ast = parseStringToExpr("42");

  ASSERT_NE(ast, nullptr);
  auto* num = dynamic_cast<NumberExprAST*>(ast.get());
  ASSERT_NE(num, nullptr);
  EXPECT_EQ(num->getVal(), 42);
}

TEST(ParserTest, ParseFloatingLiteral) {
  auto ast = parseStringToExpr("3.14159");

  ASSERT_NE(ast, nullptr);
  auto* num = dynamic_cast<NumberExprAST*>(ast.get());
  ASSERT_NE(num, nullptr);
  EXPECT_DOUBLE_EQ(num->getVal(), 3.14159);
}

// ------------------------------------------------------------------
// Variable reference
// ------------------------------------------------------------------
TEST(ParserTest, ParseVariable) {
  auto ast = parseStringToExpr("foo");

  ASSERT_NE(ast, nullptr);
  auto* var = dynamic_cast<VariableReferenceAST*>(ast.get());
  ASSERT_NE(var, nullptr);
  EXPECT_EQ(var->getName(), "foo");
}

// ------------------------------------------------------------------
// Parenthesized expression
// ------------------------------------------------------------------
TEST(ParserTest, ParseParenExpression) {
  auto ast = parseStringToExpr("(123)");

  ASSERT_NE(ast, nullptr);
  auto* num = dynamic_cast<NumberExprAST*>(ast.get());
  ASSERT_NE(num, nullptr);
  EXPECT_DOUBLE_EQ(num->getVal(), 123.0);
}

// ------------------------------------------------------------------
// Simple binary operations (with precedence)
// ------------------------------------------------------------------
TEST(ParserTest, ParseSimpleBinaryAdd) {
  auto ast = parseStringToExpr("a + b");

  ASSERT_NE(ast, nullptr);
  auto* bin = dynamic_cast<BinaryExprAST*>(ast.get());
  ASSERT_NE(bin, nullptr);
  EXPECT_EQ(bin->getOp().kind, TokenKind::PLUS);

  auto* left = dynamic_cast<const VariableReferenceAST*>(bin->getLHS());
  auto* right = dynamic_cast<const VariableReferenceAST*>(bin->getRHS());
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  EXPECT_EQ(left->getName(), "a");
  EXPECT_EQ(right->getName(), "b");
}

TEST(ParserTest, ParseBinaryWithPrecedence) {
  // Should parse as (a + b) * c
  auto ast = parseStringToExpr("a + b * c");

  ASSERT_NE(ast, nullptr);
  auto* outer = dynamic_cast<BinaryExprAST*>(ast.get());
  ASSERT_NE(outer, nullptr);
  EXPECT_EQ(outer->getOp().kind, TokenKind::PLUS);  // + has lower precedence

  auto* inner = dynamic_cast<const BinaryExprAST*>(outer->getRHS());
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->getOp().kind, TokenKind::STAR);
}

// ------------------------------------------------------------------
// Function prototype
// ------------------------------------------------------------------
std::unique_ptr<PrototypeAST> parsePrototype(const std::string& source) {
  std::istringstream ss(source);
  Parser parser(ss);
  parser.getNextToken();
  return parser.parsePrototype();
}

TEST(ParserTest, ParsePrototypeNoArgs) {
  auto proto = parsePrototype("sin();");

  ASSERT_NE(proto, nullptr);
  EXPECT_EQ(proto->getName(), "sin");
  EXPECT_TRUE(proto->getArgNames().empty());
}

TEST(ParserTest, ParsePrototypeWithArgs) {
  auto proto = parsePrototype("a(b: i32, c: i32);");

  ASSERT_NE(proto, nullptr);
  EXPECT_EQ(proto->getName(), "a");
  ASSERT_EQ(proto->getArgNames().size(), 2);
  EXPECT_EQ(proto->getArgNames()[0], "b");
  EXPECT_EQ(proto->getArgNames()[1], "c");
}

TEST(ParserTest, LexerRegex) {
  std::istringstream ss("a+b");
  Lexer lexer(ss);
  ASSERT_TRUE(lexer.getTokenNFA().matches(" a"));
}

// ------------------------------------------------------------------
// Full top-level function definition using new var syntax
// ------------------------------------------------------------------

TEST(ParserTest, ParseLambda) {
  std::string src = "var f = lambda (x: i32) i32 { x+1; };";
  auto block = parseString(src);
  ASSERT_NE(block, nullptr);
  // The function is wrapped in an anonymous top-level expr
  const auto& body = block->getBody();
  ASSERT_FALSE(body.empty());
  // The body should contain a VariableCreationAST
  const auto* varCreation =
      dynamic_cast<const VariableCreationAST*>(block->getBody()[0].get());
  ASSERT_NE(varCreation, nullptr);
  EXPECT_EQ(varCreation->getName(), "f");
  // The value should be a LambdaAST
  const auto* innerLambda =
      dynamic_cast<const LambdaAST*>(varCreation->getValue());
  ASSERT_NE(innerLambda, nullptr);
  EXPECT_EQ(innerLambda->getProto().getName(), "");
  ASSERT_EQ(innerLambda->getProto().getArgNames().size(), 1);
  EXPECT_EQ(innerLambda->getProto().getArgNames()[0], "x");
}

TEST(ParserTest, ParseFunction) {
  std::string src = "function foo(x: i32) i32 { x+1; };";
  auto block = parseString(src);
  ASSERT_NE(block, nullptr);
  // The function is wrapped in an anonymous top-level expr
  const auto& body = block->getBody();
  ASSERT_FALSE(body.empty());
  // The body should contain a VariableCreationAST
  const auto* func =
      dynamic_cast<const FunctionAST*>(block->getBody()[0].get());
  ASSERT_NE(func, nullptr);
  EXPECT_EQ(func->getProto().getName(), "foo");
  ASSERT_EQ(func->getProto().getArgNames().size(), 1);
  EXPECT_EQ(func->getProto().getArgNames()[0], "x");
}

TEST(ParserTest, IfExpression) {
  auto ast = parseStringToExpr("if (x < 10) { 1; } else { 0; }");

  ASSERT_NE(ast, nullptr);
  auto* ifExpr = dynamic_cast<IfExprAST*>(ast.get());
  ASSERT_NE(ifExpr, nullptr);

  // Check condition
  auto* cond = dynamic_cast<BinaryExprAST*>(ifExpr->getCond());
  ASSERT_NE(cond, nullptr);
  EXPECT_EQ(cond->getOp().kind, TokenKind::LESS);

  // Check then branch
  auto* thenBranch = dynamic_cast<NumberExprAST*>(ifExpr->getThen());
  ASSERT_NE(thenBranch, nullptr);
  EXPECT_DOUBLE_EQ(thenBranch->getVal(), 1.0);

  // Check else branch
  auto* elseBranch = dynamic_cast<NumberExprAST*>(ifExpr->getElse());
  ASSERT_NE(elseBranch, nullptr);
  EXPECT_DOUBLE_EQ(elseBranch->getVal(), 0.0);
}

TEST(ParserTest, Fib) {
  auto ast = parseString(
      "var fib = lambda (x: i32) i32 { if (x < 3) { 1; } else { "
      "fib(x-1)+fib(x-2); } };");
  ASSERT_NE(ast, nullptr);
}

TEST(ParserTest, ParseNestedFunction) {
  auto ast = parseString(
      "var f = lambda (x: i32) i32 { var g = lambda (y: i32) i32 { y; }; g(x); "
      "};");
  ASSERT_NE(ast, nullptr);
}

TEST(ParserTest, ParseCloser) {
  auto ast = parseString(
      "var outer = 42; var closure = lambda () { outer; }; closure();");
  ASSERT_NE(ast, nullptr);
}

// ------------------------------------------------------------------
// Named functions vs Lambdas
// ------------------------------------------------------------------

// Lambda: var f = () i32 { ... }
// The function should have an EMPTY name (it's a lambda assigned to variable f)
TEST(ParserTest, ParseLambdaAssignedToVariable) {
  std::string src = "var f = lambda () i32 { 42; };";
  auto block = parseString(src);
  ASSERT_NE(block, nullptr);

  const auto& body = block->getBody();
  ASSERT_FALSE(body.empty());

  const auto* varCreation =
      dynamic_cast<const VariableCreationAST*>(body[0].get());
  ASSERT_NE(varCreation, nullptr);
  EXPECT_EQ(varCreation->getName(), "f");

  const auto* innerLambda =
      dynamic_cast<const LambdaAST*>(varCreation->getValue());
  ASSERT_NE(innerLambda, nullptr);

  // KEY TEST: Lambda should have EMPTY name
  EXPECT_TRUE(innerLambda->getProto().getName().empty())
      << "Lambda assigned to variable should have empty name, got: '"
      << innerLambda->getProto().getName() << "'";
}

// Named function: var f = f() i32 { ... }
// The function should have name "f"
TEST(ParserTest, ParseNamedFunctionAssignedToVariable) {
  std::string src = "function f() i32 { 42; };";
  auto block = parseString(src);
  ASSERT_NE(block, nullptr);

  const auto& body = block->getBody();
  ASSERT_FALSE(body.empty());

  // Direct function definition (not assigned to variable)
  const auto* func = dynamic_cast<const FunctionAST*>(body[0].get());
  ASSERT_NE(func, nullptr);

  // KEY TEST: Named function should have name "f"
  EXPECT_EQ(func->getProto().getName(), "f")
      << "Named function should have name 'f'";
}

// Lambda with parameters: var add = (x: i32, y: i32) i32 { x + y; };
TEST(ParserTest, ParseLambdaWithParameters) {
  std::string src = "var add = lambda (x: i32, y: i32) i32 { x + y; };";
  auto block = parseString(src);
  ASSERT_NE(block, nullptr);

  const auto& body = block->getBody();
  ASSERT_FALSE(body.empty());

  const auto* varCreation =
      dynamic_cast<const VariableCreationAST*>(body[0].get());
  ASSERT_NE(varCreation, nullptr);

  const auto* innerLambda =
      dynamic_cast<const LambdaAST*>(varCreation->getValue());
  ASSERT_NE(innerLambda, nullptr);

  // Lambda should have empty name
  EXPECT_TRUE(innerLambda->getProto().getName().empty())
      << "Lambda should have empty name, got: '"
      << innerLambda->getProto().getName() << "'";

  // Should have 2 parameters
  ASSERT_EQ(innerLambda->getProto().getArgs().size(), 2);
  EXPECT_EQ(innerLambda->getProto().getArgs()[0].first, "x");
  EXPECT_EQ(innerLambda->getProto().getArgs()[1].first, "y");
}

// ------------------------------------------------------------------
// Large code block parsing performance test
// ------------------------------------------------------------------

TEST(ParserTest, ParseLargeCodeBlock) {
  // This test parses a substantial block of code with multiple functions
  // to help identify parsing performance bottlenecks.
  std::string src = R"(
    function factorial(n: i32) i32 {
      if (n <= 1) {
        return 1;
      }
      return n * factorial(n - 1);
    }

    function fibonacci(n: i32) i32 {
      if (n < 2) {
        return n;
      }
      return fibonacci(n - 1) + fibonacci(n - 2);
    }

    function gcd(a: i32, b: i32) i32 {
      if (b == 0) {
        return a;
      }
      return gcd(b, a - (a / b) * b);
    }

    function lcm(a: i32, b: i32) i32 {
      return (a * b) / gcd(a, b);
    }

    function isPrime(n: i32) i32 {
      if (n < 2) { return 0; }
      if (n == 2) { return 1; }
      var rem: i32 = n - (n / 2) * 2;
      if (rem == 0) { return 0; }
      var i: i32 = 3;
      while (i * i <= n) {
        var r: i32 = n - (n / i) * i;
        if (r == 0) { return 0; }
        i = i + 2;
      }
      return 1;
    }

    function sumToN(n: i32) i32 {
      var sum: i32 = 0;
      var i: i32 = 1;
      while (i <= n) {
        sum = sum + i;
        i = i + 1;
      }
      return sum;
    }

    function power(base: i32, exp: i32) i32 {
      if (exp == 0) { return 1; }
      if (exp == 1) { return base; }
      var half: i32 = power(base, exp / 2);
      var rem: i32 = exp - (exp / 2) * 2;
      if (rem == 0) {
        return half * half;
      }
      return base * half * half;
    }

    function abs(x: i32) i32 {
      if (x < 0) { return 0 - x; }
      return x;
    }

    function max(a: i32, b: i32) i32 {
      if (a > b) { return a; }
      return b;
    }

    function min(a: i32, b: i32) i32 {
      if (a < b) { return a; }
      return b;
    }

    function clamp(x: i32, lo: i32, hi: i32) i32 {
      return max(lo, min(x, hi));
    }

    function sign(x: i32) i32 {
      if (x > 0) { return 1; }
      if (x < 0) { return 0 - 1; }
      return 0;
    }

    function main() i32 {
      var f: i32 = factorial(10);
      var fib: i32 = fibonacci(20);
      var g: i32 = gcd(48, 18);
      var l: i32 = lcm(12, 18);
      var prime: i32 = isPrime(17);
      var sum: i32 = sumToN(100);
      var p: i32 = power(2, 10);
      var a: i32 = abs(0 - 42);
      var mx: i32 = max(10, 20);
      var mn: i32 = min(10, 20);
      var c: i32 = clamp(150, 0, 100);
      var s: i32 = sign(0 - 5);
      return f + fib + g + l + prime + sum + p + a + mx + mn + c + s;
    }
  )";

  auto start = std::chrono::high_resolution_clock::now();
  auto block = parseString(src);
  auto end = std::chrono::high_resolution_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  ASSERT_NE(block, nullptr);

  // Verify we parsed all 13 functions
  const auto& body = block->getBody();
  ASSERT_EQ(body.size(), 13) << "Expected 13 functions to be parsed";

  // Log parsing time for profiling
  std::cout << "[PARSER PERF] Large code block (" << src.size()
            << " chars, 13 functions) "
            << "parsed in " << duration.count() << "ms" << std::endl;

  // Verify function names
  std::vector<std::string> expectedNames = {
      "factorial", "fibonacci", "gcd", "lcm",   "isPrime", "sumToN", "power",
      "abs",       "max",       "min", "clamp", "sign",    "main"};

  for (size_t i = 0; i < expectedNames.size(); ++i) {
    const auto* func = dynamic_cast<const FunctionAST*>(body[i].get());
    ASSERT_NE(func, nullptr) << "Function " << i << " should be a FunctionAST";
    EXPECT_EQ(func->getProto().getName(), expectedNames[i])
        << "Function " << i << " should be named '" << expectedNames[i] << "'";
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
