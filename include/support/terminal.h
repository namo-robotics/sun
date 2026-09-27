#pragma once

#include <cstdlib>
#include <string>

#include <llvm/Support/raw_ostream.h>

/** Formats messages written by the compiler and its command-line tools. */
namespace sun::support {

/** Returns whether a terminal stream should receive ANSI colors. */
inline bool terminalColors(llvm::raw_ostream& stream) {
  const char* term = std::getenv("TERM");
  return stream.has_colors() && !std::getenv("NO_COLOR") &&
         (!term || std::string(term) != "dumb");
}

/** Builds a severity prefix, coloring it only for an interactive terminal. */
inline std::string messagePrefix(const std::string& level,
                                 llvm::raw_ostream& stream) {
  std::string prefix = "[sun][" + level + "]:";
  if (terminalColors(stream)) {
    const char* color = level == "error" ? "\033[1;31m"
                        : level == "warning" ? "\033[1;33m"
                                             : "\033[36m";
    prefix = color + prefix + "\033[0m";
  }
  return prefix + " ";
}

/** Starts a compiler message on stdout for info or stderr for diagnostics. */
inline llvm::raw_ostream& messageStream(const std::string& level) {
  auto& stream = level == "info" ? llvm::outs() : llvm::errs();
  stream << messagePrefix(level, stream);
  return stream;
}

/** Writes each line of a complete message with its own severity prefix. */
inline void logMessage(const std::string& level, const std::string& message) {
  size_t start = 0;
  while (start < message.size()) {
    size_t end = message.find('\n', start);
    messageStream(level) << message.substr(start, end - start) << "\n";
    if (end == std::string::npos) break;
    start = end + 1;
  }
}

}  // namespace sun::support
