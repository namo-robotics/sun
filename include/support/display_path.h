#pragma once

#include <filesystem>
#include <optional>
#include <string>

/** Renders file names without changing the paths used by the compiler. */
namespace sun::support {
/** Uses a fixed directory for human-readable paths until this scope ends.
 * An empty directory leaves the current display policy unchanged.
 */
class DisplayPathScope {
 public:
  /** Saves the current policy and selects an absolute display directory. */
  explicit DisplayPathScope(const std::filesystem::path& directory);
  /** Restores the policy of the enclosing invocation. */
  ~DisplayPathScope();
  /** Keeps scope restoration tied to its original lifetime. */
  DisplayPathScope(const DisplayPathScope&) = delete;
  /** Prevents replacing a scope that owns an enclosing policy. */
  DisplayPathScope& operator=(const DisplayPathScope&) = delete;

 private:
  std::optional<std::filesystem::path> previous_;
};

/** Writes and flushes a progress message with one path or an input/output pair.
 * Paths follow the active display policy; disabled levels produce no output.
 */
void logPathMessage(const std::string& level, const std::string& label,
                    const std::filesystem::path& path,
                    const std::optional<std::filesystem::path>& output = std::nullopt);

/** Logs an artifact's basename, linking existing files in formatted terminals.
 * Debug logging includes full artifact and optional source paths.
 */
void logBuildProgress(const std::string& label,
                      const std::filesystem::path& output,
                      const std::optional<std::filesystem::path>& source = std::nullopt);

/** Returns the display base for child build contexts, or the current directory.
 */
std::filesystem::path displayPathBase();

/** Makes paths inside the selected directory relative; outside paths stay
 * absolute. Without an active CLI scope, preserves the caller's spelling.
 */
std::string displayPath(const std::filesystem::path& path);
}  // namespace sun::support
