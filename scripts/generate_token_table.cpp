/** Generates a complete token transition table from the shared lexer grammar.
 */
#include <fstream>
#include <iostream>

#include "parsing/lexer.h"

/** Writes deterministic C++ arrays, failing the build on an output error. */
int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: generate_token_table OUTPUT\n";
    return 1;
  }
  auto dfa = sun::parsing::RegexParser().parse(
      sun::parsing::Lexer::getStaticFullRegex());
  std::ofstream output(argv[1]);
  output << "// Generated from the lexer grammar; do not edit.\n"
         << "static constexpr int32_t kTokenStart = " << dfa.startState()
         << ";\nstatic constexpr std::array<int32_t, 256> kTokenTransitions[] "
            "= {\n";
  // Stepping can discover more states; emit every byte of each new state.
  for (int state = 0; state < dfa.stateCount(); ++state) {
    output << "  {{";
    for (int byte = 0; byte < sun::parsing::DFA::kAlphabet; ++byte)
      output << (byte ? "," : "")
             << dfa.step(state, static_cast<unsigned char>(byte));
    output << "}},\n";
  }
  output << "};\nstatic constexpr int32_t kTokenKinds[] = {\n";
  for (int state = 0; state < dfa.stateCount(); ++state)
    output << "  " << dfa.acceptKind(state) << ",\n";
  output << "};\n";
  output.close();
  if (!output) {
    std::cerr << "could not write token table: " << argv[1] << '\n';
    return 1;
  }
}
