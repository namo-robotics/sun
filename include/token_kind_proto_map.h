// token_kind_proto_map.h — single source of truth for the TokenKind <->
// proto TokenKind mapping used by AST serialization.
//
// Add one row per operator token that can appear in a serialized AST; both
// conversion directions (and the serialization roundtrip test) derive from
// this table, so the two directions cannot drift.

#pragma once

#include <utility>

#include "lexer.h"
#include "types.pb.h"

namespace sun {
namespace serialization {

inline constexpr std::pair<TokenKind, ast::TokenKind> kTokenKindProtoMap[] = {
    {TokenKind::PLUS, ast::TOKEN_KIND_PLUS},
    {TokenKind::MINUS, ast::TOKEN_KIND_MINUS},
    {TokenKind::STAR, ast::TOKEN_KIND_STAR},
    {TokenKind::SLASH, ast::TOKEN_KIND_SLASH},
    {TokenKind::LESS, ast::TOKEN_KIND_LESS},
    {TokenKind::LESS_EQUAL, ast::TOKEN_KIND_LESS_EQUAL},
    {TokenKind::GREATER, ast::TOKEN_KIND_GREATER},
    {TokenKind::GREATER_EQUAL, ast::TOKEN_KIND_GREATER_EQUAL},
    {TokenKind::EQUAL_EQUAL, ast::TOKEN_KIND_EQUAL_EQUAL},
    {TokenKind::NOT_EQUAL, ast::TOKEN_KIND_NOT_EQUAL},
    {TokenKind::EQUAL, ast::TOKEN_KIND_EQUAL},
    {TokenKind::AND, ast::TOKEN_KIND_AND},
    {TokenKind::OR, ast::TOKEN_KIND_OR},
    {TokenKind::NOT, ast::TOKEN_KIND_NOT},
    {TokenKind::AMPERSAND, ast::TOKEN_KIND_AMPERSAND},
    {TokenKind::PIPE, ast::TOKEN_KIND_PIPE},
    {TokenKind::CARET, ast::TOKEN_KIND_CARET},
    {TokenKind::PERCENT, ast::TOKEN_KIND_PERCENT},
    {TokenKind::LEFT_SHIFT, ast::TOKEN_KIND_SHIFT_LEFT},
    {TokenKind::RIGHT_SHIFT, ast::TOKEN_KIND_SHIFT_RIGHT},
    {TokenKind::TILDE, ast::TOKEN_KIND_TILDE},
};

inline ast::TokenKind toProtoTokenKind(TokenKind kind) {
  for (const auto& [cppKind, protoKind] : kTokenKindProtoMap) {
    if (cppKind == kind) return protoKind;
  }
  return ast::TOKEN_KIND_UNKNOWN;
}

inline TokenKind fromProtoTokenKind(ast::TokenKind kind) {
  for (const auto& [cppKind, protoKind] : kTokenKindProtoMap) {
    if (protoKind == kind) return cppKind;
  }
  return TokenKind::UNKNOWN;
}

}  // namespace serialization
}  // namespace sun
