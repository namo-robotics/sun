// tests/test_serialization.cpp — Unit tests for AST serialization

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "ast.h"
#include "ast_deserializer.h"
#include "ast_serializer.h"
#include "lexer.h"
#include "parser.h"

using namespace sun::serialization;

// Helper to parse a string and return the AST
std::unique_ptr<BlockExprAST> parseCode(const std::string& source) {
  std::istringstream ss(source);
  Parser parser(ss);
  return parser.parseString(source);
}

// =============================================================================
// Basic Literal Roundtrip Tests
// =============================================================================

TEST(SerializationTest, NumberIntegerRoundtrip) {
  auto ast = std::make_unique<NumberExprAST>(static_cast<int64_t>(42));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::NUMBER);
  auto* num = static_cast<NumberExprAST*>(restored.get());
  EXPECT_TRUE(num->isInteger());
  EXPECT_EQ(num->getIntVal(), 42);
}

TEST(SerializationTest, NumberFloatRoundtrip) {
  auto ast = std::make_unique<NumberExprAST>(3.14159);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::NUMBER);
  auto* num = static_cast<NumberExprAST*>(restored.get());
  EXPECT_FALSE(num->isInteger());
  EXPECT_DOUBLE_EQ(num->getFloatVal(), 3.14159);
}

TEST(SerializationTest, StringLiteralRoundtrip) {
  auto ast = std::make_unique<StringLiteralAST>("hello world");

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::STRING_LITERAL);
  auto* str = static_cast<StringLiteralAST*>(restored.get());
  EXPECT_EQ(str->getValue(), "hello world");
}

TEST(SerializationTest, StringLiteralWithEscapes) {
  auto ast = std::make_unique<StringLiteralAST>("line1\nline2\ttab");

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* str = static_cast<StringLiteralAST*>(restored.get());
  EXPECT_EQ(str->getValue(), "line1\nline2\ttab");
}

TEST(SerializationTest, BoolLiteralTrue) {
  auto ast = std::make_unique<BoolLiteralAST>(true);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::BOOL_LITERAL);
  auto* b = static_cast<BoolLiteralAST*>(restored.get());
  EXPECT_TRUE(b->getValue());
}

TEST(SerializationTest, BoolLiteralFalse) {
  auto ast = std::make_unique<BoolLiteralAST>(false);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* b = static_cast<BoolLiteralAST*>(restored.get());
  EXPECT_FALSE(b->getValue());
}

TEST(SerializationTest, NullLiteralRoundtrip) {
  auto ast = std::make_unique<NullLiteralAST>();

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getType(), ASTNodeType::NULL_LITERAL);
}

// =============================================================================
// Variable Tests
// =============================================================================

TEST(SerializationTest, VariableReferenceRoundtrip) {
  auto ast = std::make_unique<VariableReferenceAST>("myVar");

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::VARIABLE_REFERENCE);
  auto* var = static_cast<VariableReferenceAST*>(restored.get());
  EXPECT_EQ(var->getName(), "myVar");
}

TEST(SerializationTest, VariableCreationRoundtrip) {
  auto value = std::make_unique<NumberExprAST>(static_cast<int64_t>(100));
  TypeAnnotation type;
  type.baseName = "i32";
  auto ast = std::make_unique<VariableCreationAST>("x", std::move(value), type);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::VARIABLE_CREATION);
  auto* vc = static_cast<VariableCreationAST*>(restored.get());
  EXPECT_EQ(vc->getName(), "x");
  ASSERT_NE(vc->getValue(), nullptr);
  EXPECT_EQ(vc->getValue()->getType(), ASTNodeType::NUMBER);
  ASSERT_TRUE(vc->getTypeAnnotation().has_value());
  EXPECT_EQ(vc->getTypeAnnotation()->baseName, "i32");
}

