/** Makes recorded source locations portable without changing compiler input
 * paths. */
#pragma once
#include <filesystem>
#include <string>

/** Formats source locations stored in generated code and Moon declarations. */
namespace sun::support {
/** Makes absolute source paths relative to the entrypoint directory; already
 * relative paths retain the originating library's base. */
inline std::string relativeSourcePath(const std::string& path,
                                      const std::string& entrypointDirectory) {
  const std::filesystem::path source(path);
  if (path.empty() || !source.is_absolute()) {
    return path;
  }
  const auto root = std::filesystem::absolute(
                        entrypointDirectory.empty() ? "." : entrypointDirectory)
                        .lexically_normal();
  return source.lexically_normal().lexically_relative(root).generic_string();
}
}  // namespace sun::support
