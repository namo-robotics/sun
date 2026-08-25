// text_positions.h — Convert between language-server positions and byte
// offsets. The protocol counts lines from zero and columns in UTF-16 code
// units; Sun source spans are byte offsets into the UTF-8 text.

#pragma once

#include <cstddef>
#include <string>

namespace sun::lsp {

struct LspPosition {
  int line = 0;
  int character = 0;
};

namespace detail {

// Number of bytes in the UTF-8 sequence that starts with this byte
inline size_t utf8Length(unsigned char lead) {
  if (lead < 0x80) return 1;
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;  // stray continuation byte
}

// UTF-16 code units for a sequence of this byte length
inline int utf16Units(size_t utf8Length) { return utf8Length == 4 ? 2 : 1; }

// Byte offset of the first character on a zero-based line (text.size() if
// the line does not exist)
inline size_t lineStart(const std::string& text, int line) {
  size_t offset = 0;
  for (int current = 0; current < line; ++current) {
    size_t newline = text.find('\n', offset);
    if (newline == std::string::npos) return text.size();
    offset = newline + 1;
  }
  return offset;
}

}  // namespace detail

// Byte offset for a protocol position. A column past the end of the line
// stops at the line break; a line past the end of the text stops at the end.
// A column inside a surrogate pair snaps to the start of that character.
inline int byteOffsetFromLspPosition(const std::string& text, int line,
                                     int character) {
  size_t offset = detail::lineStart(text, line);
  int units = 0;
  while (offset < text.size() && text[offset] != '\n') {
    size_t length =
        detail::utf8Length(static_cast<unsigned char>(text[offset]));
    int width = detail::utf16Units(length);
    if (units + width > character) break;
    units += width;
    offset += length;
  }
  return static_cast<int>(offset > text.size() ? text.size() : offset);
}

// Protocol position for a byte offset (clamped to the text)
inline LspPosition lspPositionFromByteOffset(const std::string& text,
                                             int offset) {
  size_t target = offset < 0 ? 0 : static_cast<size_t>(offset);
  if (target > text.size()) target = text.size();

  LspPosition position;
  size_t current = 0;
  while (current < target) {
    if (text[current] == '\n') {
      ++position.line;
      position.character = 0;
      ++current;
      continue;
    }
    size_t length =
        detail::utf8Length(static_cast<unsigned char>(text[current]));
    position.character += detail::utf16Units(length);
    current += length;
  }
  return position;
}

}  // namespace sun::lsp
