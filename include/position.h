#pragma once

#include <optional>
#include <string>

// Source location tracking for error messages and debugging
struct Position {
  int line = 1;
  int column = 1;
  int offset = 0;
  std::optional<std::string> filePath = std::nullopt;

  Position() = default;
  Position(int l, int c, int o = 0,
           std::optional<std::string> path = std::nullopt)
      : line(l), column(c), offset(o), filePath(std::move(path)) {}

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
