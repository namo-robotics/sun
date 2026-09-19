// token_kind_proto_map.h — single source of truth for the TokenKind <->
// proto TokenKind mapping used by AST serialization.
//
// Add one row per operator token that can appear in a serialized AST; both
// conversion directions (and the serialization roundtrip test) derive from
// this table, so the two directions cannot drift.

#pragma once

#include <utility>

#include "parsing/lexer.h"
#include "types.pb.h"

/** Converts syntax trees to and from the compiler protobuf representation. */
namespace sun::serialization {
namespace pbc = sun::proto::ast;

using sun::parsing::TokenKind;

inline constexpr std::pair<TokenKind, pbc::TokenKind> kTokenKindProtoMap[] = {
    {TokenKind::PLUS, pbc::TOKEN_KIND_PLUS},
    {TokenKind::MINUS, pbc::TOKEN_KIND_MINUS},
    {TokenKind::STAR, pbc::TOKEN_KIND_STAR},
    {TokenKind::SLASH, pbc::TOKEN_KIND_SLASH},
    {TokenKind::LESS, pbc::TOKEN_KIND_LESS},
    {TokenKind::LESS_EQUAL, pbc::TOKEN_KIND_LESS_EQUAL},
    {TokenKind::GREATER, pbc::TOKEN_KIND_GREATER},
    {TokenKind::GREATER_EQUAL, pbc::TOKEN_KIND_GREATER_EQUAL},
    {TokenKind::EQUAL_EQUAL, pbc::TOKEN_KIND_EQUAL_EQUAL},
    {TokenKind::NOT_EQUAL, pbc::TOKEN_KIND_NOT_EQUAL},
    {TokenKind::EQUAL, pbc::TOKEN_KIND_EQUAL},
    {TokenKind::AND, pbc::TOKEN_KIND_AND},
    {TokenKind::OR, pbc::TOKEN_KIND_OR},
    {TokenKind::NOT, pbc::TOKEN_KIND_NOT},
    {TokenKind::AMPERSAND, pbc::TOKEN_KIND_AMPERSAND},
    {TokenKind::PIPE, pbc::TOKEN_KIND_PIPE},
    {TokenKind::CARET, pbc::TOKEN_KIND_CARET},
    {TokenKind::PERCENT, pbc::TOKEN_KIND_PERCENT},
    {TokenKind::LEFT_SHIFT, pbc::TOKEN_KIND_SHIFT_LEFT},
    {TokenKind::RIGHT_SHIFT, pbc::TOKEN_KIND_SHIFT_RIGHT},
    {TokenKind::TILDE, pbc::TOKEN_KIND_TILDE},
    {TokenKind::PLUS_ASSIGN, pbc::TOKEN_KIND_PLUS_ASSIGN},
    {TokenKind::MINUS_ASSIGN, pbc::TOKEN_KIND_MINUS_ASSIGN},
    {TokenKind::STAR_ASSIGN, pbc::TOKEN_KIND_STAR_ASSIGN},
    {TokenKind::SLASH_ASSIGN, pbc::TOKEN_KIND_SLASH_ASSIGN},
    {TokenKind::PERCENT_ASSIGN, pbc::TOKEN_KIND_PERCENT_ASSIGN},
    {TokenKind::AMP_ASSIGN, pbc::TOKEN_KIND_AMP_ASSIGN},
    {TokenKind::PIPE_ASSIGN, pbc::TOKEN_KIND_PIPE_ASSIGN},
    {TokenKind::CARET_ASSIGN, pbc::TOKEN_KIND_CARET_ASSIGN},
    {TokenKind::LEFT_SHIFT_ASSIGN, pbc::TOKEN_KIND_SHIFT_LEFT_ASSIGN},
    {TokenKind::RIGHT_SHIFT_ASSIGN, pbc::TOKEN_KIND_SHIFT_RIGHT_ASSIGN},
};

/** Converts a lexer token category to its protobuf representation. */
inline pbc::TokenKind toProtoTokenKind(TokenKind kind) {
  for (const auto& [cppKind, protoKind] : kTokenKindProtoMap) {
    if (cppKind == kind) return protoKind;
  }
  return pbc::TOKEN_KIND_UNKNOWN;
}

/** Converts a protobuf token category to the lexer representation. */
inline TokenKind fromProtoTokenKind(pbc::TokenKind kind) {
  for (const auto& [cppKind, protoKind] : kTokenKindProtoMap) {
    if (protoKind == kind) return cppKind;
  }
  return TokenKind::UNKNOWN;
}

}  // namespace sun::serialization