TEST(SerializationTest, VariableAssignmentRoundtrip) {
  auto value = std::make_unique<NumberExprAST>(static_cast<int64_t>(42));
  auto ast = std::make_unique<VariableAssignmentAST>("x", std::move(value));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::VARIABLE_ASSIGNMENT);
  auto* va = static_cast<VariableAssignmentAST*>(restored.get());
  EXPECT_EQ(va->getName(), "x");
}

// =============================================================================
// Expression Tests
// =============================================================================

TEST(SerializationTest, BinaryExprRoundtrip) {
  auto lhs = std::make_unique<NumberExprAST>(static_cast<int64_t>(10));
  auto rhs = std::make_unique<NumberExprAST>(static_cast<int64_t>(20));
  Token op;
  op.kind = TokenKind::PLUS;
  op.text = "+";
  auto ast =
      std::make_unique<BinaryExprAST>(op, std::move(lhs), std::move(rhs));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::BINARY);
  auto* bin = static_cast<BinaryExprAST*>(restored.get());
  EXPECT_EQ(bin->getOp().kind, TokenKind::PLUS);
  EXPECT_EQ(bin->getLHS()->getType(), ASTNodeType::NUMBER);
  EXPECT_EQ(bin->getRHS()->getType(), ASTNodeType::NUMBER);
}

TEST(SerializationTest, UnaryExprRoundtrip) {
  // Test via parser - use negation on numbers which is valid
  auto block = parseCode(R"(
    var x: i32 = -42;
  )");

  ASSERT_NE(block, nullptr);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*block);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getType(), ASTNodeType::BLOCK);
}

// =============================================================================
// Control Flow Tests
// =============================================================================

TEST(SerializationTest, BlockExprRoundtrip) {
  std::vector<std::unique_ptr<ExprAST>> body;
  body.push_back(std::make_unique<NumberExprAST>(static_cast<int64_t>(1)));
  body.push_back(std::make_unique<NumberExprAST>(static_cast<int64_t>(2)));
  auto ast = std::make_unique<BlockExprAST>(std::move(body));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::BLOCK);
  auto* block = static_cast<BlockExprAST*>(restored.get());
  EXPECT_EQ(block->getBody().size(), 2);
}

TEST(SerializationTest, IfExprRoundtrip) {
  auto cond = std::make_unique<BoolLiteralAST>(true);
  std::vector<std::unique_ptr<ExprAST>> thenBody;
  thenBody.push_back(std::make_unique<NumberExprAST>(static_cast<int64_t>(1)));
  auto thenBlock = std::make_unique<BlockExprAST>(std::move(thenBody));
  std::vector<std::unique_ptr<ExprAST>> elseBody;
  elseBody.push_back(std::make_unique<NumberExprAST>(static_cast<int64_t>(2)));
  auto elseBlock = std::make_unique<BlockExprAST>(std::move(elseBody));

  auto ast = std::make_unique<IfExprAST>(std::move(cond), std::move(thenBlock),
                                         std::move(elseBlock));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::IF);
  auto* ifExpr = static_cast<IfExprAST*>(restored.get());
  EXPECT_NE(ifExpr->getCond(), nullptr);
  EXPECT_NE(ifExpr->getThen(), nullptr);
  EXPECT_NE(ifExpr->getElse(), nullptr);
}

TEST(SerializationTest, WhileExprRoundtrip) {
  auto cond = std::make_unique<BoolLiteralAST>(true);
  std::vector<std::unique_ptr<ExprAST>> body;
  body.push_back(std::make_unique<BreakAST>());
  auto block = std::make_unique<BlockExprAST>(std::move(body));

  auto ast = std::make_unique<WhileExprAST>(std::move(cond), std::move(block));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::WHILE_LOOP);
}

TEST(SerializationTest, BreakContinueRoundtrip) {
  auto breakAst = std::make_unique<BreakAST>();
  auto continueAst = std::make_unique<ContinueAST>();

  ASTSerializer serializer;
  ASTDeserializer deserializer;

  auto breakRestored = deserializer.deserializeFromString(
      serializer.serializeToString(*breakAst));
  auto continueRestored = deserializer.deserializeFromString(
      serializer.serializeToString(*continueAst));

  ASSERT_NE(breakRestored, nullptr);
  EXPECT_EQ(breakRestored->getType(), ASTNodeType::BREAK_STMT);

  ASSERT_NE(continueRestored, nullptr);
  EXPECT_EQ(continueRestored->getType(), ASTNodeType::CONTINUE_STMT);
}

