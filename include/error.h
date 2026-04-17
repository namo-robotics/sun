#pragma once

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "position.h"

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
           std::optional<Position> loc = std::nullopt)
      : kind_(kind), message_(message), location_(loc) {
    // Build full message with location if available
    if (loc) {
      fullMessage_ = kindToString() + ": " + loc->toString() + ": " + message;
    } else {
      fullMessage_ = kindToString() + ": " + message;
    }
  }

  const char* what() const noexcept override { return fullMessage_.c_str(); }

  Kind getKind() const { return kind_; }
  const std::string& getMessage() const { return message_; }
  const std::optional<Position>& getLocation() const { return location_; }

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

  Kind kind_;
  std::string message_;
  std::optional<Position> location_;
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
                                         const std::string& str) {
  Position loc{line, column};
  throw SunError(SunError::Kind::Parse, str, loc);
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
