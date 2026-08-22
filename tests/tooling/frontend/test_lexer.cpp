// tests/tooling/frontend/test_lexer.cpp
//
// Direct tests of the tokenizer: byte handling, longest-match/priority rules,
// and position tracking across multi-line tokens.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "parsing/lexer.h"
#include "parsing/parser.h"

namespace {

// Lex a source string to EOF and return every token (comments included).
std::vector<Token> lexAll(const std::string& source, bool emitComments = true) {
  std::istringstream ss(source);
  Lexer lexer(ss);
  lexer.setEmitComments(emitComments);

  std::vector<Token> tokens;
  while (true) {
    Token tok = lexer.getNextToken();
    bool eof = tok.isEof();
    tokens.push_back(std::move(tok));
    if (eof) break;
  }
  return tokens;
}

std::vector<TokenKind> kindsOf(const std::string& source,
                               bool emitComments = false) {
  std::vector<TokenKind> kinds;
  for (const Token& tok : lexAll(source, emitComments)) {
    if (tok.isEof()) break;
    kinds.push_back(tok.kind);
  }
  return kinds;
}

}  // namespace

// ------------------------------------------------------------------
// Reserved words
// ------------------------------------------------------------------

TEST(Tooling_Frontend_Lexer, KeywordSpellingCoversWordTokensOnly) {
  EXPECT_EQ(getKeywordSpelling(TokenKind::PARTIAL), "partial");
  EXPECT_EQ(getKeywordSpelling(TokenKind::PACKED_CLASS), "packed_class");
  EXPECT_EQ(getKeywordSpelling(TokenKind::TYPE_I32), "i32");
  EXPECT_EQ(getKeywordSpelling(TokenKind::AND), "and");
  EXPECT_EQ(getKeywordSpelling(TokenKind::TRUE_LITERAL), "true");
  EXPECT_FALSE(isKeyword(TokenKind::IDENTIFIER));
  EXPECT_FALSE(isKeyword(TokenKind::INTRINSIC_IDENTIFIER));
  EXPECT_FALSE(isKeyword(TokenKind::UNDERSCORE));
  EXPECT_FALSE(isKeyword(TokenKind::PLUS));
  EXPECT_FALSE(isKeyword(TokenKind::ARROW));
  EXPECT_FALSE(isKeyword(TokenKind::STRING));
  EXPECT_FALSE(isKeyword(TokenKind::TOK_EOF));
}

// Every word the table calls a keyword must lex as that keyword, and every
// token the lexer produces for a bare word must be in the table. The docs'
// reserved-word list (docs/pages/overview.mdx) mirrors this set.
TEST(Tooling_Frontend_Lexer, EveryKeywordLexesAsItself) {
  int count = 0;
  for (int i = 0; i < static_cast<int>(TokenKind::COUNT); ++i) {
    TokenKind kind = static_cast<TokenKind>(i);
    auto word = getKeywordSpelling(kind);
    if (!word) continue;
    ++count;
    auto kinds = kindsOf(std::string(*word));
    ASSERT_EQ(kinds.size(), 1u) << *word;
    EXPECT_EQ(kinds[0], kind) << *word;
    // A keyword token carries its spelling as text.
    EXPECT_EQ(lexAll(std::string(*word))[0].text, *word);
  }
  EXPECT_EQ(count, 54);
}

// ------------------------------------------------------------------
// Non-ASCII bytes
// ------------------------------------------------------------------

TEST(Tooling_Frontend_Lexer, Utf8InStringLiteral) {
  auto tokens = lexAll("\"h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C\"");
  ASSERT_EQ(tokens.size(), 2u);
  EXPECT_EQ(tokens[0].kind, TokenKind::STRING);
  EXPECT_EQ(tokens[0].getString().value(), "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C");
}

TEST(Tooling_Frontend_Lexer, Utf8InComments) {
  auto tokens = lexAll("// \xC3\xBCn\xC3\xAF" "code\n/* \xCF\x80 */ 1");
  ASSERT_EQ(tokens.size(), 4u);
  EXPECT_EQ(tokens[0].kind, TokenKind::COMMENT);
  EXPECT_EQ(tokens[0].text, "// \xC3\xBCn\xC3\xAF" "code");
  EXPECT_EQ(tokens[1].kind, TokenKind::BLOCK_COMMENT);
  EXPECT_EQ(tokens[1].text, "/* \xCF\x80 */");
  EXPECT_EQ(tokens[2].kind, TokenKind::INTEGER);
}

