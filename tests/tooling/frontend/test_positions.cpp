// tests/tooling/frontend/test_positions.cpp — Parser source-span tests
//
// Verifies that the parser stamps accurate [start, end) spans on AST nodes.
// The core check: slicing the source with the node's byte offsets yields
// exactly the source text of that construct.

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "ast.h"
#include "parsing/parser.h"

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

TEST(Tooling_Frontend_Positions, NumberLiteral) {
  std::string src = "var x: i32 = 42;";
  auto block = parseSource(src);
  ASSERT_EQ(block->getBody().size(), 1u);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  EXPECT_EQ(spanText(src, *var), "var x: i32 = 42");
  EXPECT_EQ(spanText(src, *var->getValue()), "42");
}

TEST(Tooling_Frontend_Positions, BinaryExpressionSpans) {
  std::string src = "var x: i32 = 1 + 23;";
  auto block = parseSource(src);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  auto* bin = static_cast<const BinaryExprAST*>(var->getValue());
  ASSERT_EQ(bin->getType(), ASTNodeType::BINARY);
  EXPECT_EQ(spanText(src, *bin), "1 + 23");
  EXPECT_EQ(spanText(src, *bin->getLHS()), "1");
  EXPECT_EQ(spanText(src, *bin->getRHS()), "23");
}

TEST(Tooling_Frontend_Positions, CallAndMemberAccess) {
  std::string src = "foo.bar(1, 2);";
  auto block = parseSource(src);
  auto* call = static_cast<CallExprAST*>(block->getBody()[0].get());
  ASSERT_EQ(call->getType(), ASTNodeType::CALL);
  EXPECT_EQ(spanText(src, *call), "foo.bar(1, 2)");
  EXPECT_EQ(spanText(src, *call->getCallee()), "foo.bar");
  auto* member = static_cast<const MemberAccessAST*>(call->getCallee());
  EXPECT_EQ(spanText(src, *member->getObject()), "foo");
}

