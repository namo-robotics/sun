// char_literal_ast.h — CharLiteralAST class

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "ast/expr_ast.h"
#include "parsing/escapes.h"

// A character literal ('a', type char) or a byte literal (b'a', type u8).
//
// Both are their own node rather than a flag on NumberExprAST: an integer
// literal takes its type from context, and these two do not. A char is always
// a char and b'a' is always a u8, so they must be immune to the literal
// coercion that keys off ASTNodeType::NUMBER.
class CharLiteralAST : public ExprAST {
  uint32_t value_;  // Unicode scalar value, or the byte for a byte literal
  bool isByte_;

 public:
  CharLiteralAST(uint32_t value, bool isByte)
      : value_(value), isByte_(isByte) {}

  ASTNodeType getType() const override { return ASTNodeType::CHAR_LITERAL; }

  uint32_t getValue() const { return value_; }
  bool isByte() const { return isByte_; }

  // A readable spelling for diagnostics and the AST dump. The formatter does
  // not use this — it reprints literals verbatim from the source.
  std::string toString() const override {
    std::string out = isByte_ ? "b'" : "'";
    switch (value_) {
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\0':
        out += "\\0";
        break;
      case '\'':
        out += "\\'";
        break;
      default:
        if (value_ >= 0x20 && value_ < 0x7F) {
          out += static_cast<char>(value_);
        } else if (isByte_) {
          out += "\\x";
          static const char* kDigits = "0123456789ABCDEF";
          out += kDigits[(value_ >> 4) & 0xF];
          out += kDigits[value_ & 0xF];
        } else {
          char utf8[4];
          int n = sun::escapes::encodeUtf8(value_, utf8);
          out.append(utf8, static_cast<size_t>(n));
        }
        break;
    }
    return out + "'";
  }

  std::string dotLabel() const override {
    return std::string(isByte_ ? "Byte\n" : "Char\n") + toString();
  }
};
