// tests/test_parser.cpp

#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <string>

#include "nfa.h"

// ------------------------------------------------------------------
TEST(RegexParserTest, Integers)
{
    RegexParser parser;
    NFA nfa = parser.parse("(0|1|2|3|4|5|6|7)*"); // Regex: a followed by zero or more b|c, then d
    ASSERT_TRUE(nfa.matches("0"));
    ASSERT_TRUE(nfa.matches("12321"));
}

TEST(RegexParserTest, CharClass)
{
    RegexParser parser;
    NFA nfa = parser.parse("[a-c]+"); // Regex: one or more of a, b, or c
    ASSERT_TRUE(nfa.matches("a"));
    ASSERT_TRUE(nfa.matches("abcabc"));
    ASSERT_FALSE(nfa.matches("abcd"));
    ASSERT_FALSE(nfa.matches("xyz"));
    ASSERT_FALSE(nfa.matches(""));

    nfa = parser.parse("[a-c]*"); // Regex: one or more of a, b, or c
    ASSERT_TRUE(nfa.matches("abc"));
    ASSERT_TRUE(nfa.matches(""));

    nfa = parser.parse("[a-cD-F]+"); // Regex: one or more of a, b, or c
    ASSERT_TRUE(nfa.matches("a"));
    ASSERT_TRUE(nfa.matches("D"));
    ASSERT_FALSE(nfa.matches("G"));
    ASSERT_FALSE(nfa.matches("0"));

    nfa = parser.parse("[a-c]+[D-F\\(]+");
    ASSERT_FALSE(nfa.matches("a"));
    ASSERT_TRUE(nfa.matches("abcDEF("));
    ASSERT_FALSE(nfa.matches("DFGabc"));
}

TEST(RegexParserTest, WildCard)
{
    RegexParser parser;
    NFA nfa = parser.parse("a.b"); // Regex: one or more of a, b, or c
    ASSERT_TRUE(nfa.matches("acb"));
    ASSERT_FALSE(nfa.matches("abc"));
    ASSERT_FALSE(nfa.matches("ab"));
}

TEST(RegexParserTest, WildCardAndKleeneStar)
{
    RegexParser parser;
    NFA nfa = parser.parse("a.*b"); // Regex: one or more of a, b, or c
    ASSERT_FALSE(nfa.matches("b"));
    ASSERT_TRUE(nfa.matches("ab"));
    ASSERT_TRUE(nfa.matches("acccccccccccccb"));
    ASSERT_FALSE(nfa.matches("abc"));

    nfa = parser.parse("(a.)*b"); // Regex: one or more of a, b, or c

    ASSERT_TRUE(nfa.matches("b"));
    ASSERT_FALSE(nfa.matches("aaab"));
    ASSERT_TRUE(nfa.matches("aaaab"));
    ASSERT_TRUE(nfa.matches("a_a_a_a_b"));
    ASSERT_FALSE(nfa.matches("a_a_a_a_"));
    ASSERT_FALSE(nfa.matches("abc"));
}

TEST(RegexParserTest, Unions)
{
    RegexParser parser;
    NFA nfa = parser.parse("alice|bob");
    ASSERT_TRUE(nfa.matches("alice"));
    ASSERT_TRUE(nfa.matches("bob"));
    ASSERT_FALSE(nfa.matches("alic"));
    ASSERT_FALSE(nfa.matches("bobby"));
    ASSERT_FALSE(nfa.matches("alicealice"));

    nfa = parser.parse("(alice)+|(bob)");
    ASSERT_TRUE(nfa.matches("alicealice"));
    ASSERT_TRUE(nfa.matches("bob"));
    ASSERT_FALSE(nfa.matches("alicebob"));
    ASSERT_FALSE(nfa.matches("bobby"));
}

TEST(RegexParserTest, NumberGroup)
{
    RegexParser parser;
    NFA nfa = parser.parse("(?<1>def)|(?<2>extern)|(?<3>[a-zA-Z][a-zA-Z0-9]*)|(?<4>if)|(?<5>then)|(?<6>else)|(?<7>[0-9]+)|(?<8>\\+)|(?<9>-)|(?<10>\\*)|(?<11>/)|(?<12><)|(?<13><=)|(?<14>>)|(?<15>>=)|(?<16>=)|(?<17>==)|(?<18>!=)|(?<19>\\()|(?<20>\\))|(?<21>,)|(?<22>for)|(?<23>in)");
    auto step = nfa.step('4');
    ASSERT_EQ(step.captures.size(), 1);
    step = nfa.step('2');
    ASSERT_TRUE(step.isAccepting);
    ASSERT_EQ(step.captures.size(), 1);
    ASSERT_EQ(step.captures.at(6).at(0).text, "4");
    ASSERT_EQ(step.captures.at(6).back().text, "42");
}