TEST(Tooling_Frontend_Positions, IfElseStatement) {
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

TEST(Tooling_Frontend_Positions, PrototypeSpan) {
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

TEST(Tooling_Frontend_Positions, WhileLoop) {
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

TEST(Tooling_Frontend_Positions, ReturnStatement) {
  std::string src = "function f() i32 { return 1 + 2; }";
  auto block = parseSource(src);
  auto* func = static_cast<FunctionAST*>(block->getBody()[0].get());
  auto* ret = func->getBody().getBody()[0].get();
  ASSERT_EQ(ret->getType(), ASTNodeType::RETURN);
  EXPECT_EQ(spanText(src, *ret), "return 1 + 2;");
}

TEST(Tooling_Frontend_Positions, ClassDefinition) {
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

TEST(Tooling_Frontend_Positions, GenericVsComparisonBacktracking) {
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

TEST(Tooling_Frontend_Positions, GenericCallSpan) {
  std::string src = "var v: i32 = create<i32>(1);";
  auto block = parseSource(src);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  ASSERT_EQ(var->getValue()->getType(), ASTNodeType::GENERIC_CALL);
  EXPECT_EQ(spanText(src, *var->getValue()), "create<i32>(1)");
}

TEST(Tooling_Frontend_Positions, ForLoopAfterBacktrack) {
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

TEST(Tooling_Frontend_Positions, TernarySpan) {
  std::string src = "var x: i32 = a > 0 ? 1 : 2;";
  auto block = parseSource(src);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  ASSERT_EQ(var->getValue()->getType(), ASTNodeType::TERNARY);
  EXPECT_EQ(spanText(src, *var->getValue()), "a > 0 ? 1 : 2");
}

TEST(Tooling_Frontend_Positions, UnaryAndIndexSpans) {
  std::string src = "var x: i32 = -arr[2];";
  auto block = parseSource(src);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  auto* unary = var->getValue();
  ASSERT_EQ(unary->getType(), ASTNodeType::UNARY);
  EXPECT_EQ(spanText(src, *unary), "-arr[2]");
}

TEST(Tooling_Frontend_Positions, BlockSpan) {
  std::string src = "function f() i32 { return 1; }";
  auto block = parseSource(src);
  auto* func = static_cast<FunctionAST*>(block->getBody()[0].get());
  EXPECT_EQ(spanText(src, func->getBody()), "{ return 1; }");
}

TEST(Tooling_Frontend_Positions, LineAndColumnTracking) {
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

// ------------------------------------------------------------------
// TypeAnnotation span tests (formatter relies on slicing these)
// ------------------------------------------------------------------

TEST(Tooling_Frontend_TypeSpans, VarAnnotation) {
  std::string src = "var x: i32 = 42;";
  auto block = parseSource(src);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  ASSERT_TRUE(var->getTypeAnnotation().has_value());
  EXPECT_EQ(spanText(src, var->getTypeAnnotation()->span), "i32");
}

TEST(Tooling_Frontend_TypeSpans, PointerAndRefTypes) {
  std::string src =
      "function f(a: ref Foo, b: raw_ptr<Bar>, c: ptr<Baz>, "
      "d: static_ptr<u8>) void {}";
  auto block = parseSource(src);
  auto* fn = static_cast<FunctionAST*>(block->getBody()[0].get());
  const auto& args = fn->getProto().getArgs();
  ASSERT_EQ(args.size(), 4u);
  EXPECT_EQ(spanText(src, args[0].second.span), "ref Foo");
  EXPECT_EQ(spanText(src, args[1].second.span), "raw_ptr<Bar>");
  EXPECT_EQ(spanText(src, args[2].second.span), "ptr<Baz>");
  EXPECT_EQ(spanText(src, args[3].second.span), "static_ptr<u8>");
}

TEST(Tooling_Frontend_TypeSpans, GenericsAndArrays) {
  std::string src =
      "function f(a: Map<string, i32>, b: array<i32, 3>, "
      "c: Vec<Vec<i32>>) void {}";
  auto block = parseSource(src);
  auto* fn = static_cast<FunctionAST*>(block->getBody()[0].get());
  const auto& args = fn->getProto().getArgs();
  ASSERT_EQ(args.size(), 3u);
  EXPECT_EQ(spanText(src, args[0].second.span), "Map<string, i32>");
  EXPECT_EQ(spanText(src, args[1].second.span), "array<i32, 3>");
  EXPECT_EQ(spanText(src, args[2].second.span), "Vec<Vec<i32>>");
  // Nested type argument spans
  const auto& mapArgs = args[0].second.typeArguments;
  ASSERT_EQ(mapArgs.size(), 2u);
  EXPECT_EQ(spanText(src, mapArgs[0]->span), "string");
  EXPECT_EQ(spanText(src, mapArgs[1]->span), "i32");
}

TEST(Tooling_Frontend_TypeSpans, ErrorUnionReturnType) {
  std::string src = "function divide(a: i32, b: i32) i32, IError { throw 1; }";
  auto block = parseSource(src);
  auto* fn = static_cast<FunctionAST*>(block->getBody()[0].get());
  const auto& ret = fn->getProto().getReturnType();
  ASSERT_TRUE(ret.has_value());
  EXPECT_TRUE(ret->canError);
  EXPECT_EQ(spanText(src, ret->span), "i32, IError");
}

TEST(Tooling_Frontend_TypeSpans, PlainReturnType) {
  std::string src = "function f() Foo.Bar {}";
  auto block = parseSource(src);
  auto* fn = static_cast<FunctionAST*>(block->getBody()[0].get());
  const auto& ret = fn->getProto().getReturnType();
  ASSERT_TRUE(ret.has_value());
  EXPECT_EQ(spanText(src, ret->span), "Foo.Bar");
}

TEST(Tooling_Frontend_TypeSpans, LambdaTypeBacktrackedComma) {
  // The ',' after the lambda type is tentatively eaten (IError check) and
  // pushed back; the span must not include it.
  std::string src = "function g(cb: (i32) i32, x: i32) void {}";
  auto block = parseSource(src);
  auto* fn = static_cast<FunctionAST*>(block->getBody()[0].get());
  const auto& args = fn->getProto().getArgs();
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(spanText(src, args[0].second.span), "(i32) i32");
  EXPECT_EQ(spanText(src, args[1].second.span), "i32");
}

TEST(Tooling_Frontend_TypeSpans, ThrowingLambdaType) {
  std::string src = "function g(cb: (i32) i32, IError) void {}";
  auto block = parseSource(src);
  auto* fn = static_cast<FunctionAST*>(block->getBody()[0].get());
  const auto& args = fn->getProto().getArgs();
  ASSERT_EQ(args.size(), 1u);
  EXPECT_TRUE(args[0].second.canError);
  EXPECT_EQ(spanText(src, args[0].second.span), "(i32) i32, IError");
}

TEST(Tooling_Frontend_TypeSpans, FnType) {
  std::string src = "function g(cb: _(i32, bool) void) void {}";
  auto block = parseSource(src);
  auto* fn = static_cast<FunctionAST*>(block->getBody()[0].get());
  const auto& args = fn->getProto().getArgs();
  ASSERT_EQ(args.size(), 1u);
  EXPECT_EQ(spanText(src, args[0].second.span), "_(i32, bool) void");
}
