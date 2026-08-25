// tests/tooling/lsp/test_text_positions.cpp — Protocol position conversion
//
// The language server protocol counts columns in UTF-16 code units; Sun
// spans are byte offsets. These tests pin the conversion in both directions.

#include <gtest/gtest.h>

#include <string>

#include "lsp/text_positions.h"

using sun::lsp::byteOffsetFromLspPosition;
using sun::lsp::lspPositionFromByteOffset;

TEST(Tooling_Lsp_TextPositions, AsciiSingleLine) {
  std::string text = "var x = 1;";
  EXPECT_EQ(byteOffsetFromLspPosition(text, 0, 0), 0);
  EXPECT_EQ(byteOffsetFromLspPosition(text, 0, 4), 4);
  auto position = lspPositionFromByteOffset(text, 4);
  EXPECT_EQ(position.line, 0);
  EXPECT_EQ(position.character, 4);
}

TEST(Tooling_Lsp_TextPositions, MultipleLines) {
  std::string text = "a\nbb\nccc";
  EXPECT_EQ(byteOffsetFromLspPosition(text, 1, 1), 3);
  EXPECT_EQ(byteOffsetFromLspPosition(text, 2, 2), 7);
  auto position = lspPositionFromByteOffset(text, 7);
  EXPECT_EQ(position.line, 2);
  EXPECT_EQ(position.character, 2);
}

TEST(Tooling_Lsp_TextPositions, TwoByteCharacterIsOneUnit) {
  // "é" is two bytes in UTF-8 and one UTF-16 unit
  std::string text = "var \xC3\xA9 = x;";
  EXPECT_EQ(byteOffsetFromLspPosition(text, 0, 7), 8);
  auto position = lspPositionFromByteOffset(text, 8);
  EXPECT_EQ(position.character, 7);
}

TEST(Tooling_Lsp_TextPositions, FourByteCharacterIsTwoUnits) {
  // "😀" is four bytes in UTF-8 and a surrogate pair in UTF-16
  std::string text = "\"\xF0\x9F\x98\x80\" x";
  // 1 (quote) + 2 (emoji) + 1 (quote) + 1 (space) = 5 units -> byte 7
  EXPECT_EQ(byteOffsetFromLspPosition(text, 0, 5), 7);
  auto position = lspPositionFromByteOffset(text, 7);
  EXPECT_EQ(position.character, 5);
  // A column inside the surrogate pair snaps to the character's start
  EXPECT_EQ(byteOffsetFromLspPosition(text, 0, 2), 1);
}

TEST(Tooling_Lsp_TextPositions, ClampsPastLineAndTextEnd) {
  std::string text = "ab\ncd";
  EXPECT_EQ(byteOffsetFromLspPosition(text, 0, 10), 2);  // stops at '\n'
  EXPECT_EQ(byteOffsetFromLspPosition(text, 1, 10), 5);  // end of text
  EXPECT_EQ(byteOffsetFromLspPosition(text, 5, 0), 5);   // line past end
  auto position = lspPositionFromByteOffset(text, 50);
  EXPECT_EQ(position.line, 1);
  EXPECT_EQ(position.character, 2);
}

TEST(Tooling_Lsp_TextPositions, RoundTrip) {
  std::string text = "x\n\xC3\xA9\xF0\x9F\x98\x80 y\nz";
  for (int offset = 0; offset <= static_cast<int>(text.size()); ++offset) {
    // Skip offsets inside a multi-byte character
    unsigned char byte = offset < static_cast<int>(text.size())
                             ? static_cast<unsigned char>(text[offset])
                             : 0;
    if ((byte & 0xC0) == 0x80) continue;
    auto position = lspPositionFromByteOffset(text, offset);
    EXPECT_EQ(
        byteOffsetFromLspPosition(text, position.line, position.character),
        offset)
        << "offset " << offset;
  }
}