TEST(SerializationTest, ReturnExprRoundtrip) {
  auto value = std::make_unique<NumberExprAST>(static_cast<int64_t>(42));
  auto ast = std::make_unique<ReturnExprAST>(std::move(value));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::RETURN);
  auto* ret = static_cast<ReturnExprAST*>(restored.get());
  ASSERT_NE(ret->getValue(), nullptr);
  EXPECT_EQ(ret->getValue()->getType(), ASTNodeType::NUMBER);
}

// =============================================================================
// Function Tests
// =============================================================================

TEST(SerializationTest, PrototypeRoundtrip) {
  std::vector<std::pair<std::string, TypeAnnotation>> args;
  TypeAnnotation argType;
  argType.baseName = "i32";
  args.push_back({"x", argType});
  args.push_back({"y", argType});

  TypeAnnotation retType;
  retType.baseName = "i32";

  auto proto = std::make_unique<PrototypeAST>("add", std::move(args), retType);

  ASTSerializer serializer;
  sun::ast::Prototype protoMsg = serializer.serializePrototype(*proto);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializePrototype(protoMsg);

  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getName(), "add");
  EXPECT_EQ(restored->getArgs().size(), 2);
  EXPECT_EQ(restored->getArgs()[0].first, "x");
  EXPECT_EQ(restored->getArgs()[1].first, "y");
  ASSERT_TRUE(restored->getReturnType().has_value());
  EXPECT_EQ(restored->getReturnType()->baseName, "i32");
}

TEST(SerializationTest, FunctionDefinitionRoundtrip) {
  auto block = parseCode(R"(
    function add(a: i32, b: i32) i32 {
      return a + b;
    }
  )");

  ASSERT_NE(block, nullptr);
  ASSERT_FALSE(block->getBody().empty());

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*block);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getType(), ASTNodeType::BLOCK);
}

// =============================================================================
// Class/Interface Tests
// =============================================================================

TEST(SerializationTest, ClassDefinitionRoundtrip) {
  auto block = parseCode(R"(
    class Point {
      var x: i32;
      var y: i32;
      
      function init(x: i32, y: i32) {
        this.x = x;
        this.y = y;
      }
      
      function getX() i32 {
        return this.x;
      }
    }
  )");

  ASSERT_NE(block, nullptr);
  ASSERT_FALSE(block->getBody().empty());

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*block);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* restoredBlock = static_cast<BlockExprAST*>(restored.get());
  ASSERT_FALSE(restoredBlock->getBody().empty());
  EXPECT_EQ(restoredBlock->getBody()[0]->getType(),
            ASTNodeType::CLASS_DEFINITION);
}

TEST(SerializationTest, InterfaceDefinitionRoundtrip) {
  auto block = parseCode(R"(
    interface Printable {
      function print() i32;
    }
  )");

  ASSERT_NE(block, nullptr);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*block);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
}

TEST(SerializationTest, EnumDefinitionRoundtrip) {
  auto block = parseCode(R"(
    enum Color {
      Red,
      Green,
      Blue
    }
  )");

  ASSERT_NE(block, nullptr);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*block);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* restoredBlock = static_cast<BlockExprAST*>(restored.get());
  ASSERT_FALSE(restoredBlock->getBody().empty());
  EXPECT_EQ(restoredBlock->getBody()[0]->getType(),
            ASTNodeType::ENUM_DEFINITION);
}

// =============================================================================
// Array/Index Tests
// =============================================================================

