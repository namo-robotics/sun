#pragma once

#include <string>

#include "driver/sun_config.h"
#include "moon_bundling/moon_builder.h"
#include "support/save_and_restore.h"

/** Coordinates compilation, dependency loading, linking, and program execution.
 */
namespace sun::driver {

/** Carries compiler settings through lazy and transitive source builds. */
class GitDependencyBuildScope {
 public:
  /** Selects settings and shares source refreshes within one command. */
  GitDependencyBuildScope(bool debugInfo, bool optimize,
                          bool forceRebuild = false, bool refresh = false);
  /** Restores the enclosing build's settings. */
  ~GitDependencyBuildScope();
  /** Prevents copying ownership of build settings. */
  GitDependencyBuildScope(const GitDependencyBuildScope&) = delete;
  /** Prevents replacing ownership of build settings. */
  GitDependencyBuildScope& operator=(const GitDependencyBuildScope&) = delete;

 private:
  sun::support::SaveAndRestore<sun::moon_bundling::MoonBuildOptions> settings_;
  sun::support::SaveAndRestore<bool> refresh_;
};

/** Reports whether a Git dependency build is currently active. */
bool isCurrentlyBuildingGitDependency();

/** Borrows the active Git dependency settings, or returns null outside a build. */
const sun::moon_bundling::MoonBuildOptions* gitDependencyBuildOptions();

/** Builds a direct source file or configured product in an isolated directory.
 * Direct source builds use the consumer config; configured products run their
 * build graph and stage resources outside the checkout. Tests are excluded.
 * Reports missing or ambiguous entrypoints and dependency cycles. */
std::filesystem::path resolveGitDependency(const SunConfig& consumer,
                                           const std::string& name);

/** Rejects invalid Git URLs, invalid revisions, and escaping source paths. */
void validateGitSource(const ConfigEntrypoint& entry);

/** Fetches a versioned checkout once and returns its entrypoint's absolute
 * path. Uses SUN_GIT_CACHE or .sun/cache/git beneath the current working directory
 * and Git's normal authentication.
 * Refresh fetches a new snapshot while preserving previous checkouts.
 * Reports fetch failures and paths escaping the checkout as compiler errors. */
std::string resolveGitEntrypoint(const ConfigEntrypoint& entry,
                                 bool refresh = false);

}  // namespace sun::driver