TEST(Tooling_Frontend_Lexer, Utf8InTemplateString) {
  auto tokens = lexAll("`caf\xC3\xA9`");
  ASSERT_EQ(tokens.size(), 2u);
  EXPECT_EQ(tokens[0].kind, TokenKind::TEMPLATE_STRING);
  EXPECT_EQ(tokens[0].getTemplateString().value(), "caf\xC3\xA9");
}

// Columns count BYTES, not codepoints -- both before and after the byte-range
// widening. Pinned so nobody "fixes" it without meaning to.
TEST(Tooling_Frontend_Lexer, ColumnsCountBytes) {
  auto tokens = lexAll("\"\xC3\xA9\" x");
  ASSERT_GE(tokens.size(), 2u);
  EXPECT_EQ(tokens[0].kind, TokenKind::STRING);
  EXPECT_EQ(tokens[0].start.column, 1);
  EXPECT_EQ(tokens[0].end.column, 5);  // quote + 2 bytes + quote
  EXPECT_EQ(tokens[1].kind, TokenKind::IDENTIFIER);
  EXPECT_EQ(tokens[1].start.column, 6);
}

// ------------------------------------------------------------------
// Longest match, ties broken by declaration order
// ------------------------------------------------------------------

TEST(Tooling_Frontend_Lexer, LongestMatchWins) {
  EXPECT_EQ(kindsOf(">>="),
            std::vector<TokenKind>{TokenKind::RIGHT_SHIFT_ASSIGN});
  EXPECT_EQ(kindsOf(">>"), std::vector<TokenKind>{TokenKind::RIGHT_SHIFT});
  EXPECT_EQ(kindsOf(">="), std::vector<TokenKind>{TokenKind::GREATER_EQUAL});
  EXPECT_EQ(kindsOf(">"), std::vector<TokenKind>{TokenKind::GREATER});
  EXPECT_EQ(kindsOf("<<="), std::vector<TokenKind>{TokenKind::LEFT_SHIFT_ASSIGN});
  EXPECT_EQ(kindsOf("..."), std::vector<TokenKind>{TokenKind::ELLIPSIS});
  EXPECT_EQ(kindsOf("::"), std::vector<TokenKind>{TokenKind::DOUBLE_COLON});
  EXPECT_EQ(kindsOf("=>"), std::vector<TokenKind>{TokenKind::FAT_ARROW});
}

TEST(Tooling_Frontend_Lexer, FloatBeatsIntegerFollowedByDot) {
  EXPECT_EQ(kindsOf("3.0"), std::vector<TokenKind>{TokenKind::FLOAT});
  EXPECT_EQ(kindsOf("3"), std::vector<TokenKind>{TokenKind::INTEGER});
  // "3." is INTEGER then DOT: FLOAT needs a digit after the point
  EXPECT_EQ(kindsOf("3."),
            (std::vector<TokenKind>{TokenKind::INTEGER, TokenKind::DOT}));
}

TEST(Tooling_Frontend_Lexer, KeywordVsIdentifierPriority) {
  EXPECT_EQ(kindsOf("static_ptr"), std::vector<TokenKind>{TokenKind::STATIC_PTR});
  EXPECT_EQ(kindsOf("raw_ptr"), std::vector<TokenKind>{TokenKind::RAW_PTR});
  EXPECT_EQ(kindsOf("ptr"), std::vector<TokenKind>{TokenKind::PTR});
  EXPECT_EQ(kindsOf("i32"), std::vector<TokenKind>{TokenKind::TYPE_I32});
  // A keyword prefix of a longer word is not a keyword -- longest match first
  EXPECT_EQ(kindsOf("ifx"), std::vector<TokenKind>{TokenKind::IDENTIFIER});
  EXPECT_EQ(kindsOf("returned"), std::vector<TokenKind>{TokenKind::IDENTIFIER});
  EXPECT_EQ(kindsOf("i320"), std::vector<TokenKind>{TokenKind::IDENTIFIER});
}