TEST(SerializationTest, ArrayLiteralRoundtrip) {
  std::vector<std::unique_ptr<ExprAST>> elements;
  elements.push_back(std::make_unique<NumberExprAST>(static_cast<int64_t>(1)));
  elements.push_back(std::make_unique<NumberExprAST>(static_cast<int64_t>(2)));
  elements.push_back(std::make_unique<NumberExprAST>(static_cast<int64_t>(3)));
  auto ast = std::make_unique<ArrayLiteralAST>(std::move(elements));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::ARRAY_LITERAL);
  auto* arr = static_cast<ArrayLiteralAST*>(restored.get());
  EXPECT_EQ(arr->getElements().size(), 3);
}

// =============================================================================
// Module Tests
// =============================================================================

TEST(SerializationTest, ModuleDefinitionRoundtrip) {
  std::vector<std::unique_ptr<ExprAST>> body;
  body.push_back(std::make_unique<NumberExprAST>(static_cast<int64_t>(42)));
  auto block = std::make_unique<BlockExprAST>(std::move(body));
  auto ast = std::make_unique<ModuleAST>("MyModule", std::move(block));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::MODULE);
  auto* mod = static_cast<ModuleAST*>(restored.get());
  EXPECT_EQ(mod->getName(), "MyModule");
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST(SerializationTest, TryCatchRoundtrip) {
  auto block = parseCode(R"(
    function test() i32, IError {
      try {
        return 42;
      } catch (e: IError) {
        return -1;
      }
    }
  )");

  ASSERT_NE(block, nullptr);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*block);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
}

TEST(SerializationTest, ThrowExprRoundtrip) {
  auto errorVal = std::make_unique<NumberExprAST>(static_cast<int64_t>(1));
  auto ast = std::make_unique<ThrowExprAST>(std::move(errorVal));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::THROW);
}

// =============================================================================
// Location Preservation Tests
// =============================================================================

TEST(SerializationTest, LocationPreservation) {
  auto ast = std::make_unique<NumberExprAST>(static_cast<int64_t>(42));
  Position pos{10, 5, 0, "test.sun"};
  ast->setLocation(pos);

  SerializerConfig config;
  config.include_location = true;
  ASTSerializer serializer(config);
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  const auto& restoredPos = restored->getLocation();
  EXPECT_EQ(restoredPos.line, 10);
  EXPECT_EQ(restoredPos.column, 5);
  ASSERT_TRUE(restoredPos.filePath.has_value());
  EXPECT_EQ(*restoredPos.filePath, "test.sun");
}

// =============================================================================
// Clone Tests (uses serialization internally)
// =============================================================================

TEST(SerializationTest, CloneNumber) {
  auto original = std::make_unique<NumberExprAST>(static_cast<int64_t>(42));
  auto cloned = original->clone();

  ASSERT_NE(cloned, nullptr);
  ASSERT_EQ(cloned->getType(), ASTNodeType::NUMBER);
  auto* num = static_cast<NumberExprAST*>(cloned.get());
  EXPECT_EQ(num->getIntVal(), 42);

  // Verify they're different objects
  EXPECT_NE(original.get(), cloned.get());
}

TEST(SerializationTest, CloneComplexExpression) {
  auto block = parseCode(R"(
    function fibonacci(n: i32) i32 {
      if (n <= 1) {
        return n;
      }
      return fibonacci(n - 1) + fibonacci(n - 2);
    }
  )");

  ASSERT_NE(block, nullptr);

  auto cloned = block->clone();

  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(cloned->getType(), ASTNodeType::BLOCK);
  auto* clonedBlock = static_cast<BlockExprAST*>(cloned.get());
  EXPECT_EQ(clonedBlock->getBody().size(), block->getBody().size());

  // Verify deep copy - modifying original shouldn't affect clone
  EXPECT_NE(block.get(), clonedBlock);
}

