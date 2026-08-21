// tests/tooling/frontend/test_parser.cpp

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Include your public headers
#include "ast.h"     // Needed to inspect the parsed AST nodes
#include "error.h"   // For SunError exception type
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
TEST(Tooling_Frontend_Parser, ParseIntegerLiteral) {
  auto ast = parseStringToExpr("42");

  ASSERT_NE(ast, nullptr);
  auto* num = dynamic_cast<NumberExprAST*>(ast.get());
  ASSERT_NE(num, nullptr);
  EXPECT_EQ(num->getVal(), 42);
}

TEST(Tooling_Frontend_Parser, ParseFloatingLiteral) {
  auto ast = parseStringToExpr("3.14159");

  ASSERT_NE(ast, nullptr);
  auto* num = dynamic_cast<NumberExprAST*>(ast.get());
  ASSERT_NE(num, nullptr);
  EXPECT_DOUBLE_EQ(num->getVal(), 3.14159);
}

// ------------------------------------------------------------------
// Variable reference
// ------------------------------------------------------------------
TEST(Tooling_Frontend_Parser, ParseVariable) {
  auto ast = parseStringToExpr("foo");

  ASSERT_NE(ast, nullptr);
  auto* var = dynamic_cast<VariableReferenceAST*>(ast.get());
  ASSERT_NE(var, nullptr);
  EXPECT_EQ(var->getName(), "foo");
}

// ------------------------------------------------------------------
// Parenthesized expression
// ------------------------------------------------------------------
TEST(Tooling_Frontend_Parser, ParseParenExpression) {
  auto ast = parseStringToExpr("(123)");

  ASSERT_NE(ast, nullptr);
  auto* paren = dynamic_cast<ParenExprAST*>(ast.get());
  ASSERT_NE(paren, nullptr);
  auto* num = dynamic_cast<const NumberExprAST*>(paren->getInner());
  ASSERT_NE(num, nullptr);
  EXPECT_DOUBLE_EQ(num->getVal(), 123.0);
}

TEST(Tooling_Frontend_Parser, ParseNestedParenExpression) {
  auto ast = parseStringToExpr("((1))");

  ASSERT_NE(ast, nullptr);
  auto* outer = dynamic_cast<ParenExprAST*>(ast.get());
  ASSERT_NE(outer, nullptr);
  auto* inner = dynamic_cast<const ParenExprAST*>(outer->getInner());
  ASSERT_NE(inner, nullptr);
  auto* num = dynamic_cast<const NumberExprAST*>(inner->getInner());
  ASSERT_NE(num, nullptr);
  EXPECT_DOUBLE_EQ(num->getVal(), 1.0);
}

TEST(Tooling_Frontend_Parser, ParseTemplateString) {
  auto ast = parseStringToExpr("`Hello ${name}!`");

  ASSERT_NE(ast, nullptr);
  auto* interp = dynamic_cast<InterpolatedStringAST*>(ast.get());
  ASSERT_NE(interp, nullptr);
  EXPECT_EQ(interp->getRawContent(), "Hello ${name}!");
  const auto& segments = interp->getSegments();
  ASSERT_EQ(segments.size(), 3u);
  EXPECT_TRUE(segments[0].isLiteral);
  EXPECT_EQ(segments[0].rawText, "Hello ");
  EXPECT_EQ(segments[0].cookedText, "Hello ");
  EXPECT_FALSE(segments[1].isLiteral);
  EXPECT_EQ(segments[1].rawText, "name");
  ASSERT_NE(segments[1].expression, nullptr);
  EXPECT_EQ(segments[1].expression->getType(),
            ASTNodeType::VARIABLE_REFERENCE);
  EXPECT_TRUE(segments[2].isLiteral);
  EXPECT_EQ(segments[2].rawText, "!");
}

