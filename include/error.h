#pragma once

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "position.h"

// ANSI color codes for terminal output
namespace ansi {
constexpr const char* red = "\033[1;31m";
constexpr const char* blue = "\033[1;34m";
constexpr const char* cyan = "\033[36m";
constexpr const char* yellow = "\033[1;33m";
constexpr const char* reset = "\033[0m";
}  // namespace ansi

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
    // Error type in red
    fullMessage_ = std::string(ansi::red) + kindToString() + ansi::reset;
    if (location_) {
      // File path in blue
      fullMessage_ +=
          ": " + std::string(ansi::blue) + location_->toString() + ansi::reset;
    }
    fullMessage_ += ": " + message_;

    // Add source preview if available
    if (!sourceLine_.empty() && location_) {
      fullMessage_ += "\n";
      // Gutter width: leading space + line number digits + space before |
      int lineNumWidth = std::to_string(location_->line).length();
      std::string gutter(lineNumWidth + 2, ' ');  // aligns with " N | "

      // Show previous line for context (if available)
      if (!prevSourceLine_.empty() && location_->line > 1) {
        fullMessage_ += " " + std::string(ansi::cyan) +
                        std::to_string(location_->line - 1) + ansi::reset +
                        " | " + prevSourceLine_ + "\n";
      }

      // Show current line number (in cyan) and source
      fullMessage_ += " " + std::string(ansi::cyan) +
                      std::to_string(location_->line) + ansi::reset + " | " +
                      sourceLine_ + "\n";

      // Show caret pointing to error column (in red)
      fullMessage_ += gutter + "| ";
      if (location_->column > 1) {
        fullMessage_ += std::string(location_->column - 1, ' ');
      }
      fullMessage_ += std::string(ansi::red) + "^" + ansi::reset;
    }
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
  throw SunError(SunError::Kind::Compile, str, loc);
}

[[noreturn]] inline void logTypeError(
    const std::string& str, std::optional<Position> loc = std::nullopt) {
  throw SunError(SunError::Kind::Type, str, loc);
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
  throw SunError(SunError::Kind::Semantic, str, loc);
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
