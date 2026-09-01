// tests/tooling/serialization/test_serialization.cpp — Unit tests for AST
// serialization

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include "ast.h"
#include "moon_bundling/module_types.h"
#include "parsing/lexer.h"
#include "parsing/parser.h"
#include "serialization/ast_deserializer.h"
#include "serialization/ast_serializer.h"
#include "serialization/token_kind_proto_map.h"

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

TEST(Tooling_Serialization, NumberIntegerRoundtrip) {
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

TEST(Tooling_Serialization, NumberFloatRoundtrip) {
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

TEST(Tooling_Serialization, NumberSuffixRoundtrip) {
  auto ast = std::make_unique<NumberExprAST>(static_cast<int64_t>(21), "u8");

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::NUMBER);
  auto* num = static_cast<NumberExprAST*>(restored.get());
  EXPECT_TRUE(num->isInteger());
  EXPECT_EQ(num->getIntVal(), 21);
  EXPECT_EQ(num->getSuffix(), "u8");
}

TEST(Tooling_Serialization, StringLiteralRoundtrip) {
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

TEST(Tooling_Serialization, StringLiteralWithEscapes) {
  auto ast = std::make_unique<StringLiteralAST>("line1\nline2\ttab");

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* str = static_cast<StringLiteralAST*>(restored.get());
  EXPECT_EQ(str->getValue(), "line1\nline2\ttab");
}

TEST(Tooling_Serialization, BoolLiteralTrue) {
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

TEST(Tooling_Serialization, BoolLiteralFalse) {
  auto ast = std::make_unique<BoolLiteralAST>(false);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* b = static_cast<BoolLiteralAST*>(restored.get());
  EXPECT_FALSE(b->getValue());
}

// Both literal forms share one node, so the round-trip must keep the value
// and which form it was.
TEST(Tooling_Serialization, CharLiteralRoundtrip) {
  auto ast = std::make_unique<CharLiteralAST>(0x1F600, /*isByte=*/false);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::CHAR_LITERAL);
  auto* c = static_cast<CharLiteralAST*>(restored.get());
  EXPECT_EQ(c->getValue(), 0x1F600u);
  EXPECT_FALSE(c->isByte());
}

TEST(Tooling_Serialization, ByteLiteralRoundtrip) {
  auto ast = std::make_unique<CharLiteralAST>(0xFF, /*isByte=*/true);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::CHAR_LITERAL);
  auto* c = static_cast<CharLiteralAST*>(restored.get());
  EXPECT_EQ(c->getValue(), 0xFFu);
  EXPECT_TRUE(c->isByte());
}

TEST(Tooling_Serialization, NullLiteralRoundtrip) {
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

TEST(Tooling_Serialization, VariableReferenceRoundtrip) {
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

TEST(Tooling_Serialization, VariableCreationRoundtrip) {
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

TEST(Tooling_Serialization, VariableAssignmentRoundtrip) {
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

TEST(Tooling_Serialization, BinaryExprRoundtrip) {
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

// Every kind in the shared TokenKind<->proto table must survive a roundtrip,
// so a newly mapped operator is covered automatically
TEST(Tooling_Serialization, MappedOpTokenKindsRoundtrip) {
  for (const auto& [kind, protoKind] : kTokenKindProtoMap) {
    auto lhs = std::make_unique<NumberExprAST>(static_cast<int64_t>(10));
    auto rhs = std::make_unique<NumberExprAST>(static_cast<int64_t>(3));
    Token op = Token::make(kind, Position{}, Position{});
    auto ast =
        std::make_unique<BinaryExprAST>(op, std::move(lhs), std::move(rhs));

    ASTSerializer serializer;
    std::string data = serializer.serializeToString(*ast);

    ASTDeserializer deserializer;
    auto restored = deserializer.deserializeFromString(data);

    ASSERT_NE(restored, nullptr);
    ASSERT_EQ(restored->getType(), ASTNodeType::BINARY);
    auto* bin = static_cast<BinaryExprAST*>(restored.get());
    EXPECT_EQ(bin->getOp().kind, kind)
        << "operator " << getTokenInfo().at(kind).text;
  }
}

TEST(Tooling_Serialization, CompoundAssignmentRoundtrip) {
  auto target = std::make_unique<VariableReferenceAST>("x");
  auto value = std::make_unique<NumberExprAST>(static_cast<int64_t>(5));
  Token op = Token::make(TokenKind::PLUS_ASSIGN, Position{}, Position{});
  auto ast = std::make_unique<CompoundAssignmentAST>(std::move(target), op,
                                                     std::move(value));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::COMPOUND_ASSIGNMENT);
  auto* compound = static_cast<CompoundAssignmentAST*>(restored.get());
  EXPECT_EQ(compound->getOp().kind, TokenKind::PLUS_ASSIGN);
  EXPECT_EQ(compound->binaryOpKind(), TokenKind::PLUS);
  EXPECT_EQ(compound->getTarget()->getType(), ASTNodeType::VARIABLE_REFERENCE);
  EXPECT_EQ(compound->getValue()->getType(), ASTNodeType::NUMBER);
}

TEST(Tooling_Serialization, TernaryExprRoundtrip) {
  auto cond = std::make_unique<VariableReferenceAST>("c");
  auto thenExpr = std::make_unique<NumberExprAST>(static_cast<int64_t>(1));
  auto elseExpr = std::make_unique<NumberExprAST>(static_cast<int64_t>(2));
  auto ast = std::make_unique<TernaryExprAST>(
      std::move(cond), std::move(thenExpr), std::move(elseExpr), Position{});

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::TERNARY);
  auto* ternary = static_cast<TernaryExprAST*>(restored.get());
  EXPECT_EQ(ternary->getCond()->getType(), ASTNodeType::VARIABLE_REFERENCE);
  EXPECT_EQ(ternary->getThen()->getType(), ASTNodeType::NUMBER);
  EXPECT_EQ(ternary->getElse()->getType(), ASTNodeType::NUMBER);
}

TEST(Tooling_Serialization, UnaryExprRoundtrip) {
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

TEST(Tooling_Serialization, BlockExprRoundtrip) {
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

TEST(Tooling_Serialization, IfExprRoundtrip) {
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

TEST(Tooling_Serialization, WhileExprRoundtrip) {
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

TEST(Tooling_Serialization, BreakContinueRoundtrip) {
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

TEST(Tooling_Serialization, ReturnExprRoundtrip) {
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

TEST(Tooling_Serialization, PrototypeRoundtrip) {
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

TEST(Tooling_Serialization, FunctionDefinitionRoundtrip) {
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

TEST(Tooling_Serialization, ClassDefinitionRoundtrip) {
  auto block = parseCode(R"(
    class Point {
      var x: i32;
      var y: i32;
      
      init(x: i32, y: i32) {
        this.x = x;
        this.y = y;
      }
      
      method getX() i32 {
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

// Packing changes layout, so losing it across a .moon boundary would give
// caller and callee different offsets for the same class - silent corruption.
TEST(Tooling_Serialization, PackedClassModifierRoundtrip) {
  auto block = parseCode(R"(
    packed_class Header {
      var magic: u8;
      var length: i32;

      init() {}
    }
  )");

  ASSERT_NE(block, nullptr);
  ASSERT_FALSE(block->getBody().empty());
  ASSERT_TRUE(
      static_cast<ClassDefinitionAST*>(block->getBody()[0].get())->isPacked());

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*block);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* restoredBlock = static_cast<BlockExprAST*>(restored.get());
  ASSERT_FALSE(restoredBlock->getBody().empty());
  ASSERT_EQ(restoredBlock->getBody()[0]->getType(),
            ASTNodeType::CLASS_DEFINITION);
  EXPECT_TRUE(
      static_cast<ClassDefinitionAST*>(restoredBlock->getBody()[0].get())
          ->isPacked());
}

// Regression: isPartial_ used to be dropped because the deserializer passed
// is_partial into the ctor's `precompiled` parameter instead
TEST(Tooling_Serialization, PartialClassModifierRoundtrip) {
  auto block = parseCode(R"(
    partial class Extra {
      method helper() i32 { return 1; }
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
  EXPECT_TRUE(
      static_cast<ClassDefinitionAST*>(restoredBlock->getBody()[0].get())
          ->isPartial());
}

TEST(Tooling_Serialization, InterfaceDefinitionRoundtrip) {
  auto block = parseCode(R"(
    interface Printable {
      method print() i32;
    }
  )");

  ASSERT_NE(block, nullptr);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*block);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
}

TEST(Tooling_Serialization, EnumDefinitionRoundtrip) {
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

TEST(Tooling_Serialization, ArrayLiteralRoundtrip) {
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

TEST(Tooling_Serialization, ModuleDefinitionRoundtrip) {
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

TEST(Tooling_Serialization, TryCatchRoundtrip) {
  auto block = parseCode(R"(
    function test() i32 throws IError {
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

TEST(Tooling_Serialization, ThrowExprRoundtrip) {
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
// Lossless parse-tree node roundtrips (ParenExpr, InterpolatedString)
// =============================================================================

TEST(Tooling_Serialization, ParenExprRoundtrip) {
  auto block = parseCode("var x: i32 = (1 + 2);");
  ASSERT_NE(block, nullptr);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  ASSERT_EQ(var->getValue()->getType(), ASTNodeType::PAREN_EXPR);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*var->getValue());

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::PAREN_EXPR);
  auto* paren = static_cast<ParenExprAST*>(restored.get());
  ASSERT_NE(paren->getInner(), nullptr);
  EXPECT_EQ(paren->getInner()->getType(), ASTNodeType::BINARY);

  // clone() goes through the same proto roundtrip - must not be nullptr
  auto cloned = var->getValue()->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(cloned->getType(), ASTNodeType::PAREN_EXPR);
}

TEST(Tooling_Serialization, InterpolatedStringRoundtrip) {
  auto block = parseCode("var s = `Hello ${name}!`;");
  ASSERT_NE(block, nullptr);
  auto* var = static_cast<VariableCreationAST*>(block->getBody()[0].get());
  ASSERT_EQ(var->getValue()->getType(), ASTNodeType::INTERPOLATED_STRING);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*var->getValue());

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::INTERPOLATED_STRING);
  auto* interp = static_cast<InterpolatedStringAST*>(restored.get());
  EXPECT_EQ(interp->getRawContent(), "Hello ${name}!");
  const auto& segments = interp->getSegments();
  ASSERT_EQ(segments.size(), 3u);
  EXPECT_TRUE(segments[0].isLiteral);
  EXPECT_EQ(segments[0].rawText, "Hello ");
  EXPECT_FALSE(segments[1].isLiteral);
  ASSERT_NE(segments[1].expression, nullptr);
  EXPECT_EQ(segments[1].expression->getType(), ASTNodeType::VARIABLE_REFERENCE);
  EXPECT_TRUE(segments[2].isLiteral);

  // clone() must not silently return nullptr
  auto cloned = var->getValue()->clone();
  ASSERT_NE(cloned, nullptr);
  EXPECT_EQ(cloned->getType(), ASTNodeType::INTERPOLATED_STRING);
}

// =============================================================================
// Location Preservation Tests
// =============================================================================

TEST(Tooling_Serialization, LocationPreservation) {
  auto ast = std::make_unique<NumberExprAST>(static_cast<int64_t>(42));
  Position pos{10, 5, 0, "test.sun"};
  pos.setEnd(10, 7, 2);
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
  ASSERT_TRUE(restoredPos.endLine.has_value());
  EXPECT_EQ(*restoredPos.endLine, 10);
  ASSERT_TRUE(restoredPos.endColumn.has_value());
  EXPECT_EQ(*restoredPos.endColumn, 7);
  ASSERT_TRUE(restoredPos.endOffset.has_value());
  EXPECT_EQ(*restoredPos.endOffset, 2);
}

// =============================================================================
// Clone Tests (uses serialization internally)
// =============================================================================

TEST(Tooling_Serialization, CloneNumber) {
  auto original = std::make_unique<NumberExprAST>(static_cast<int64_t>(42));
  auto cloned = original->clone();

  ASSERT_NE(cloned, nullptr);
  ASSERT_EQ(cloned->getType(), ASTNodeType::NUMBER);
  auto* num = static_cast<NumberExprAST*>(cloned.get());
  EXPECT_EQ(num->getIntVal(), 42);

  // Verify they're different objects
  EXPECT_NE(original.get(), cloned.get());
}

TEST(Tooling_Serialization, CloneComplexExpression) {
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

TEST(Tooling_Serialization, ClonePreservesLocation) {
  auto original = std::make_unique<NumberExprAST>(static_cast<int64_t>(42));
  Position pos{20, 15, 0, "clone_test.sun"};
  pos.setEnd(20, 17, 2);
  original->setLocation(pos);

  auto cloned = original->clone();

  ASSERT_NE(cloned, nullptr);
  const auto& clonedPos = cloned->getLocation();
  EXPECT_EQ(clonedPos.line, 20);
  EXPECT_EQ(clonedPos.column, 15);
  ASSERT_TRUE(clonedPos.filePath.has_value());
  EXPECT_EQ(*clonedPos.filePath, "clone_test.sun");
  ASSERT_TRUE(clonedPos.endOffset.has_value());
  EXPECT_EQ(*clonedPos.endOffset, 2);
}

// =============================================================================
// Program Serialization Tests
// =============================================================================

TEST(Tooling_Serialization, ProgramRoundtrip) {
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

TEST(Tooling_Serialization, GenericTypeAnnotation) {
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
TEST(Tooling_Serialization, ReferenceTypeAnnotation) {
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
TEST(Tooling_Serialization, FunctionPointerTypeAnnotationRoundtrip) {
  auto block = parseCode(R"(
    function use(callback: function (i32, bool) i64 throws IError) void {}
  )");
  ASSERT_NE(block, nullptr);

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*block);
  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* restoredBlock = static_cast<BlockExprAST*>(restored.get());
  auto* function = static_cast<FunctionAST*>(restoredBlock->getBody()[0].get());
  const auto& type = function->getProto().getArgs()[0].second;
  EXPECT_TRUE(type.isFunction());
  EXPECT_TRUE(type.canError);
  EXPECT_EQ(type.toString(), "function (i32, bool) i64 throws IError");
}

TEST(Tooling_Serialization, ModuleFunctionPointerTypeSyntax) {
  auto current = sun::ModuleTypeResolver::parseTypeSignature(
      "function (i32, bool) i64 throws IError");
  ASSERT_NE(current, nullptr);
  ASSERT_EQ(current->getKind(), sun::Type::Kind::Function);
  EXPECT_EQ(current->toString(), "function (i32, bool) i64 throws IError");
}

TEST(Tooling_Serialization, ModuleLegacyFunctionPointerTypeCompatibility) {
  auto legacy = sun::ModuleTypeResolver::parseTypeSignature("(i32) -> i32");
  ASSERT_NE(legacy, nullptr);
  ASSERT_EQ(legacy->getKind(), sun::Type::Kind::Function);
  EXPECT_EQ(legacy->toString(), "function (i32) i32");
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(Tooling_Serialization, EmptyBlock) {
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

TEST(Tooling_Serialization, DeeplyNestedExpression) {
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

TEST(Tooling_Serialization, LargeProgram) {
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

TEST(Tooling_Serialization, UnicodeStringLiteral) {
  auto ast = std::make_unique<StringLiteralAST>("Hello 世界 🌍");

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* str = static_cast<StringLiteralAST*>(restored.get());
  EXPECT_EQ(str->getValue(), "Hello 世界 🌍");
}

// ============================================================================
// Payload enums + match destructuring
// ============================================================================

TEST(Tooling_Serialization, EnumPayloadRoundtrip) {
  std::vector<EnumVariantDecl> variants;
  variants.push_back({"Circle", 0, Position{}, {}});
  variants.back().payloadTypes.push_back(TypeAnnotation("f64"));
  variants.push_back({"Rect", 1, Position{}, {}});
  variants.back().payloadTypes.push_back(TypeAnnotation("f64"));
  variants.back().payloadTypes.push_back(TypeAnnotation("f64"));
  variants.push_back({"Empty", 2, Position{}, {}});
  auto ast = std::make_unique<EnumDefinitionAST>("Shape", std::move(variants));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::ENUM_DEFINITION);
  auto* enumDef = static_cast<EnumDefinitionAST*>(restored.get());
  ASSERT_EQ(enumDef->getNumVariants(), 3u);
  EXPECT_TRUE(enumDef->hasAnyPayload());
  const auto* circle = enumDef->getVariant("Circle");
  ASSERT_NE(circle, nullptr);
  ASSERT_EQ(circle->payloadTypes.size(), 1u);
  EXPECT_EQ(circle->payloadTypes[0].baseName, "f64");
  const auto* rect = enumDef->getVariant("Rect");
  ASSERT_NE(rect, nullptr);
  EXPECT_EQ(rect->payloadTypes.size(), 2u);
  const auto* empty = enumDef->getVariant("Empty");
  ASSERT_NE(empty, nullptr);
  EXPECT_FALSE(empty->hasPayload());
}

TEST(Tooling_Serialization, MatchBindingsRoundtrip) {
  // match x { Shape.Circle(r, _) => r, _ => y }
  auto disc = std::make_unique<VariableReferenceAST>("x");
  std::vector<MatchArm> arms;
  auto pattern = std::make_unique<MemberAccessAST>(
      std::make_unique<VariableReferenceAST>("Shape"), "Circle");
  arms.emplace_back(std::move(pattern), false,
                    std::make_unique<VariableReferenceAST>("r"));
  arms.back().hasPayloadParens = true;
  PatternBinding b1;
  b1.name = "r";
  arms.back().bindings.push_back(std::move(b1));
  PatternBinding b2;
  b2.isWildcard = true;
  arms.back().bindings.push_back(std::move(b2));
  arms.emplace_back(nullptr, true, std::make_unique<VariableReferenceAST>("y"));
  auto ast = std::make_unique<MatchExprAST>(std::move(disc), std::move(arms));

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::MATCH);
  auto* match = static_cast<MatchExprAST*>(restored.get());
  ASSERT_EQ(match->getArms().size(), 2u);
  const auto& arm = match->getArms()[0];
  EXPECT_TRUE(arm.hasPayloadParens);
  ASSERT_EQ(arm.bindings.size(), 2u);
  EXPECT_EQ(arm.bindings[0].name, "r");
  EXPECT_FALSE(arm.bindings[0].isWildcard);
  EXPECT_TRUE(arm.bindings[1].isWildcard);
  EXPECT_FALSE(match->getArms()[1].hasPayloadParens);
}

TEST(Tooling_Serialization, GenericEnumTypeParamsRoundtrip) {
  std::vector<EnumVariantDecl> variants;
  variants.push_back({"Some", 0, Position{}, {}});
  variants.back().payloadTypes.push_back(TypeAnnotation("T"));
  variants.push_back({"None", 1, Position{}, {}});
  auto ast = std::make_unique<EnumDefinitionAST>(
      "Option", std::move(variants),
      /*precompiled=*/false, std::vector<TypeParameter>{TypeParameter("T")});

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(restored->getType(), ASTNodeType::ENUM_DEFINITION);
  auto* enumDef = static_cast<EnumDefinitionAST*>(restored.get());
  EXPECT_TRUE(enumDef->isGeneric());
  ASSERT_EQ(enumDef->getTypeParameters().size(), 1u);
  EXPECT_EQ(enumDef->getTypeParameters()[0].name, "T");
  const auto* some = enumDef->getVariant("Some");
  ASSERT_NE(some, nullptr);
  ASSERT_EQ(some->payloadTypes.size(), 1u);
  EXPECT_EQ(some->payloadTypes[0].baseName, "T");
}

// Constraints must survive the wire: ExprAST::clone() is serialize followed by
// deserialize, and generic instantiation clones, so a constraint lost here
// would vanish from every specialization without any error.
TEST(Tooling_Serialization, TypeParameterConstraintRoundtrip) {
  std::vector<EnumVariantDecl> variants;
  variants.push_back({"Some", 0, Position{}, {}});
  variants.back().payloadTypes.push_back(TypeAnnotation("T"));
  variants.push_back({"None", 1, Position{}, {}});
  auto ast = std::make_unique<EnumDefinitionAST>(
      "Maybe", std::move(variants), /*precompiled=*/false,
      std::vector<TypeParameter>{
          TypeParameter("T", TypeConstraint("_Numeric"))});

  ASTSerializer serializer;
  std::string data = serializer.serializeToString(*ast);

  ASTDeserializer deserializer;
  auto restored = deserializer.deserializeFromString(data);

  ASSERT_NE(restored, nullptr);
  auto* enumDef = static_cast<EnumDefinitionAST*>(restored.get());
  ASSERT_EQ(enumDef->getTypeParameters().size(), 1u);
  EXPECT_EQ(enumDef->getTypeParameters()[0].name, "T");
  ASSERT_TRUE(enumDef->getTypeParameters()[0].constraint.has_value());
  EXPECT_EQ(enumDef->getTypeParameters()[0].constraint->name, "_Numeric");
}

// An unconstrained parameter round-trips as unconstrained, not as one
// carrying an empty constraint.
TEST(Tooling_Serialization, UnconstrainedTypeParameterStaysUnconstrained) {
  std::vector<EnumVariantDecl> variants;
  variants.push_back({"None", 0, Position{}, {}});
  auto ast = std::make_unique<EnumDefinitionAST>(
      "Plain", std::move(variants), /*precompiled=*/false,
      std::vector<TypeParameter>{TypeParameter("T")});

  ASTSerializer serializer;
  ASTDeserializer deserializer;
  auto restored =
      deserializer.deserializeFromString(serializer.serializeToString(*ast));

  ASSERT_NE(restored, nullptr);
  auto* enumDef = static_cast<EnumDefinitionAST*>(restored.get());
  ASSERT_EQ(enumDef->getTypeParameters().size(), 1u);
  EXPECT_FALSE(enumDef->getTypeParameters()[0].constraint.has_value());
}