TEST(Tooling_Frontend_Lexer, UnderscoreVsIntrinsicIdentifier) {
  EXPECT_EQ(kindsOf("_"), std::vector<TokenKind>{TokenKind::UNDERSCORE});
  EXPECT_EQ(kindsOf("_foo"),
            std::vector<TokenKind>{TokenKind::INTRINSIC_IDENTIFIER});
}

// ------------------------------------------------------------------
// Positions across multi-line tokens
// ------------------------------------------------------------------

TEST(Tooling_Frontend_Lexer, PositionsAcrossMultiLineBlockComment) {
  auto tokens = lexAll("x\n/* line1\nline2\nline3 */ y");
  ASSERT_EQ(tokens.size(), 4u);

  EXPECT_EQ(tokens[0].kind, TokenKind::IDENTIFIER);
  EXPECT_EQ(tokens[0].start.line, 1);

  // The comment spans lines 2-4; rewinding to the accept must land on line 4
  EXPECT_EQ(tokens[1].kind, TokenKind::BLOCK_COMMENT);
  EXPECT_EQ(tokens[1].start.line, 2);
  EXPECT_EQ(tokens[1].start.column, 1);
  EXPECT_EQ(tokens[1].end.line, 4);
  EXPECT_EQ(tokens[1].end.column, 9);  // "line3 */" is 8 bytes

  EXPECT_EQ(tokens[2].kind, TokenKind::IDENTIFIER);
  EXPECT_EQ(tokens[2].start.line, 4);
  EXPECT_EQ(tokens[2].start.column, 10);
}

TEST(Tooling_Frontend_Lexer, PositionsAfterLineComment) {
  auto tokens = lexAll("// c\nx", /*emitComments=*/false);
  ASSERT_EQ(tokens.size(), 2u);
  EXPECT_EQ(tokens[0].kind, TokenKind::IDENTIFIER);
  EXPECT_EQ(tokens[0].start.line, 2);
  EXPECT_EQ(tokens[0].start.column, 1);
  EXPECT_EQ(tokens[0].start.offset, 5);
}

TEST(Tooling_Frontend_Lexer, EofPositionAfterTrailingWhitespace) {
  auto tokens = lexAll("abc   \n");
  ASSERT_EQ(tokens.size(), 2u);
  EXPECT_TRUE(tokens[1].isEof());
  EXPECT_EQ(tokens[1].start.line, 2);
  EXPECT_EQ(tokens[1].start.column, 1);
  EXPECT_EQ(tokens[1].start.offset, 7);
}

// ------------------------------------------------------------------
// Re-lexing from an arbitrary offset (parser backtracking)
// ------------------------------------------------------------------

TEST(Tooling_Frontend_Lexer, RelexFromSavedPosition) {
  std::istringstream ss("var x = a < b;");
  Lexer lexer(ss);

  lexer.getNextToken();  // var
  Position saved = lexer.getPosition();
  std::vector<TokenKind> first;
  for (int i = 0; i < 5; ++i) first.push_back(lexer.getNextToken().kind);

  lexer.setPosition(saved);
  std::vector<TokenKind> second;
  for (int i = 0; i < 5; ++i) second.push_back(lexer.getNextToken().kind);

  EXPECT_EQ(first, second);
}

// The old scan loop broke out early once the stream's eofbit was set, so
// re-lexing after the input was exhausted produced no match at all.
TEST(Tooling_Frontend_Lexer, RelexAfterInputExhausted) {
  std::istringstream ss("a < b");
  Lexer lexer(ss);

  std::vector<TokenKind> kinds;
  Position afterFirst;
  Token t = lexer.getNextToken();
  afterFirst = lexer.getPosition();
  while (!t.isEof()) t = lexer.getNextToken();  // drain to EOF, setting eofbit

  lexer.setPosition(afterFirst);
  EXPECT_EQ(lexer.getNextToken().kind, TokenKind::LESS);
  EXPECT_EQ(lexer.getNextToken().kind, TokenKind::IDENTIFIER);
  EXPECT_TRUE(lexer.getNextToken().isEof());
}

