#pragma once

#include <optional>
#include <string>

// Source location tracking for error messages and debugging
struct Position {
  int line = 1;
  int column = 1;
  int offset = 0;  // Absolute byte position from start of source file
  std::optional<std::string> filePath = std::nullopt;

  // Optional end position for spanning tokens/expressions
  std::optional<int> endLine = std::nullopt;
  std::optional<int> endColumn = std::nullopt;

  Position() = default;
  Position(int l, int c, int o = 0,
           std::optional<std::string> path = std::nullopt)
      : line(l), column(c), offset(o), filePath(std::move(path)) {}

  // Set end position (for token/expression span)
  void setEnd(int el, int ec) {
    endLine = el;
    endColumn = ec;
  }

  // Check if this position has end info
  bool hasEnd() const { return endLine.has_value() && endColumn.has_value(); }

  // Format as "file:line:column" or "line:column" if no file
  std::string toString() const {
    std::string result;
    if (filePath) {
      result = *filePath + ":";
    }
    result += std::to_string(line) + ":" + std::to_string(column);
    return result;
  }
};
