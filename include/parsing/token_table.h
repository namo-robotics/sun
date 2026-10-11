/** Provides the immutable token automaton generated from the lexer grammar. */
#pragma once

#include <array>
#include <cstdint>
#include <span>

/** Turns source text into syntax trees and provides source formatting. */
namespace sun::parsing {

/** Views build-generated transitions without allocating or constructing a DFA.
 */
struct TokenTable {
  std::span<const std::array<int32_t, 256>> transitions;
  std::span<const int32_t> kinds;
  int32_t start;

  /** Returns the state from which token matching begins. */
  int32_t startState() const { return start; }
  /** Advances the scanner by one input byte. */
  int32_t step(int32_t state, unsigned char byte) const {
    return transitions[state][byte];
  }
  /** Returns the winning token kind, or a negative value for no match. */
  int32_t acceptKind(int32_t state) const { return kinds[state]; }
};

/** Returns the process-wide, read-only token transition table. */
const TokenTable& getTokenTable();

}  // namespace sun::parsing
