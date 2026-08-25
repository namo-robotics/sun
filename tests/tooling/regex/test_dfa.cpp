// tests/tooling/regex/test_dfa.cpp
//
// Tests for the lazily-determinized DFA: match semantics across the regex
// features the lexer relies on, capture tracking, and the transition cache.
//
// The match/capture expectations here were validated against the NFA
// simulation the DFA replaced (a step-for-step differential over every one of
// these regex/input pairs, including all capture slots of the master regex)
// before that simulation was removed.

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "parsing/lexer.h"  // for Lexer::getStaticFullRegex()
#include "parsing/nfa.h"

namespace {

struct MatchCase {
  std::string input;
  bool expected;
};

void expectMatches(const std::string& regex,
                   const std::vector<MatchCase>& cases) {
  RegexParser parser;
  DFA dfa = parser.parse(regex);
  for (const MatchCase& c : cases) {
    EXPECT_EQ(dfa.matches(c.input), c.expected)
        << "regex=" << regex << " input=" << c.input;
  }
}

}  // namespace

// ------------------------------------------------------------------
// Match semantics
// ------------------------------------------------------------------

TEST(Tooling_Regex_Dfa, AlternationAndRepetition) {
  expectMatches("(0|1|2|3|4|5|6|7)*", {{"", true},
                                       {"0", true},
                                       {"12321", true},
                                       {"9", false},
                                       {"1239", false}});
  expectMatches("alice|bob", {{"alice", true},
                              {"bob", true},
                              {"alic", false},
                              {"bobby", false},
                              {"alicealice", false}});
  expectMatches("(alice)+|(bob)", {{"alicealice", true},
                                   {"bob", true},
                                   {"alicebob", false},
                                   {"bobby", false}});
}

TEST(Tooling_Regex_Dfa, CharacterClasses) {
  expectMatches("[a-c]+", {{"a", true},
                           {"abcabc", true},
                           {"abcd", false},
                           {"xyz", false},
                           {"", false}});
  expectMatches("[a-c]*", {{"abc", true}, {"", true}, {"d", false}});
  expectMatches(
      "[a-cD-F]+",
      {{"a", true}, {"D", true}, {"aDbEcF", true}, {"G", false}, {"0", false}});
  expectMatches("[a-c]+[D-F\\(]+",
                {{"abcDEF(", true}, {"a", false}, {"DFGabc", false}});
}

TEST(Tooling_Regex_Dfa, WildcardMatchesEveryByte) {
  expectMatches("a.b", {{"acb", true},
                        {"a\nb", true},
                        {"a\xFF"
                         "b",
                         true},
                        {"abc", false},
                        {"ab", false}});
  expectMatches(
      "a.*b",
      {{"ab", true}, {"acccccccccccccb", true}, {"b", false}, {"abc", false}});
  expectMatches("(a.)*b", {{"b", true},
                           {"aaaab", true},
                           {"a_a_a_a_b", true},
                           {"aaab", false},
                           {"a_a_a_a_", false},
                           {"abc", false}});
}

TEST(Tooling_Regex_Dfa, NegatedClassesCoverHighBytes) {
  expectMatches("[^a]+", {{"b", true},
                          {"\x80", true},
                          {"\xC3\xA9", true},
                          {"\xFF", true},
                          {"a", false},
                          {"bba", false}});
}

TEST(Tooling_Regex_Dfa, TokenPatterns) {
  // The STRING token pattern
  expectMatches("\"([^\"\\\\]|\\\\.)*\"", {{"\"\"", true},
                                           {"\"abc\"", true},
                                           {"\"a\\\"b\"", true},
                                           {"\"h\xC3\xA9llo\"", true},
                                           {"\"unterminated", false}});
  // The BLOCK_COMMENT token pattern -- must not run past the first */
  expectMatches("/\\*[^*]*\\*+([^/*][^*]*\\*+)*/", {{"/**/", true},
                                                    {"/* x */", true},
                                                    {"/* a * b */", true},
                                                    {"/* a\nb */", true},
                                                    {"/* unterminated", false},
                                                    {"/* a */ b */", false}});
  // The FLOAT/number pattern
  expectMatches("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?",
                {{"0", true},
                 {"-0", true},
                 {"123", true},
                 {"3.14", true},
                 {"-2E-10", true},
                 {"0.001e+5", true},
                 {"", false},
                 {"abc", false},
                 {"1.", false},
                 {".5", false},
                 {"1e", false},
                 {"1e+", false}});
}

// ------------------------------------------------------------------
// Captures survive determinization
// ------------------------------------------------------------------

