#pragma once

#include "driver/sun_config.h"

/** Checks and explicitly installs dependencies supplied by the host system. */
namespace sun::driver {

/** Runs declared checks and validates cached Debian artifacts against installed packages.
 * System dependencies without a check are left to the package manager.
 */
void checkSystemDependencies(const SunConfig& config);

/** Requests native dependency installation, or prints a plan when dryRun is true.
 * Passing checks skip installation; unchecked packages go to the package manager.
 * Refuses host package-manager operations for a foreign compilation target.
 */
void installSystemDependencies(const SunConfig& config, bool dryRun, bool yes);

/** Enables explicit installation for dependencies reached through Git builds. */
class DependencyInstallScope {
 public:
  /** Selects installation instead of checking for the lifetime of this scope. */
  explicit DependencyInstallScope(bool yes);
  /** Restores the previous dependency policy. */
  ~DependencyInstallScope();
  /** Prevents copying a scope that restores thread-local state. */
  DependencyInstallScope(const DependencyInstallScope&) = delete;
  /** Prevents assigning a scope that restores thread-local state. */
  DependencyInstallScope& operator=(const DependencyInstallScope&) = delete;

 private:
  bool previousInstall_;
  bool previousYes_;
};
}  // namespace sun::driver
