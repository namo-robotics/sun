#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "position.h"

/// Global source text manager for error reporting.
/// Stores source text for all compiled files so that error messages
/// can display source line previews even after parsing is complete.
class SourceManager {
 public:
  /// Get the global singleton instance
  static SourceManager& instance() {
    static SourceManager mgr;
    return mgr;
  }

  /// Register source text for a file path.
  /// Call this when reading a file for compilation.
  void addSource(const std::string& filePath, const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_[filePath] = content;
    // Pre-compute line offsets for fast line lookup
    computeLineOffsets(filePath);
  }

  /// Register source text without a file path (e.g., executeString).
  /// Uses a synthetic path like "<string:N>".
  std::string addAnonymousSource(const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string syntheticPath =
        "<string:" + std::to_string(anonymousCounter_++) + ">";
    sources_[syntheticPath] = content;
    computeLineOffsets(syntheticPath);
    return syntheticPath;
  }

  /// Get a specific line from a file (1-indexed).
  /// Returns empty string if file or line not found.
  std::string getLine(const std::string& filePath, int lineNum) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sources_.find(filePath);
    if (it == sources_.end()) return "";

    auto offsetIt = lineOffsets_.find(filePath);
    if (offsetIt == lineOffsets_.end()) return "";

    const auto& offsets = offsetIt->second;
    if (lineNum < 1 || lineNum > static_cast<int>(offsets.size())) return "";

    size_t start = offsets[lineNum - 1];
    size_t end;
    if (lineNum < static_cast<int>(offsets.size())) {
      end = offsets[lineNum];
      // Skip newline character at end of line
      if (end > start && it->second[end - 1] == '\n') end--;
      if (end > start && it->second[end - 1] == '\r') end--;
    } else {
      end = it->second.size();
    }

    return it->second.substr(start, end - start);
  }

  /// Get line from a Position (uses filePath if available).
  std::string getLine(const Position& pos) const {
    if (pos.filePath) {
      return getLine(*pos.filePath, pos.line);
    }
    return "";
  }

  /// Get source line and previous line for error preview.
  /// Returns {currentLine, previousLine}.
  std::pair<std::string, std::string> getLineWithContext(
      const Position& pos) const {
    std::string currentLine = getLine(pos);
    std::string prevLine;
    if (pos.filePath && pos.line > 1) {
      prevLine = getLine(*pos.filePath, pos.line - 1);
    }
    return {currentLine, prevLine};
  }

  /// Check if source is available for a file path.
  bool hasSource(const std::string& filePath) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.find(filePath) != sources_.end();
  }

  /// Clear all stored sources. Useful for testing.
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_.clear();
    lineOffsets_.clear();
    anonymousCounter_ = 0;
  }

 private:
  SourceManager() = default;
  SourceManager(const SourceManager&) = delete;
  SourceManager& operator=(const SourceManager&) = delete;

  /// Compute line start offsets for fast line lookup.
  void computeLineOffsets(const std::string& filePath) {
    auto it = sources_.find(filePath);
    if (it == sources_.end()) return;

    std::vector<size_t> offsets;
    offsets.push_back(0);  // Line 1 starts at offset 0

    const std::string& content = it->second;
    for (size_t i = 0; i < content.size(); ++i) {
      if (content[i] == '\n') {
        offsets.push_back(i + 1);  // Next line starts after newline
      }
    }

    lineOffsets_[filePath] = std::move(offsets);
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::string> sources_;
  std::map<std::string, std::vector<size_t>> lineOffsets_;
  int anonymousCounter_ = 0;
};
