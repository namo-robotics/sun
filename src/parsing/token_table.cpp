/** Connects the generated token automaton to the scanner. */
#include "parsing/token_table.h"

/** Turns source text into syntax trees and provides source formatting. */
namespace sun::parsing {
/** Keeps generated storage private while exposing an immutable view. */
namespace {
#include "generated/token_table.inc"
}  // namespace

const TokenTable& getTokenTable() {
  static constexpr TokenTable table{kTokenTransitions, kTokenKinds,
                                    kTokenStart};
  return table;
}

}  // namespace sun::parsing