TEST(RegexParserTest, CanReachAcceptingWithNonEmptyInput)
{
    RegexParser parser;
    NFA nfa = parser.parse("[1-9]*");
    //SERT_TRUE(nfa.canReachAcceptingWithNonEmptyInput());
    auto step = nfa.step('4');
    ASSERT_TRUE(nfa.isAccepting);
    // ASSERT_TRUE(nfa.canReachAcceptingWithNonEmptyInput());
    // step = nfa.step('2');
    // ASSERT_TRUE(nfa.canReachAcceptingWithNonEmptyInput());
    // step = nfa.step('a');
    // ASSERT_FALSE(nfa.canReachAcceptingWithNonEmptyInput());

    // nfa.reset();
    // ASSERT_TRUE(nfa.canReachAcceptingWithNonEmptyInput());
    // step = nfa.step(' ');
    // ASSERT_FALSE(nfa.canReachAcceptingWithNonEmptyInput());
}

TEST(RegexParserTest, CaptureNumber)
{
    RegexParser parser;
    NFA nfa = parser.parse("([0-9]+)");

    auto step = nfa.step('1');
    ASSERT_TRUE(step.isAccepting);
    ASSERT_EQ(step.captures.size(), 1);

    step = nfa.step('2');
    ASSERT_TRUE(step.isAccepting);
    ASSERT_EQ(step.captures.size(), 1);
}

TEST(RegexParserTest, CaptureGroups)
{
    RegexParser parser;
    NFA nfa = parser.parse(".*(a)|.*(b)");

    auto step = nfa.step('a');
    ASSERT_TRUE(step.isAccepting);
    ASSERT_EQ(step.captures.size(), 1);

    auto step2 = nfa.step('b');
    ASSERT_TRUE(step2.isAccepting);
    ASSERT_EQ(step2.captures.size(), 2);
    ASSERT_EQ(step2.captures.at(1).size(), 1);
    ASSERT_EQ(step2.captures.at(1)[0].groupIdx, 1);
    ASSERT_EQ(step2.captures.at(1)[0].start.offset, 1);
    ASSERT_EQ(step2.captures.at(1)[0].end.offset, 2);
}



TEST(RegexParserTest, NumberRegex)
{
    RegexParser parser;
    NFA nfa = parser.parse("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?");

    ASSERT_TRUE(nfa.matches("0"));
    ASSERT_TRUE(nfa.matches("-0"));
    ASSERT_TRUE(nfa.matches("123"));
    ASSERT_TRUE(nfa.matches("-123"));
    ASSERT_TRUE(nfa.matches("3.14"));
    ASSERT_TRUE(nfa.matches("-3.14"));
    ASSERT_TRUE(nfa.matches("2e10"));
    ASSERT_TRUE(nfa.matches("-2E-10"));
    ASSERT_TRUE(nfa.matches("0.001e+5"));

    ASSERT_FALSE(nfa.matches(""));
    ASSERT_FALSE(nfa.matches("abc"));
    ASSERT_FALSE(nfa.matches("1."));
    ASSERT_FALSE(nfa.matches(".5"));
    ASSERT_FALSE(nfa.matches("1e"));
    ASSERT_FALSE(nfa.matches("1e+"));
}

TEST(RegexParserTest, WhiteSpace)
{
    RegexParser parser;
    NFA nfa = parser.parse("[ \n\t\r]*(a)");

    ASSERT_TRUE(nfa.matches("a"));
    ASSERT_TRUE(nfa.matches(" a"));
    ASSERT_TRUE(nfa.matches("             a"));
    ASSERT_TRUE(nfa.matches("             \n         \r \t     a"));

    ASSERT_FALSE(nfa.matches(""));
    ASSERT_FALSE(nfa.matches("b"));
}

TEST(RegexParserTest, FullRegex)
{
    RegexParser parser;
    NFA nfa = parser.parse("[ \n\t\r]*((?<1>def)|(?<2>extern)|(?<3>[a-zA-Z][a-zA-Z0-9]*)|(?<4>if)|(?<5>then)|(?<6>else)|(?<7>-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?)|(?<8>\\+)|(?<9>-)|(?<10>\\*)|(?<11>/)|(?<12><)|(?<13><=)|(?<14>>)|(?<15>>=)|(?<16>=)|(?<17>==)|(?<18>!=)|(?<19>\\()|(?<20>\\))|(?<21>,)|(?<22>for)|(?<23>in))");

    ASSERT_TRUE(nfa.matches(" def"));
    ASSERT_TRUE(nfa.matches("a"));
}