TEST(Tooling_Regex_Dfa, CaptureOffsetsOnMasterRegex) {
  DFA& dfa = Lexer::getTokenDFA();

  // "  function" -- the leading whitespace is outside every named group, so
  // the winning capture starts at the first non-space byte.
  dfa.fullReset();
  for (char c : std::string("  function")) dfa.step(c);

  const RegexCapture* best = dfa.bestCapture();
  ASSERT_NE(best, nullptr);
  EXPECT_EQ(best->groupNameNum, static_cast<int>(TokenKind::FUNCTION));
  EXPECT_EQ(best->start.offset, 2);
  EXPECT_EQ(best->end.offset, 10);
  EXPECT_EQ(best->length(), 8);
}

TEST(Tooling_Regex_Dfa, LongestMatchWinsOverShorterAlternative) {
  DFA& dfa = Lexer::getTokenDFA();

  // "static_ptr" is also a legal IDENTIFIER prefix; the longer keyword wins.
  dfa.fullReset();
  for (char c : std::string("static_ptr")) dfa.step(c);
  ASSERT_NE(dfa.bestCapture(), nullptr);
  EXPECT_EQ(dfa.bestCapture()->groupNameNum,
            static_cast<int>(TokenKind::STATIC_PTR));

  // One byte further and IDENTIFIER is the only (and longer) match.
  dfa.fullReset();
  for (char c : std::string("static_ptrs")) dfa.step(c);
  ASSERT_NE(dfa.bestCapture(), nullptr);
  EXPECT_EQ(dfa.bestCapture()->groupNameNum,
            static_cast<int>(TokenKind::IDENTIFIER));
}

TEST(Tooling_Regex_Dfa, CaptureTracksLongestPerGroup) {
  RegexParser parser;
  DFA dfa = parser.parse("([0-9]+)");

  ASSERT_TRUE(dfa.step('1'));
  ASSERT_NE(dfa.captureFor(0), nullptr);
  EXPECT_EQ(dfa.captureFor(0)->length(), 1);

  ASSERT_TRUE(dfa.step('2'));
  ASSERT_NE(dfa.captureFor(0), nullptr);
  EXPECT_EQ(dfa.captureFor(0)->length(), 2);
}

TEST(Tooling_Regex_Dfa, ResetInvalidatesCaptures) {
  RegexParser parser;
  DFA dfa = parser.parse("([0-9]+)");

  ASSERT_TRUE(dfa.step('1'));
  ASSERT_NE(dfa.captureFor(0), nullptr);

  dfa.fullReset();
  EXPECT_EQ(dfa.captureFor(0), nullptr);
  EXPECT_EQ(dfa.bestCapture(), nullptr);
}

// ------------------------------------------------------------------
// Structure and cache behaviour
// ------------------------------------------------------------------

TEST(Tooling_Regex_Dfa, TransitionCacheIsReused) {
  RegexParser parser;
  DFA dfa = parser.parse(Lexer::getStaticFullRegex());

  const std::string source =
      "function main() i32 { var x = 1 + 2 * 3; return x; }";

  dfa.matches(source);
  const int statesAfterFirst = dfa.stateCount();
  const long long missesAfterFirst = dfa.transitionMisses();
  EXPECT_GT(missesAfterFirst, 0);

  dfa.matches(source);
  EXPECT_EQ(dfa.stateCount(), statesAfterFirst) << "no new states on replay";
  EXPECT_EQ(dfa.transitionMisses(), missesAfterFirst)
      << "every transition should have been cached by the first pass";
}

TEST(Tooling_Regex_Dfa, StartStateIsNotAccepting) {
  // No token alternative matches the empty string, so a scan can never
  // produce a zero-length match.
  DFA& dfa = Lexer::getTokenDFA();
  EXPECT_EQ(dfa.acceptKind(dfa.startState()), DFA::kNoAccept);
}

TEST(Tooling_Regex_Dfa, DeadStateSelfLoops) {
  RegexParser parser;
  DFA dfa = parser.parse("abc");
  int32_t s = dfa.startState();
  s = dfa.step(s, 'x');
  ASSERT_TRUE(DFA::isDead(s));
  EXPECT_TRUE(DFA::isDead(dfa.step(s, 'a')));
  EXPECT_TRUE(DFA::isDead(dfa.step(s, 'b')));
}

TEST(Tooling_Regex_Dfa, AcceptKindIsLowestTokenKindOnTies) {
  // "i32" is both TYPE_I32 and a legal IDENTIFIER at the same length; the
  // lower TokenKind index (declaration order) wins.
  DFA& dfa = Lexer::getTokenDFA();
  int32_t s = dfa.startState();
  for (char c : std::string("i32"))
    s = dfa.step(s, static_cast<unsigned char>(c));
  ASSERT_FALSE(DFA::isDead(s));
  EXPECT_EQ(dfa.acceptKind(s), static_cast<int>(TokenKind::TYPE_I32));
  EXPECT_LT(static_cast<int>(TokenKind::TYPE_I32),
            static_cast<int>(TokenKind::IDENTIFIER));
}
