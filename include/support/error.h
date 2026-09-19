#pragma once

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/position.h"
#include "support/source_manager.h"

/** Provides source locations, diagnostics, and shared compiler utilities. */
namespace sun::support {

// ANSI color codes for terminal output

constexpr const char* red = "\033[1;31m";
constexpr const char* blue = "\033[1;34m";
constexpr const char* cyan = "\033[36m";
constexpr const char* yellow = "\033[1;33m";
constexpr const char* reset = "\033[0m";

/**
 * Render a diagnostic in the standard compiler format: colored label, blue
 * file:line:column, the message, then the offending source line with a red
 * caret under the column. Shared by SunError and multi-error passes like the
 * borrow checker, so every compiler error looks the same.
 */
inline std::string formatDiagnostic(const std::string& label,
                                    const std::string& labelColor,
                                    const std::string& message,
                                    const std::optional<Position>& location,
                                    const std::string& sourceLine,
                                    const std::string& prevSourceLine) {
  std::string out = labelColor + label + reset;
  if (location) {
    out += ": " + std::string(blue) + location->toString() + reset;
  }
  out += ": " + message;

  if (!sourceLine.empty() && location) {
    out += "\n";
    // Gutter width: leading space + line number digits + space before |
    int lineNumWidth = std::to_string(location->line).length();
    std::string gutter(lineNumWidth + 2, ' ');  // aligns with " N | "

    // Show previous line for context (if available)
    if (!prevSourceLine.empty() && location->line > 1) {
      out += " " + std::string(cyan) + std::to_string(location->line - 1) +
             reset + " | " + prevSourceLine + "\n";
    }

    // Show current line number (in cyan) and source
    out += " " + std::string(cyan) + std::to_string(location->line) + reset +
           " | " + sourceLine + "\n";

    // Show caret pointing to error column (in red)
    out += gutter + "| ";
    if (location->column > 1) {
      out += std::string(location->column - 1, ' ');
    }
    out += std::string(red) + "^" + reset;
  }
  return out;
}

/**
 * Another place an error points at. A pass that finds several problems at once
 * (the borrow checker) reports the first as the error itself and lists the rest
 * here, together with any location that explains it, so an editor can mark
 * every one of them instead of only the first.
 */
struct RelatedDiagnostic {
  /** Controls which diagnostic severities are written to the log. */
  enum class Level {
    Error,  // A problem in its own right
    Note    // Context for the problem, such as a conflicting borrow
  };

  std::string message;
  Position location;
  Level level = Level::Error;
};

/**
 * Custom error type for Sun compiler errors
 */
class SunError : public std::exception {
 public:
  /** Identifies the compiler stage or rule that produced an error. */
  enum class Kind {
    Compile,   // General compilation error
    Parse,     // Parsing error
    Type,      // Type checking error
    Semantic,  // Semantic analysis error
    Borrow     // Borrow checking error
  };

  /** Creates a compiler exception with its diagnostic category and source context. */
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

  /** Returns the formatted diagnostic message through the exception interface. */
  const char* what() const noexcept override { return fullMessage_.c_str(); }

  /** Returns the type category used for semantic checks and dispatch. */
  Kind getKind() const { return kind_; }
  /** Returns the message stored by this object. */
  const std::string& getMessage() const { return message_; }
  /** Returns the source position used for diagnostics. */
  const std::optional<Position>& getLocation() const { return location_; }
  /** Returns the source line stored by this object. */
  const std::string& getSourceLine() const { return sourceLine_; }

  /**
   * Record another place this error points at. It joins the printed message,
   * rendered like any other diagnostic.
   */
  void addRelated(const std::string& message, const Position& location,
                  RelatedDiagnostic::Level level) {
    related_.push_back({message, location, level});
    buildFullMessage();
  }
  /** Returns the related diagnostic locations. */
  const std::vector<RelatedDiagnostic>& getRelated() const { return related_; }

  /**
   * What the error is called when printed, such as "Parse Error"
   */
  std::string getLabel() const { return kindToString(); }

 private:
  /** Returns the readable category name for this compiler error. */
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
      case Kind::Borrow:
        return "Borrow check failed";
    }
    return "Error";
  }

  /** Combines the error category, message, and source context into a diagnostic. */
  void buildFullMessage() {
    fullMessage_ = formatDiagnostic(kindToString(), red, message_, location_,
                                    sourceLine_, prevSourceLine_);
    for (const auto& related : related_) {
      bool isNote = related.level == RelatedDiagnostic::Level::Note;
      auto [line, prevLine] =
          SourceManager::instance().getLineWithContext(related.location);
      fullMessage_ +=
          "\n" + formatDiagnostic(isNote ? "Note" : kindToString(),
                                  isNote ? cyan : red, related.message,
                                  related.location, line, prevLine);
    }
  }

  Kind kind_;
  std::string message_;
  std::optional<Position> location_;
  std::string sourceLine_;
  std::string prevSourceLine_;
  std::string fullMessage_;
  std::vector<RelatedDiagnostic> related_;
};

// Unified error handling - throws SunError and does not return

/** Reports a compilation error and throws an exception to stop the current operation. */
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

/** Reports a type error with its optional source position. */
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

/** Reports invalid syntax with its source location and line text. */
[[noreturn]] inline void logParsingError(int line, int column,
                                         const std::string& str,
                                         const std::string& sourceLine = "",
                                         const std::string& filePath = "") {
  Position loc{
      line, column, 0,
      filePath.empty() ? std::nullopt : std::optional<std::string>(filePath)};
  throw SunError(SunError::Kind::Parse, str, loc, sourceLine);
}

/**
 * Overload accepting Position directly (preferred for new code)
 */
[[noreturn]] inline void logParsingError(const Position& loc,
                                         const std::string& str,
                                         const std::string& sourceLine = "",
                                         const std::string& prevLine = "") {
  throw SunError(SunError::Kind::Parse, str, loc, sourceLine, prevLine);
}

/** Reports a semantic error with its optional source position. */
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

/**
 * Log error without throwing - useful for non-fatal diagnostics
 */
inline void logErrorNoThrow(const std::string& msg,
                            std::optional<Position> loc = std::nullopt) {
  if (loc) {
    std::cerr << "Error: " << loc->toString() << ": " << msg << std::endl;
  } else {
    std::cerr << "Error: " << msg << std::endl;
  }
}

/**
 * Non-fatal diagnostic (e.g. unreachable match arms)
 */
inline void logWarning(const std::string& msg,
                       std::optional<Position> loc = std::nullopt) {
  if (loc) {
    std::cerr << "Warning: " << loc->toString() << ": " << msg << std::endl;
  } else {
    std::cerr << "Warning: " << msg << std::endl;
  }
}

}  // namespace sun::support
