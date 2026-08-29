// tests/tooling/frontend/test_lowering.cpp — LoweringPass tests
//
// The lowering pass strips ParenExprAST, desugars InterpolatedStringAST into
// sun.String append calls (stamping real source locations), and leaves all
// core AST nodes untouched.

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "ast.h"
#include "parsing/lowering_pass.h"
#include "parsing/parser.h"
#include "serialization/ast_serializer.h"

namespace {

std::unique_ptr<BlockExprAST> parseProgram(const std::string& source) {
  std::istringstream ss(source);
  Parser parser(ss);
  return parser.parseString(source);
}

std::string serialize(const BlockExprAST& block) {
  sun::serialization::ASTSerializer serializer;
  return serializer.serializeProgramToString(block);
}

// Count nodes of the given type anywhere in the tree
int countNodes(ExprAST& node, ASTNodeType type) {
  int count = node.getType() == type ? 1 : 0;
  node.forEachChildSlot([&](std::unique_ptr<ExprAST>& child) {
    if (child) count += countNodes(*child, type);
  });
  return count;
}

// The corpus exercises every child-bearing statement/expression kind so the
// traversal is proven not to drop or reorder children.
const char* kCorpus = R"(
using sun;

enum Color { Red, Green, Blue }

interface IShape {
    function area() i32;
    function describe() i32 { return 0; }
}

class Point implements IShape {
    var x: i32;
    var y: i32;

    init(x_: i32, y_: i32) {
        this.x = x_;
        this.y = y_;
    }

    function area() i32 {
        return this.x * this.y;
    }
}

function classify(d: i64) i64 {
    return match d {
        0 => 1,
        _ => 2
    };
}

function compute(n: i32) i32 throws IError {
    if (n < 0) { throw 1; }
    var total: i32 = 0;
    for (var i: i32 = 0; i < n; i = i + 1) {
        total += i;
    }
    while (total > 100) {
        total = total - 1;
        if (total == 150) { break; } else { continue; }
    }
    var arr = [1, 2, 3];
    var first: i32 = arr[0];
    arr[1] = first > 0 ? -first : ~first;
    var f = lambda (a: i32) i32 { return a * 2; };
    return f(total);
}

function main() i32 {
    try {
        var r = compute(10);
        return r;
    } catch (e: IError) {
        return -1;
    }
}
)";

}  // namespace

TEST(Tooling_Frontend_Lowering, StripsAllParenNodes) {
  auto block = parseProgram(kCorpus);
  ASSERT_NE(block, nullptr);
  // The corpus contains parens (if/while/for conditions, grouping)
  EXPECT_GT(countNodes(*block, ASTNodeType::PAREN_EXPR), 0);

  LoweringPass pass;
  pass.run(*block);

  EXPECT_EQ(countNodes(*block, ASTNodeType::PAREN_EXPR), 0);
  EXPECT_EQ(countNodes(*block, ASTNodeType::INTERPOLATED_STRING), 0);
  EXPECT_FALSE(pass.usedInterpolation());
}

TEST(Tooling_Frontend_Lowering, IdempotentOnCorpus) {
  auto block = parseProgram(kCorpus);
  ASSERT_NE(block, nullptr);

  LoweringPass first;
  first.run(*block);
  std::string once = serialize(*block);

  LoweringPass second;
  second.run(*block);
  EXPECT_EQ(serialize(*block), once);
}

TEST(Tooling_Frontend_Lowering, NoOpOnParenFreeProgram) {
  const char* src =
      "function f() i32 {\n"
      "    var x: i32 = 1 + 2;\n"
      "    return x;\n"
      "}";
  auto block = parseProgram(src);
  ASSERT_NE(block, nullptr);
  std::string before = serialize(*block);

  LoweringPass pass;
  pass.run(*block);

  EXPECT_EQ(serialize(*block), before);
}