// The generic-vs-comparison backtrack with nothing following it
TEST(Tooling_Frontend_Lexer, ParsesComparisonAtEndOfInput) {
  const std::string source = "function f(a: i32, b: i32) bool { return a < b; }";
  std::istringstream ss(source);
  Parser parser(ss);
  EXPECT_NO_THROW({ auto ast = parser.parseString(source); });
}

// ------------------------------------------------------------------
// Shared DFA cache
// ------------------------------------------------------------------

TEST(Tooling_Frontend_Lexer, SharedDfaCacheStopsGrowing) {
  const std::string source =
      "function main() i32 {\n"
      "  var total = 0;\n"
      "  for (var i = 0; i < 10; i = i + 1) { total += i * 2; }\n"
      "  var s = \"text\"; // trailing\n"
      "  return total;\n"
      "}\n";

  lexAll(source);
  const int states = Lexer::getTokenDFA().stateCount();
  const long long misses = Lexer::getTokenDFA().transitionMisses();

  lexAll(source);
  EXPECT_EQ(Lexer::getTokenDFA().stateCount(), states)
      << "re-lexing the same source must not create DFA states";
  EXPECT_EQ(Lexer::getTokenDFA().transitionMisses(), misses)
      << "re-lexing the same source must not miss the transition cache";
}

TEST(Tooling_Frontend_Lexer, DfaStateCountStaysBounded) {
  // Lex a broad mix of constructs; the state count is bounded by the token
  // grammar, not by the input.
  lexAll(
      "class C implements I { var a: array<i32>; function f() void, IError {} }"
      " match x { 1 => y, _ => z } lambda (a) => a >>= 2 & 3 | 4 ^ 5 ~ 6;"
      " `tmpl ${x}` \"str\" 1.5e-3 0 _i /* c */ // c\n"
      " static_ptr<u8> raw_ptr<f64> ... :: -> => != <= >= %= /= *=");
  EXPECT_LT(Lexer::getTokenDFA().stateCount(), 4000);
}

// ------------------------------------------------------------------
// Throughput (disabled: reporting only, no assertions)
// Run with --gtest_also_run_disabled_tests --gtest_filter=LexerTest.DISABLED_*
// ------------------------------------------------------------------

TEST(Tooling_Frontend_Lexer, DISABLED_ThroughputMBps) {
  std::string unit =
      "function compute(a: i32, b: i32) i32 {\n"
      "  var total = 0;  // accumulate\n"
      "  for (var i = 0; i < 100; i = i + 1) {\n"
      "    total += (a * i) - (b / 2) + 3.5e2;\n"
      "  }\n"
      "  var msg = \"result: \";\n"
      "  return total >> 1;\n"
      "}\n";

  std::string source;
  while (source.size() < 2u * 1024 * 1024) source += unit;

  const int kRuns = 5;
  auto begin = std::chrono::steady_clock::now();
  size_t tokens = 0;
  for (int r = 0; r < kRuns; ++r) {
    std::istringstream ss(source);
    Lexer lexer(ss);
    while (!lexer.getNextToken().isEof()) ++tokens;
  }
  auto elapsed = std::chrono::duration<double>(
                     std::chrono::steady_clock::now() - begin)
                     .count();

  const double bytes = static_cast<double>(source.size()) * kRuns;
  std::printf(
      "\n  lexed %.2f MB in %.3f s  =>  %.2f MB/s  (%zu tokens)\n"
      "  DFA states=%d  transition misses=%lld  table=%.0f KiB\n\n",
      bytes / (1024 * 1024), elapsed, bytes / (1024 * 1024) / elapsed, tokens,
      Lexer::getTokenDFA().stateCount(),
      Lexer::getTokenDFA().transitionMisses(),
      Lexer::getTokenDFA().stateCount() * 256 * 4 / 1024.0);
}

TEST(Tooling_Frontend_Lexer, DISABLED_ConstructionCost) {
  std::istringstream ss("x");
  auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000; ++i) {
    Lexer lexer(ss);
    (void)lexer;
  }
  auto elapsed = std::chrono::duration<double>(
                     std::chrono::steady_clock::now() - begin)
                     .count();
  std::printf("\n  1000 Lexer constructions in %.6f s (%.3f us each)\n\n",
              elapsed, elapsed * 1e6 / 1000);
}
