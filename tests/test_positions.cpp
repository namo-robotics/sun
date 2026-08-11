// tests/test_positions.cpp — Parser source-span tests
//
// Verifies that the parser stamps accurate [start, end) spans on AST nodes.
// The core check: slicing the source with the node's byte offsets yields
// exactly the source text of that construct.

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "ast.h"
#include "parser.h"

namespace {

std::unique_ptr<BlockExprAST> parseSource(const std::string& source) {
  std::istringstream ss(source);
  Parser parser(ss);
  return parser.parseString(source);
}

// Slice the source using the node's span; fails the test if no span is set
std::string spanText(const std::string& source, const ExprAST& node) {
  const Position& loc = node.getLocation();
  EXPECT_TRUE(loc.endOffset.has_value())
      << "node has no endOffset: " << node.toString();
  if (!loc.endOffset) return "";
  return source.substr(loc.offset, *loc.endOffset - loc.offset);
}

std::string spanText(const std::string& source, const Position& loc) {
  EXPECT_TRUE(loc.endOffset.has_value()) << "position has no endOffset";
  if (!loc.endOffset) return "";
  return source.substr(loc.offset, *loc.endOffset - loc.offset);
}

}  // namespace

TEST(PositionTest, NumberLiteral) {
  std::string src = "var x: i32 = 42;";
  auto block = parseSource(src);
  ASSERT_EQ(block->getBody().size(), 1u);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  EXPECT_EQ(spanText(src, *var), "var x: i32 = 42");
  EXPECT_EQ(spanText(src, *var->getValue()), "42");
}

TEST(PositionTest, BinaryExpressionSpans) {
  std::string src = "var x: i32 = 1 + 23;";
  auto block = parseSource(src);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  auto* bin = static_cast<const BinaryExprAST*>(var->getValue());
  ASSERT_EQ(bin->getType(), ASTNodeType::BINARY);
  EXPECT_EQ(spanText(src, *bin), "1 + 23");
  EXPECT_EQ(spanText(src, *bin->getLHS()), "1");
  EXPECT_EQ(spanText(src, *bin->getRHS()), "23");
}

TEST(PositionTest, CallAndMemberAccess) {
  std::string src = "foo.bar(1, 2);";
  auto block = parseSource(src);
  auto* call = static_cast<CallExprAST*>(block->getBody()[0].get());
  ASSERT_EQ(call->getType(), ASTNodeType::CALL);
  EXPECT_EQ(spanText(src, *call), "foo.bar(1, 2)");
  EXPECT_EQ(spanText(src, *call->getCallee()), "foo.bar");
  auto* member = static_cast<const MemberAccessAST*>(call->getCallee());
  EXPECT_EQ(spanText(src, *member->getObject()), "foo");
}

TEST(PositionTest, IfElseStatement) {
  std::string src =
      "function f(a: i32) i32 {\n"
      "    if (a > 1) { return 1; } else { return 2; }\n"
      "    return 0;\n"
      "}";
  auto block = parseSource(src);
  auto* func = static_cast<FunctionAST*>(block->getBody()[0].get());
  ASSERT_EQ(func->getType(), ASTNodeType::FUNCTION);
  EXPECT_EQ(spanText(src, *func), src);
  auto* ifExpr = static_cast<IfExprAST*>(
      const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
          func->getBody().getBody())[0]
          .get());
  ASSERT_EQ(ifExpr->getType(), ASTNodeType::IF);
  EXPECT_EQ(spanText(src, *ifExpr),
            "if (a > 1) { return 1; } else { return 2; }");
}

TEST(PositionTest, PrototypeSpan) {
  std::string src =
      "function add(a: i32, b: i32) i32 {\n"
      "    return a + b;\n"
      "}";
  auto block = parseSource(src);
  auto* func = static_cast<FunctionAST*>(block->getBody()[0].get());
  // Prototype span covers the signature up to (but excluding) the body
  EXPECT_EQ(spanText(src, func->getProto().getLocation()),
            "(a: i32, b: i32) i32");
}

TEST(PositionTest, WhileLoop) {
  std::string src =
      "function f() i32 {\n"
      "    var i: i32 = 0;\n"
      "    while (i < 10) { i = i + 1; }\n"
      "    return i;\n"
      "}";
  auto block = parseSource(src);
  auto* func = static_cast<FunctionAST*>(block->getBody()[0].get());
  const auto& body = func->getBody().getBody();
  ASSERT_GE(body.size(), 2u);
  EXPECT_EQ(spanText(src, *body[1]), "while (i < 10) { i = i + 1; }");
}

