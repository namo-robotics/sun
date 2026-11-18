#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

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

/** Labels configured outputs by entrypoint identity until this scope ends. */
class BuildProgressScope {
 public:
  /** Selects the production and test labels, for example example/ui/viewc.
   * @param identity Configured package and entrypoint ID.
   * @param output Production artifact path.
   * @param tests Optional companion test artifact path.
   */
  BuildProgressScope(const std::string& identity,
                     const std::filesystem::path& output,
                     const std::filesystem::path& tests);
  /** Restores labels belonging to the enclosing build. */
  ~BuildProgressScope();
  /** Keeps restoration tied to the original scope. */
  BuildProgressScope(const BuildProgressScope&) = delete;
  /** Prevents replacing an active scope. */
  BuildProgressScope& operator=(const BuildProgressScope&) = delete;

 private:
  std::array<std::pair<std::filesystem::path, std::string>, 2> previous_;
};

/** Writes and flushes a progress message with one path or an input/output pair.
 * Paths follow the active display policy; disabled levels produce no output.
 */
void logPathMessage(
    const std::string& level, const std::string& label,
    const std::filesystem::path& path,
    const std::optional<std::filesystem::path>& output = std::nullopt);

/** Logs a configured entrypoint ID, or a basename for an unconfigured output.
 * Existing files remain linked in formatted terminals.
 * Debug logging includes full artifact and optional source paths.
 */
void logBuildProgress(
    const std::string& label, const std::filesystem::path& output,
    const std::optional<std::filesystem::path>& source = std::nullopt);

/** Returns the display base for child build contexts, or the current directory.
 */
std::filesystem::path displayPathBase();

/** Makes paths inside the selected directory relative; outside paths stay
 * absolute. Without an active CLI scope, preserves the caller's spelling.
 */
std::string displayPath(const std::filesystem::path& path);
}  // namespace sun::support
