#pragma once

#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <string>
#include <string_view>

/** Formats messages written by the compiler and its command-line tools. */
namespace sun::support {

/** Returns whether a terminal stream should receive ANSI colors. */
inline bool terminalColors(llvm::raw_ostream& stream) {
  const char* term = std::getenv("TERM");
  return stream.has_colors() && !std::getenv("NO_COLOR") &&
         (!term || std::string(term) != "dumb");
}

/** Styles text with a severity color, leaving redirected output unchanged. */
inline std::string terminalStyledText(const std::string& text,
                                      const std::string& level,
                                      llvm::raw_ostream& stream) {
  if (!terminalColors(stream)) return text;
  const char* color = level == "error"     ? "\033[1;31m"
                      : level == "warning" ? "\033[1;33m"
                                           : "\033[36m";
  return color + text + "\033[0m";
}

/** Builds a severity prefix, coloring it only for an interactive terminal. */
inline std::string messagePrefix(const std::string& level,
                                 llvm::raw_ostream& stream) {
  return terminalStyledText("[sun][" + level + "]:", level, stream) + " ";
}

/** Filters progress and warnings using SUN_LOG_LEVEL (info, warn, or error).
 * Missing or unrecognized values use warn; errors and diagnostic notes remain
 * visible at every level.
 */
inline bool messageEnabled(const std::string& level) {
  const char* configured = std::getenv("SUN_LOG_LEVEL");
  const std::string_view threshold = configured ? configured : "warn";
  if (level == "info") return threshold == "info";
  if (level == "warning" || level == "warn") return threshold != "error";
  return true;
}

/** Starts an enabled compiler message, discarding messages below the threshold.
 */
inline llvm::raw_ostream& messageStream(const std::string& level) {
  if (!messageEnabled(level)) return llvm::nulls();
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