TEST(Tooling_Frontend_Parser, TemplateStringRawVsCookedEscapes) {
  auto ast = parseStringToExpr("`a\\nb`");

  ASSERT_NE(ast, nullptr);
  auto* interp = dynamic_cast<InterpolatedStringAST*>(ast.get());
  ASSERT_NE(interp, nullptr);
  const auto& segments = interp->getSegments();
  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0].rawText, "a\\nb");   // escapes unprocessed
  EXPECT_EQ(segments[0].cookedText, "a\nb");  // escapes processed
}

// ------------------------------------------------------------------
// Simple binary operations (with precedence)
// ------------------------------------------------------------------
TEST(Tooling_Frontend_Parser, ParseSimpleBinaryAdd) {
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

TEST(Tooling_Frontend_Parser, ParseBinaryWithPrecedence) {
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

TEST(Tooling_Frontend_Parser, ParsePrototypeNoArgs) {
  auto proto = parsePrototype("sin();");

  ASSERT_NE(proto, nullptr);
  EXPECT_EQ(proto->getName(), "sin");
  EXPECT_TRUE(proto->getArgNames().empty());
}

TEST(Tooling_Frontend_Parser, ParsePrototypeWithArgs) {
  auto proto = parsePrototype("a(b: i32, c: i32);");

  ASSERT_NE(proto, nullptr);
  EXPECT_EQ(proto->getName(), "a");
  ASSERT_EQ(proto->getArgNames().size(), 2);
  EXPECT_EQ(proto->getArgNames()[0], "b");
  EXPECT_EQ(proto->getArgNames()[1], "c");
}

TEST(Tooling_Frontend_Parser, LexerRegex) {
  ASSERT_TRUE(Lexer::getTokenDFA().matches(" a"));
}

// ------------------------------------------------------------------
// Full top-level function definition using new var syntax
// ------------------------------------------------------------------

TEST(Tooling_Frontend_Parser, ParseLambda) {
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

TEST(Tooling_Frontend_Parser, ParseFunction) {
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

TEST(Tooling_Frontend_Parser, IfExpression) {
  auto ast = parseStringToExpr("if (x < 10) { 1; } else { 0; }");

  ASSERT_NE(ast, nullptr);
  auto* ifExpr = dynamic_cast<IfExprAST*>(ast.get());
  ASSERT_NE(ifExpr, nullptr);

  // Check condition (the if-condition parens are preserved in the parse tree)
  auto* condParen = dynamic_cast<ParenExprAST*>(ifExpr->getCond());
  ASSERT_NE(condParen, nullptr);
  auto* cond = dynamic_cast<const BinaryExprAST*>(condParen->getInner());
  ASSERT_NE(cond, nullptr);
  EXPECT_EQ(cond->getOp().kind, TokenKind::LESS);

  // Branches stay blocks in the lossless parse tree (LoweringPass unwraps)
  auto* thenBlock = dynamic_cast<BlockExprAST*>(ifExpr->getThen());
  ASSERT_NE(thenBlock, nullptr);
  ASSERT_EQ(thenBlock->getBody().size(), 1u);
  auto* thenBranch =
      dynamic_cast<NumberExprAST*>(thenBlock->getBody()[0].get());
  ASSERT_NE(thenBranch, nullptr);
  EXPECT_DOUBLE_EQ(thenBranch->getVal(), 1.0);

  auto* elseBlock = dynamic_cast<BlockExprAST*>(ifExpr->getElse());
  ASSERT_NE(elseBlock, nullptr);
  ASSERT_EQ(elseBlock->getBody().size(), 1u);
  auto* elseBranch =
      dynamic_cast<NumberExprAST*>(elseBlock->getBody()[0].get());
  ASSERT_NE(elseBranch, nullptr);
  EXPECT_DOUBLE_EQ(elseBranch->getVal(), 0.0);
}

TEST(Tooling_Frontend_Parser, Fib) {
  auto ast = parseString(
      "var fib = lambda (x: i32) i32 { if (x < 3) { 1; } else { "
      "fib(x-1)+fib(x-2); } };");
  ASSERT_NE(ast, nullptr);
}

TEST(Tooling_Frontend_Parser, ParseNestedFunction) {
  auto ast = parseString(
      "var f = lambda (x: i32) i32 { var g = lambda (y: i32) i32 { y; }; g(x); "
      "};");
  ASSERT_NE(ast, nullptr);
}

TEST(Tooling_Frontend_Parser, ParseCloser) {
  auto ast = parseString(
      "var outer = 42; var closure = lambda () i32 { outer; }; closure();");
  ASSERT_NE(ast, nullptr);
}

// ------------------------------------------------------------------
// Named functions vs Lambdas
// ------------------------------------------------------------------

// Lambda: var f = () i32 { ... }
// The function should have an EMPTY name (it's a lambda assigned to variable f)
TEST(Tooling_Frontend_Parser, ParseLambdaAssignedToVariable) {
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
TEST(Tooling_Frontend_Parser, ParseNamedFunctionAssignedToVariable) {
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
TEST(Tooling_Frontend_Parser, ParseLambdaWithParameters) {
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

TEST(Tooling_Frontend_Parser, ParseLargeCodeBlock) {
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

// ------------------------------------------------------------------
// Parsing error tests - verify enhanced error messages
// ------------------------------------------------------------------

TEST(Tooling_Frontend_Parser_Errors, MissingSemicolonAfterVarDecl) {
  // Missing semicolon after variable declaration
  EXPECT_THROW(
      {
        try {
          parseString(R"(
function main() i32 {
    var x: i32 = 42
    return x;
}
)");
        } catch (const SunError& e) {
          // Print error to stdout for test explorer visibility
          std::cout << "\n--- Error Message ---\n"
                    << e.what() << "\n---------------------\n";
          // Verify it's a parse error with source context
          EXPECT_EQ(e.getKind(), SunError::Kind::Parse);
          EXPECT_TRUE(e.getLocation().has_value());
          std::string what = e.what();
          // Should contain source line preview
          EXPECT_TRUE(what.find("|") != std::string::npos)
              << "Error should contain source preview with '|': " << what;
          EXPECT_TRUE(what.find("^") != std::string::npos)
              << "Error should contain caret '^': " << what;
          throw;  // Re-throw to satisfy EXPECT_THROW
        }
      },
      SunError);
}

TEST(Tooling_Frontend_Parser_Errors, MissingClosingBrace) {
  // Missing closing brace in function body
  EXPECT_THROW(
      {
        try {
          parseString(R"(
function test() i32 {
    return 1;

function main() i32 {
    return 0;
}
)");
        } catch (const SunError& e) {
          // Print error to stdout for test explorer visibility
          std::cout << "\n--- Error Message ---\n"
                    << e.what() << "\n---------------------\n";
          EXPECT_EQ(e.getKind(), SunError::Kind::Parse);
          std::string what = e.what();
          // Should mention expected '}'
          EXPECT_TRUE(what.find("}") != std::string::npos ||
                      what.find("brace") != std::string::npos)
              << "Error should mention missing brace: " << what;
          throw;
        }
      },
      SunError);
}

TEST(Tooling_Frontend_Parser_Errors, MissingTypeAnnotation) {
  // Missing type annotation with colon but no type
  EXPECT_THROW(
      {
        try {
          parseString(R"(
function main() i32 {
    var x: = 42;
    return x;
}
)");
        } catch (const SunError& e) {
          // Print error to stdout for test explorer visibility
          std::cout << "\n--- Error Message ---\n"
                    << e.what() << "\n---------------------\n";
          EXPECT_EQ(e.getKind(), SunError::Kind::Parse);
          std::string what = e.what();
          // Should have source preview with line number and caret
          EXPECT_TRUE(what.find("|") != std::string::npos);
          EXPECT_TRUE(what.find("^") != std::string::npos);
          throw;
        }
      },
      SunError);
}

TEST(Tooling_Frontend_Parser_Errors, InvalidExpressionInParens) {
  // Empty parentheses
  EXPECT_THROW(
      {
        try {
          parseString(R"(
function main() i32 {
    var x: i32 = ();
    return x;
}
)");
        } catch (const SunError& e) {
          // Print error to stdout for test explorer visibility
          std::cout << "\n--- Error Message ---\n"
                    << e.what() << "\n---------------------\n";
          EXPECT_EQ(e.getKind(), SunError::Kind::Parse);
          throw;
        }
      },
      SunError);
}

// `&&` / `||` are not Sun operators; the error must name `and` / `or`.
static std::string parseErrorMessage(const std::string& src) {
  try {
    parseString(src);
  } catch (const SunError& e) {
    EXPECT_EQ(e.getKind(), SunError::Kind::Parse);
    return e.what();
  }
  ADD_FAILURE() << "expected a parse error";
  return "";
}

TEST(Tooling_Frontend_Parser_Errors, DoubleAmpersandSuggestsAnd) {
  std::string what = parseErrorMessage(R"(
function main() i32 {
    if (true && false) { return 1; }
    return 0;
}
)");
  EXPECT_TRUE(what.find("unexpected '&&'") != std::string::npos) << what;
  EXPECT_TRUE(what.find("'and'") != std::string::npos) << what;
  EXPECT_TRUE(what.find("3:14") != std::string::npos) << what;  // points at '&&'
}

TEST(Tooling_Frontend_Parser_Errors, DoublePipeSuggestsOr) {
  std::string what = parseErrorMessage(R"(
function main() i32 {
    var ok: bool = true || false;
    return 0;
}
)");
  EXPECT_TRUE(what.find("unexpected '||'") != std::string::npos) << what;
  EXPECT_TRUE(what.find("'or'") != std::string::npos) << what;
}

// A keyword where a name belongs is a name collision, not a syntax error;
// the diagnostic must say which word is reserved (issue #92).
TEST(Tooling_Frontend_Parser_Errors, KeywordAsVariableNameIsReserved) {
  std::string what = parseErrorMessage(R"(
function main() i32 {
    var partial = 1;
    return 0;
}
)");
  EXPECT_TRUE(what.find("'partial' is a reserved word") != std::string::npos)
      << what;
  EXPECT_TRUE(what.find("3:9") != std::string::npos) << what;  // at 'partial'
  EXPECT_TRUE(what.find("expected identifier") == std::string::npos) << what;
}

TEST(Tooling_Frontend_Parser_Errors, KeywordAsFunctionNameIsReserved) {
  std::string what = parseErrorMessage(R"(
function class() i32 { return 0; }
)");
  EXPECT_TRUE(what.find("'class' is a reserved word") != std::string::npos)
      << what;
}

TEST(Tooling_Frontend_Parser_Errors, KeywordAsParameterNameIsReserved) {
  std::string what = parseErrorMessage(R"(
function f(spawn: i32) i32 { return spawn; }
)");
  EXPECT_TRUE(what.find("'spawn' is a reserved word") != std::string::npos)
      << what;
}

TEST(Tooling_Frontend_Parser_Errors, KeywordAsClassNameIsReserved) {
  std::string what = parseErrorMessage(R"(
class array { var x: i32; function init() { this.x = 0; } }
)");
  EXPECT_TRUE(what.find("'array' is a reserved word") != std::string::npos)
      << what;
}

TEST(Tooling_Frontend_Parser_Errors, KeywordAsFieldNameIsReserved) {
  std::string what = parseErrorMessage(R"(
class P { var match: i32; function init() { this.match = 0; } }
)");
  EXPECT_TRUE(what.find("'match' is a reserved word") != std::string::npos)
      << what;
}

TEST(Tooling_Frontend_Parser_Errors, KeywordAsMemberNameIsReserved) {
  std::string what = parseErrorMessage(R"(
function main() i32 {
    var p = P();
    return p.unsafe;
}
)");
  EXPECT_TRUE(what.find("'unsafe' is a reserved word") != std::string::npos)
      << what;
}

TEST(Tooling_Frontend_Parser_Errors, NonKeywordKeepsExpectedIdentifierMessage) {
  std::string what = parseErrorMessage(R"(
function main() i32 {
    var 5 = 1;
    return 0;
}
)");
  EXPECT_TRUE(what.find("expected identifier after 'var'") !=
              std::string::npos)
      << what;
  EXPECT_TRUE(what.find("reserved word") == std::string::npos) << what;
}

// `in`, `as` and `const` only mean something in one position each; they
// stay usable as names.
TEST(Tooling_Frontend_Parser, ContextualWordsAreValidNames) {
  auto ast = parseString(R"(
function main() i32 {
    var in: i32 = 5;
    var as: i32 = 6;
    var const: i32 = 7;
    return in + as + const;
}
)");
  ASSERT_NE(ast, nullptr);
}

// ------------------------------------------------------------------
// Driver-based parsing error tests - these log errors to stderr
// Uses compileString which goes through the full driver pipeline
// ------------------------------------------------------------------

#include "execution_utils.h"

TEST(Tooling_Frontend_Parser_ErrorDriver, MissingSemicolonLogsEnhancedError) {
  // This test verifies that parsing errors through the driver
  // include enhanced error messages with source context
  testing::internal::CaptureStderr();

  bool threw = false;
  try {
    compileString(R"(
function main() i32 {
    var x: i32 = 42
    return x;
}
)");
  } catch (const SunError& e) {
    threw = true;
    std::cerr << e.what() << std::endl;
  }

  std::string output = testing::internal::GetCapturedStderr();

  // Print the error to stdout so it's visible in test explorer
  std::cout << "\n--- Captured Error Message ---\n"
            << output << "------------------------------\n";

  EXPECT_TRUE(threw) << "Should have thrown SunError";

  // Verify the error output contains source context markers
  EXPECT_TRUE(output.find("|") != std::string::npos)
      << "Error output should contain '|' for source preview: " << output;
  EXPECT_TRUE(output.find("^") != std::string::npos)
      << "Error output should contain '^' caret: " << output;
  EXPECT_TRUE(output.find("Parse Error") != std::string::npos)
      << "Error output should identify as Parse Error: " << output;
}

TEST(Tooling_Frontend_Parser_ErrorDriver, MissingColonInArgLogsError) {
  testing::internal::CaptureStderr();

  bool threw = false;
  try {
    compileString(R"(
function add(a i32, b: i32) i32 {
    return a + b;
}
function main() i32 { return 0; }
)");
  } catch (const SunError& e) {
    threw = true;
    std::cerr << e.what() << std::endl;
  }

  std::string output = testing::internal::GetCapturedStderr();

  // Print the error to stdout so it's visible in test explorer
  std::cout << "\n--- Captured Error Message ---\n"
            << output << "------------------------------\n";

  EXPECT_TRUE(threw) << "Should have thrown SunError";
  // Should show the problematic line with function signature
  EXPECT_TRUE(output.find("|") != std::string::npos)
      << "Should have source preview: " << output;
  EXPECT_TRUE(output.find("^") != std::string::npos)
      << "Should have caret: " << output;
}

TEST(Tooling_Frontend_Parser_ErrorDriver, UnexpectedTokenLogsError) {
  testing::internal::CaptureStderr();

  bool threw = false;
  try {
    compileString(R"(
function main() i32 {
    var x: i32 = 10 +;
    return x;
}
)");
  } catch (const SunError& e) {
    threw = true;
    std::cerr << e.what() << std::endl;
  }

  std::string output = testing::internal::GetCapturedStderr();

  // Print the error to stdout so it's visible in test explorer
  std::cout << "\n--- Captured Error Message ---\n"
            << output << "------------------------------\n";

  EXPECT_TRUE(threw) << "Should have thrown SunError";
  // Should contain source preview
  EXPECT_TRUE(output.find("|") != std::string::npos)
      << "Should have source preview: " << output;
}

// ------------------------------------------------------------------
// Return type requirement tests
// ------------------------------------------------------------------

TEST(Tooling_Frontend_Parser_Errors, MissingReturnTypeFunction) {
  // Function without return type should error
  EXPECT_THROW(
      {
        try {
          parseString(R"(
function test() {
    return 1;
}
)");
        } catch (const SunError& e) {
          std::cout << "\n--- Error Message ---\n"
                    << e.what() << "\n---------------------\n";
          EXPECT_EQ(e.getKind(), SunError::Kind::Parse);
          std::string what = e.what();
          EXPECT_TRUE(what.find("Return type is required") != std::string::npos)
              << "Error should mention return type requirement: " << what;
          throw;
        }
      },
      SunError);
}

TEST(Tooling_Frontend_Parser_Errors, MissingReturnTypeLambda) {
  // Lambda without return type should error
  EXPECT_THROW(
      {
        try {
          parseString(R"(
function main() i32 {
    var f = lambda (x: i32) { return x + 1; };
    return f(1);
}
)");
        } catch (const SunError& e) {
          std::cout << "\n--- Error Message ---\n"
                    << e.what() << "\n---------------------\n";
          EXPECT_EQ(e.getKind(), SunError::Kind::Parse);
          std::string what = e.what();
          EXPECT_TRUE(what.find("Return type is required") != std::string::npos)
              << "Error should mention return type requirement: " << what;
          throw;
        }
      },
      SunError);
}

TEST(Tooling_Frontend_Parser, InitMethodWithoutReturnType) {
  // init methods should be allowed without explicit return type
  auto ast = parseString(R"(
class Point {
    var x: i32;
    var y: i32;
    function init(px: i32, py: i32) {
        this.x = px;
        this.y = py;
    }
}
function main() i32 {
    var p = Point(1, 2);
    return p.x;
}
)");
  ASSERT_NE(ast, nullptr);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ------------------------------------------------------------------
// Unary expression tests
// ------------------------------------------------------------------
TEST(Tooling_Frontend_Parser, UnaryMinusIsUnaryNode) {
  auto ast = parseStringToExpr("-x");

  ASSERT_NE(ast, nullptr);
  auto* unary = dynamic_cast<UnaryExprAST*>(ast.get());
  ASSERT_NE(unary, nullptr);
  EXPECT_EQ(unary->getOp().kind, TokenKind::MINUS);
  EXPECT_EQ(unary->getOperand()->getType(), ASTNodeType::VARIABLE_REFERENCE);
}

TEST(Tooling_Frontend_Parser, NotIsUnaryNode) {
  auto ast = parseStringToExpr("not flag");

  ASSERT_NE(ast, nullptr);
  auto* unary = dynamic_cast<UnaryExprAST*>(ast.get());
  ASSERT_NE(unary, nullptr);
  EXPECT_EQ(unary->getOp().kind, TokenKind::NOT);
}

TEST(Tooling_Frontend_Parser, TildeIsUnaryNode) {
  auto ast = parseStringToExpr("~bits");

  ASSERT_NE(ast, nullptr);
  auto* unary = dynamic_cast<UnaryExprAST*>(ast.get());
  ASSERT_NE(unary, nullptr);
  EXPECT_EQ(unary->getOp().kind, TokenKind::TILDE);
}

TEST(Tooling_Frontend_Parser, UnaryMinusBindsTighterThanBinary) {
  auto ast = parseStringToExpr("-a + b");

  ASSERT_NE(ast, nullptr);
  auto* bin = dynamic_cast<BinaryExprAST*>(ast.get());
  ASSERT_NE(bin, nullptr);
  EXPECT_EQ(bin->getOp().kind, TokenKind::PLUS);
  EXPECT_EQ(bin->getLHS()->getType(), ASTNodeType::UNARY);
}

// ------------------------------------------------------------------
// Comment collection tests (lossless AST / formatter support)
// ------------------------------------------------------------------

// Parse a full program and return the parser for side-table inspection
static std::unique_ptr<Parser> parseProgramCollectingComments(
    const std::string& source) {
  std::istringstream dummy("");
  auto parser = std::make_unique<Parser>(dummy);
  parser->setCollectComments(true);
  auto ast = parser->parseString(source);
  EXPECT_NE(ast, nullptr);
  return parser;
}

TEST(Tooling_Frontend_Comments, BlockCommentsAreSkippedByDefault) {
  std::istringstream dummy("");
  Parser parser(dummy);
  auto ast = parser.parseString(
      "/* leading */ function main() i32 { return /* mid */ 42; } /* end */");
  ASSERT_NE(ast, nullptr);
  EXPECT_TRUE(parser.getComments().empty());
}

TEST(Tooling_Frontend_Comments, CollectsLineAndBlockComments) {
  auto parser = parseProgramCollectingComments(
      "// leading line\n"
      "function main() i32 {\n"
      "    var x: i32 = /* inline */ 40; // trailing\n"
      "    /* own line */\n"
      "    return x + 2;\n"
      "}\n");
  const auto& comments = parser->getComments();
  ASSERT_EQ(comments.size(), 4u);

  std::vector<Comment> ordered;
  for (const auto& [offset, c] : comments) ordered.push_back(c);

  EXPECT_EQ(ordered[0].text, "// leading line");
  EXPECT_FALSE(ordered[0].isBlock);
  EXPECT_TRUE(ordered[0].ownLine);

  EXPECT_EQ(ordered[1].text, "/* inline */");
  EXPECT_TRUE(ordered[1].isBlock);
  EXPECT_FALSE(ordered[1].ownLine);
  EXPECT_EQ(ordered[1].span.line, 3);

  EXPECT_EQ(ordered[2].text, "// trailing");
  EXPECT_FALSE(ordered[2].isBlock);
  EXPECT_FALSE(ordered[2].ownLine);

  EXPECT_EQ(ordered[3].text, "/* own line */");
  EXPECT_TRUE(ordered[3].isBlock);
  EXPECT_TRUE(ordered[3].ownLine);
}

TEST(Tooling_Frontend_Comments, MultilineBlockCommentSpan) {
  auto parser = parseProgramCollectingComments(
      "/* line one\n"
      "   line two */\n"
      "function main() i32 { return 1; }\n");
  const auto& comments = parser->getComments();
  ASSERT_EQ(comments.size(), 1u);
  const Comment& c = comments.begin()->second;
  EXPECT_EQ(c.span.line, 1);
  EXPECT_EQ(c.span.endLine, 2);
  EXPECT_TRUE(c.isBlock);
  EXPECT_TRUE(c.ownLine);
}

TEST(Tooling_Frontend_Comments, BlockCommentsDoNotNest) {
  // The outer comment closes at the first */; "rest" must be real tokens
  std::istringstream dummy("");
  Parser parser(dummy);
  parser.setCollectComments(true);
  auto ast = parser.parseString(
      "function main() i32 { return /* a /* b */ 5; }");
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(parser.getComments().size(), 1u);
  EXPECT_EQ(parser.getComments().begin()->second.text, "/* a /* b */");
}

TEST(Tooling_Frontend_Comments, StarsInsideBlockComment) {
  std::istringstream dummy("");
  Parser parser(dummy);
  parser.setCollectComments(true);
  auto ast = parser.parseString(
      "function main() i32 { return /* ** * *** */ 5; }");
  ASSERT_NE(ast, nullptr);
  ASSERT_EQ(parser.getComments().size(), 1u);
  EXPECT_EQ(parser.getComments().begin()->second.text, "/* ** * *** */");
}