TEST(PositionTest, ReturnStatement) {
  std::string src = "function f() i32 { return 1 + 2; }";
  auto block = parseSource(src);
  auto* func = static_cast<FunctionAST*>(block->getBody()[0].get());
  auto* ret = func->getBody().getBody()[0].get();
  ASSERT_EQ(ret->getType(), ASTNodeType::RETURN);
  EXPECT_EQ(spanText(src, *ret), "return 1 + 2;");
}

TEST(PositionTest, ClassDefinition) {
  std::string src =
      "class Point {\n"
      "    var x: i32;\n"
      "    var y: i32;\n"
      "}";
  auto block = parseSource(src);
  auto* cls = static_cast<ClassDefinitionAST*>(block->getBody()[0].get());
  ASSERT_EQ(cls->getType(), ASTNodeType::CLASS_DEFINITION);
  EXPECT_EQ(spanText(src, *cls), src);
  // Field locations point at the field names
  ASSERT_EQ(cls->getFields().size(), 2u);
  EXPECT_EQ(spanText(src, cls->getFields()[0].location), "x");
  EXPECT_EQ(spanText(src, cls->getFields()[1].location), "y");
}

TEST(PositionTest, GenericVsComparisonBacktracking) {
  // 'a < b' comparison must survive the generic-call backtrack with correct
  // spans (this exercises prevTok_ save/restore)
  std::string src = "var r: bool = a < b;";
  auto block = parseSource(src);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  auto* bin = static_cast<const BinaryExprAST*>(var->getValue());
  ASSERT_EQ(bin->getType(), ASTNodeType::BINARY);
  EXPECT_EQ(spanText(src, *bin), "a < b");
  EXPECT_EQ(spanText(src, *bin->getLHS()), "a");
  EXPECT_EQ(spanText(src, *bin->getRHS()), "b");
}

TEST(PositionTest, GenericCallSpan) {
  std::string src = "var v: i32 = create<i32>(1);";
  auto block = parseSource(src);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  ASSERT_EQ(var->getValue()->getType(), ASTNodeType::GENERIC_CALL);
  EXPECT_EQ(spanText(src, *var->getValue()), "create<i32>(1)");
}

TEST(PositionTest, ForLoopAfterBacktrack) {
  // Traditional for loop goes through the for-in backtrack path
  std::string src =
      "function f() i32 {\n"
      "    for (var i: i32 = 0; i < 3; i = i + 1) { var x: i32 = i; }\n"
      "    return 0;\n"
      "}";
  auto block = parseSource(src);
  auto* func = static_cast<FunctionAST*>(block->getBody()[0].get());
  auto* forLoop = func->getBody().getBody()[0].get();
  ASSERT_EQ(forLoop->getType(), ASTNodeType::FOR_LOOP);
  EXPECT_EQ(spanText(src, *forLoop),
            "for (var i: i32 = 0; i < 3; i = i + 1) { var x: i32 = i; }");
}

TEST(PositionTest, TernarySpan) {
  std::string src = "var x: i32 = a > 0 ? 1 : 2;";
  auto block = parseSource(src);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  ASSERT_EQ(var->getValue()->getType(), ASTNodeType::TERNARY);
  EXPECT_EQ(spanText(src, *var->getValue()), "a > 0 ? 1 : 2");
}

TEST(PositionTest, UnaryAndIndexSpans) {
  std::string src = "var x: i32 = -arr[2];";
  auto block = parseSource(src);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  auto* unary = var->getValue();
  ASSERT_EQ(unary->getType(), ASTNodeType::UNARY);
  EXPECT_EQ(spanText(src, *unary), "-arr[2]");
}

TEST(PositionTest, BlockSpan) {
  std::string src = "function f() i32 { return 1; }";
  auto block = parseSource(src);
  auto* func = static_cast<FunctionAST*>(block->getBody()[0].get());
  EXPECT_EQ(spanText(src, func->getBody()), "{ return 1; }");
}

TEST(PositionTest, LineAndColumnTracking) {
  std::string src =
      "var a: i32 = 1;\n"
      "var b: i32 = 2;";
  auto block = parseSource(src);
  ASSERT_EQ(block->getBody().size(), 2u);
  const auto& locA = block->getBody()[0]->getLocation();
  const auto& locB = block->getBody()[1]->getLocation();
  EXPECT_EQ(locA.line, 1);
  EXPECT_EQ(locA.column, 1);
  EXPECT_EQ(locB.line, 2);
  EXPECT_EQ(locB.column, 1);
  ASSERT_TRUE(locB.endLine.has_value());
  EXPECT_EQ(*locB.endLine, 2);
}
