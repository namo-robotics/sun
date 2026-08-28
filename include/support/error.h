#pragma once

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "support/position.h"
#include "support/source_manager.h"

// ANSI color codes for terminal output
namespace ansi {
constexpr const char* red = "\033[1;31m";
constexpr const char* blue = "\033[1;34m";
constexpr const char* cyan = "\033[36m";
constexpr const char* yellow = "\033[1;33m";
constexpr const char* reset = "\033[0m";
}  // namespace ansi

// Render a diagnostic in the standard compiler format: colored label, blue
// file:line:column, the message, then the offending source line with a red
// caret under the column. Shared by SunError and multi-error passes like the
// borrow checker, so every compiler error looks the same.
inline std::string formatDiagnostic(const std::string& label,
                                    const std::string& labelColor,
                                    const std::string& message,
                                    const std::optional<Position>& location,
                                    const std::string& sourceLine,
                                    const std::string& prevSourceLine) {
  std::string out = labelColor + label + ansi::reset;
  if (location) {
    out += ": " + std::string(ansi::blue) + location->toString() + ansi::reset;
  }
  out += ": " + message;

  if (!sourceLine.empty() && location) {
    out += "\n";
    // Gutter width: leading space + line number digits + space before |
    int lineNumWidth = std::to_string(location->line).length();
    std::string gutter(lineNumWidth + 2, ' ');  // aligns with " N | "

    // Show previous line for context (if available)
    if (!prevSourceLine.empty() && location->line > 1) {
      out += " " + std::string(ansi::cyan) +
             std::to_string(location->line - 1) + ansi::reset + " | " +
             prevSourceLine + "\n";
    }

    // Show current line number (in cyan) and source
    out += " " + std::string(ansi::cyan) + std::to_string(location->line) +
           ansi::reset + " | " + sourceLine + "\n";

    // Show caret pointing to error column (in red)
    out += gutter + "| ";
    if (location->column > 1) {
      out += std::string(location->column - 1, ' ');
    }
    out += std::string(ansi::red) + "^" + ansi::reset;
  }
  return out;
}

// Custom error type for Sun compiler errors
class SunError : public std::exception {
 public:
  enum class Kind {
    Compile,  // General compilation error
    Parse,    // Parsing error
    Type,     // Type checking error
    Semantic  // Semantic analysis error
  };

  SunError(Kind kind, const std::string& message,
           std::optional<Position> loc = std::nullopt,
           const std::string& sourceLine = "",
           const std::string& prevSourceLine = "")
      : kind_(kind),
        message_(message),
        location_(loc),
        sourceLine_(sourceLine),
        prevSourceLine_(prevSourceLine) {
    buildFullMessage();
  }

  const char* what() const noexcept override { return fullMessage_.c_str(); }

  Kind getKind() const { return kind_; }
  const std::string& getMessage() const { return message_; }
  const std::optional<Position>& getLocation() const { return location_; }
  const std::string& getSourceLine() const { return sourceLine_; }

 private:
  std::string kindToString() const {
    switch (kind_) {
      case Kind::Compile:
        return "Error";
      case Kind::Parse:
        return "Parse Error";
      case Kind::Type:
        return "Type Error";
      case Kind::Semantic:
        return "Semantic Error";
    }
    return "Error";
  }

  void buildFullMessage() {
    fullMessage_ = formatDiagnostic(kindToString(), ansi::red, message_,
                                    location_, sourceLine_, prevSourceLine_);
  }

  Kind kind_;
  std::string message_;
  std::optional<Position> location_;
  std::string sourceLine_;
  std::string prevSourceLine_;
  std::string fullMessage_;
};

// Unified error handling - throws SunError and does not return

[[noreturn]] inline void logAndThrowError(
    const std::string& str, std::optional<Position> loc = std::nullopt) {
  std::string sourceLine, prevLine;
  if (loc && loc->filePath) {
    auto [current, prev] = SourceManager::instance().getLineWithContext(*loc);
    sourceLine = current;
    prevLine = prev;
  }
  throw SunError(SunError::Kind::Compile, str, loc, sourceLine, prevLine);
}

[[noreturn]] inline void logTypeError(
    const std::string& str, std::optional<Position> loc = std::nullopt) {
  std::string sourceLine, prevLine;
  if (loc && loc->filePath) {
    auto [current, prev] = SourceManager::instance().getLineWithContext(*loc);
    sourceLine = current;
    prevLine = prev;
  }
  throw SunError(SunError::Kind::Type, str, loc, sourceLine, prevLine);
}

[[noreturn]] inline void logParsingError(int line, int column,
                                         const std::string& str,
                                         const std::string& sourceLine = "",
                                         const std::string& filePath = "") {
  Position loc{
      line, column, 0,
      filePath.empty() ? std::nullopt : std::optional<std::string>(filePath)};
  throw SunError(SunError::Kind::Parse, str, loc, sourceLine);
}

// Overload accepting Position directly (preferred for new code)
[[noreturn]] inline void logParsingError(const Position& loc,
                                         const std::string& str,
                                         const std::string& sourceLine = "",
                                         const std::string& prevLine = "") {
  throw SunError(SunError::Kind::Parse, str, loc, sourceLine, prevLine);
}

[[noreturn]] inline void logSemanticError(
    const std::string& str, std::optional<Position> loc = std::nullopt) {
  std::string sourceLine, prevLine;
  if (loc && loc->filePath) {
    auto [current, prev] = SourceManager::instance().getLineWithContext(*loc);
    sourceLine = current;
    prevLine = prev;
  }
  throw SunError(SunError::Kind::Semantic, str, loc, sourceLine, prevLine);
}

// Log error without throwing - useful for non-fatal diagnostics
inline void logErrorNoThrow(const std::string& msg,
                            std::optional<Position> loc = std::nullopt) {
  if (loc) {
    std::cerr << "Error: " << loc->toString() << ": " << msg << std::endl;
  } else {
    std::cerr << "Error: " << msg << std::endl;
  }
}

// Non-fatal diagnostic (e.g. unreachable match arms)
inline void logWarning(const std::string& msg,
                       std::optional<Position> loc = std::nullopt) {
  if (loc) {
    std::cerr << "Warning: " << loc->toString() << ": " << msg << std::endl;
  } else {
    std::cerr << "Warning: " << msg << std::endl;
  }
}