TEST(Tooling_Frontend_Lowering, ParenStrippingPreservesInner) {
  auto block = parseProgram("var x: i32 = ((1 + 2)) * 3;");
  ASSERT_NE(block, nullptr);

  LoweringPass pass;
  pass.run(*block);

  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  auto* mul = static_cast<const BinaryExprAST*>(var->getValue());
  ASSERT_EQ(mul->getType(), ASTNodeType::BINARY);
  // The doubly-parenthesized LHS is now the bare addition
  ASSERT_EQ(mul->getLHS()->getType(), ASTNodeType::BINARY);
  auto* add = static_cast<const BinaryExprAST*>(mul->getLHS());
  EXPECT_EQ(add->getLHS()->getType(), ASTNodeType::NUMBER);
  EXPECT_EQ(add->getRHS()->getType(), ASTNodeType::NUMBER);
}

TEST(Tooling_Frontend_Lowering, DesugarsInterpolatedString) {
  std::string src = "var s = `x = ${(1 + 2)}`;";
  auto block = parseProgram(src);
  ASSERT_NE(block, nullptr);
  EXPECT_EQ(countNodes(*block, ASTNodeType::INTERPOLATED_STRING), 1);

  LoweringPass pass;
  pass.run(*block);

  EXPECT_TRUE(pass.usedInterpolation());
  EXPECT_EQ(countNodes(*block, ASTNodeType::INTERPOLATED_STRING), 0);
  // Parens inside ${...} are lowered too
  EXPECT_EQ(countNodes(*block, ASTNodeType::PAREN_EXPR), 0);

  // Desugared shape: block of [alloc var, string var, append_literal,
  // append, result ref]
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  ASSERT_EQ(var->getValue()->getType(), ASTNodeType::BLOCK);
  const auto* desugared = static_cast<const BlockExprAST*>(var->getValue());
  ASSERT_EQ(desugared->getBody().size(), 5u);
  EXPECT_EQ(desugared->getBody()[0]->getType(), ASTNodeType::VARIABLE_CREATION);
  EXPECT_EQ(desugared->getBody()[1]->getType(), ASTNodeType::VARIABLE_CREATION);
  EXPECT_EQ(desugared->getBody()[2]->getType(), ASTNodeType::CALL);
  EXPECT_EQ(desugared->getBody()[3]->getType(), ASTNodeType::CALL);
  EXPECT_EQ(desugared->getBody()[4]->getType(),
            ASTNodeType::VARIABLE_REFERENCE);
}

TEST(Tooling_Frontend_Lowering, DesugaredNodesCarryTemplateLocation) {
  std::string src = "var s = `hi`;";
  auto block = parseProgram(src);
  ASSERT_NE(block, nullptr);

  LoweringPass pass;
  pass.run(*block);

  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  ASSERT_EQ(var->getValue()->getType(), ASTNodeType::BLOCK);
  const auto* desugared = static_cast<const BlockExprAST*>(var->getValue());
  // Every synthetic node points at the template literal, not 1:1 defaults
  int templateCol = static_cast<int>(src.find('`')) + 1;
  for (const auto& stmt : desugared->getBody()) {
    EXPECT_EQ(stmt->getLocation().line, 1);
    EXPECT_EQ(stmt->getLocation().column, templateCol);
  }
}

TEST(Tooling_Frontend_Lowering,
     InterpolationSubExpressionPositionsAreAbsolute) {
  std::string src = "var s = `x = ${foo}`;";
  auto block = parseProgram(src);
  ASSERT_NE(block, nullptr);

  // Before lowering: the ${foo} sub-expression's offsets point into the file
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  ASSERT_EQ(var->getValue()->getType(), ASTNodeType::INTERPOLATED_STRING);
  const auto* interp =
      static_cast<const InterpolatedStringAST*>(var->getValue());
  const auto& segments = interp->getSegments();
  ASSERT_EQ(segments.size(), 2u);
  ASSERT_FALSE(segments[1].isLiteral);
  const auto& loc = segments[1].expression->getLocation();
  ASSERT_TRUE(loc.endOffset.has_value());
  EXPECT_EQ(src.substr(loc.offset, *loc.endOffset - loc.offset), "foo");
  EXPECT_EQ(loc.line, 1);
  EXPECT_EQ(loc.column, static_cast<int>(src.find("foo")) + 1);
}