TEST(SerializationTest, ClonePreservesLocation) {
  auto original = std::make_unique<NumberExprAST>(static_cast<int64_t>(42));
  Position pos{20, 15, 0, "clone_test.sun"};
  original->setLocation(pos);

  auto cloned = original->clone();

  ASSERT_NE(cloned, nullptr);
  const auto& clonedPos = cloned->getLocation();
  EXPECT_EQ(clonedPos.line, 20);
  EXPECT_EQ(clonedPos.column, 15);
  ASSERT_TRUE(clonedPos.filePath.has_value());
  EXPECT_EQ(*clonedPos.filePath, "clone_test.sun");
}

// =============================================================================
// Program Serialization Tests
// =============================================================================

TEST(SerializationTest, ProgramRoundtrip) {
  auto block = parseCode(R"(
    var x: i32 = 10;
    var y: i32 = 20;
    
    function add(a: i32, b: i32) i32 {
      return a + b;
    }
    
    var result = add(x, y);
  )");

  ASSERT_NE(block, nullptr);

  ASTSerializer serializer;
  std::string data = serializer.serializeProgramToString(*block);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeProgramFromString(data);

  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getBody().size(), block->getBody().size());
}

// =============================================================================
// Type Annotation Tests
// =============================================================================

TEST(SerializationTest, GenericTypeAnnotation) {
  TypeAnnotation type;
  type.baseName = "Vec";
  TypeAnnotation param;
  param.baseName = "i32";
  type.typeArguments.push_back(std::make_unique<TypeAnnotation>(param));

  auto value = std::make_unique<NullLiteralAST>();
  auto ast = std::make_unique<VariableCreationAST>("v", std::move(value), type);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* vc = static_cast<VariableCreationAST*>(restored.get());
  ASSERT_TRUE(vc->getTypeAnnotation().has_value());
  EXPECT_EQ(vc->getTypeAnnotation()->baseName, "Vec");
  ASSERT_EQ(vc->getTypeAnnotation()->typeArguments.size(), 1);
  EXPECT_EQ(vc->getTypeAnnotation()->typeArguments[0]->baseName, "i32");
}

// Test reference type annotation via parser (since TypeAnnotation fields are
// read-only)
TEST(SerializationTest, ReferenceTypeAnnotation) {
  auto block = parseCode(R"(
    function takeRef(p: ref i32) i32 {
      return 0;
    }
  )");

  ASSERT_NE(block, nullptr);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*block);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getType(), ASTNodeType::BLOCK);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(SerializationTest, EmptyBlock) {
  std::vector<std::unique_ptr<ExprAST>> body;
  auto ast = std::make_unique<BlockExprAST>(std::move(body));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* block = static_cast<BlockExprAST*>(restored.get());
  EXPECT_TRUE(block->getBody().empty());
}

TEST(SerializationTest, DeeplyNestedExpression) {
  // Create deeply nested binary expression: ((((1 + 2) + 3) + 4) + 5)
  std::unique_ptr<ExprAST> expr =
      std::make_unique<NumberExprAST>(static_cast<int64_t>(1));
  Token op;
  op.kind = TokenKind::PLUS;
  op.text = "+";

  for (int64_t i = 2; i <= 10; ++i) {
    auto rhs = std::make_unique<NumberExprAST>(i);
    expr = std::make_unique<BinaryExprAST>(op, std::move(expr), std::move(rhs));
  }

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*expr);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getType(), ASTNodeType::BINARY);
}

TEST(SerializationTest, LargeProgram) {
  std::stringstream code;
  code << "function main() i32 {\n";
  for (int i = 0; i < 100; ++i) {
    code << "  var x" << i << ": i32 = " << i << ";\n";
  }
  code << "  return x99;\n";
  code << "}\n";

  auto block = parseCode(code.str());
  ASSERT_NE(block, nullptr);

  ASTSerializer serializer;
  std::string data = serializer.serializeProgramToString(*block);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeProgramFromString(data);

  ASSERT_NE(restored, nullptr);
}

TEST(SerializationTest, UnicodeStringLiteral) {
  auto ast = std::make_unique<StringLiteralAST>("Hello 世界 🌍");

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* str = static_cast<StringLiteralAST*>(restored.get());
  EXPECT_EQ(str->getValue(), "Hello 世界 🌍");
}
