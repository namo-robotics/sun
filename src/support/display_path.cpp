/** Keeps display-only path policy separate from source and artifact identity.
 */
#include "support/display_path.h"

#include <utility>

/** Renders paths for the current compiler invocation. */
namespace sun::support {
/** Isolates rendering policy between independent compiler threads. */
namespace {
thread_local std::optional<std::filesystem::path> displayRoot;
}

DisplayPathScope::DisplayPathScope(const std::filesystem::path& directory)
    : previous_(displayRoot) {
  if (!directory.empty()) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(directory, error);
    if (!error) {
      displayRoot = absolute.lexically_normal();
    }
  }
}

DisplayPathScope::~DisplayPathScope() { displayRoot = std::move(previous_); }

std::filesystem::path displayPathBase() {
  if (displayRoot) {
    return *displayRoot;
  }
  std::error_code error;
  return std::filesystem::current_path(error);
}

std::string displayPath(const std::filesystem::path& path) {
  const auto spelling = path.string();
  if (!displayRoot || path.empty() ||
      (spelling.front() == '<' && spelling.back() == '>')) {
    return path.string();
  }
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error) {
    return path.string();
  }
  absolute = absolute.lexically_normal();
  auto relative = absolute.lexically_relative(*displayRoot);
  if (!relative.empty() && *relative.begin() != "..") {
    return relative.string();
  }
  return absolute.string();
}
}  // namespace sun::support
