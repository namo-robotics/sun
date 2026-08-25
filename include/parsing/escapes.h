// escapes.h — Escape sequences shared by every literal form
//
// String literals ("..."), template strings (`...`), character literals ('a')
// and byte literals (b'a') all understand the same core escapes. Only the
// quote character differs, so the core table lives here and each caller adds
// its own quote on top.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace sun::escapes {

// The escapes every literal form shares: \n \t \r \\ \0.
// Returns the character the escape stands for, or nullopt if `c` does not
// name one of them (the caller decides what to do with quotes and \x / \u).
inline std::optional<char> simple(char c) {
  switch (c) {
    case 'n':
      return '\n';
    case 't':
      return '\t';
    case 'r':
      return '\r';
    case '\\':
      return '\\';
    case '0':
      return '\0';
    default:
      return std::nullopt;
  }
}

// Highest Unicode scalar value.
inline constexpr uint32_t kMaxScalar = 0x10FFFF;

// UTF-16 surrogates are not scalar values and may not appear on their own.
inline bool isSurrogate(uint32_t v) { return v >= 0xD800 && v <= 0xDFFF; }

// A Unicode scalar value is anything up to kMaxScalar that is not a surrogate.
inline bool isScalarValue(uint32_t v) {
  return v <= kMaxScalar && !isSurrogate(v);
}

// Value of one hex digit, or -1.
inline int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Number of bytes in the UTF-8 sequence that starts with `lead`, or 0 if
// `lead` is not a valid leading byte.
inline int utf8SequenceLength(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 0;
}

// Decode the UTF-8 sequence at the start of `s`. Returns the scalar value and
// writes the number of bytes consumed to `length`, or nullopt if the bytes are
// not well-formed UTF-8 (bad continuation byte, overlong form, surrogate, or
// out of range).
inline std::optional<uint32_t> decodeUtf8(std::string_view s, int& length) {
  if (s.empty()) return std::nullopt;
  const int len = utf8SequenceLength(static_cast<unsigned char>(s[0]));
  if (len == 0 || static_cast<size_t>(len) > s.size()) return std::nullopt;

  static constexpr uint32_t kLeadMask[5] = {0, 0x7F, 0x1F, 0x0F, 0x07};
  uint32_t value = static_cast<unsigned char>(s[0]) & kLeadMask[len];
  for (int i = 1; i < len; ++i) {
    auto byte = static_cast<unsigned char>(s[i]);
    if ((byte & 0xC0) != 0x80) return std::nullopt;
    value = (value << 6) | (byte & 0x3F);
  }

  // Reject overlong encodings: each length has a minimum value it may carry.
  static constexpr uint32_t kMinValue[5] = {0, 0, 0x80, 0x800, 0x10000};
  if (value < kMinValue[len]) return std::nullopt;
  if (!isScalarValue(value)) return std::nullopt;

  length = len;
  return value;
}

// Encode `scalar` as UTF-8 into `out` (at least 4 bytes). Returns the number
// of bytes written.
inline int encodeUtf8(uint32_t scalar, char* out) {
  if (scalar < 0x80) {
    out[0] = static_cast<char>(scalar);
    return 1;
  }
  if (scalar < 0x800) {
    out[0] = static_cast<char>(0xC0 | (scalar >> 6));
    out[1] = static_cast<char>(0x80 | (scalar & 0x3F));
    return 2;
  }
  if (scalar < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (scalar >> 12));
    out[1] = static_cast<char>(0x80 | ((scalar >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (scalar & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (scalar >> 18));
  out[1] = static_cast<char>(0x80 | ((scalar >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((scalar >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (scalar & 0x3F));
  return 4;
}

}  // namespace sun::escapes
