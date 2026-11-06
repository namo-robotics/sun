#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "driver/sun_config.h"

/** Downloads configured inputs and assembles distributable artifacts. */
namespace sun::driver {

/** A production artifact and its public name in a distribution. */
struct PackageArtifact {
  std::string name;
  std::string path;
  bool library = false;
};

/** A distribution's resolved membership, resources, and output base. */
struct PackagePlan {
  std::string name;
  std::filesystem::path output;
  std::vector<PackageArtifact> artifacts;
  std::vector<ConfigResource> resources;
};

/** Returns the SHA-256 digest of a file, reporting unreadable inputs. */
std::string packageFileHash(const std::filesystem::path& path);

/** Reports whether configured dependency resolution is active on this thread. */
bool isCurrentlyBuildingDependency();

/** Resolves a configured dependency to a verified local directory on demand,
 * without building its tests. */
std::filesystem::path resolveConfigDependency(
    const SunConfig& config, const std::string& name,
    const std::string& relativePath = "");

/** Resolves entrypoint-relative resources for an install or package layout. */
std::vector<ConfigResource> packageEntrypointResources(
    const SunConfig& config, const ConfigEntrypoint& entry);

/** Checks membership and output locations before compiling production files. */
std::vector<PackagePlan> planPackages(
    const SunConfig& config, const std::vector<PackageArtifact>& artifacts);

/** Stages and compresses complete packages, preserving valid unchanged outputs.
 */
void buildPackages(const SunConfig& config,
                   const std::vector<PackagePlan>& plans, bool archive = true);

}  // namespace sun::driver
