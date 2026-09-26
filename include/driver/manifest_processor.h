// manifest_processor.h — Shared manifest handling for the driver, the
// --emit-moon path and the LSP: locate the manifest block, resolve entry
// paths through local, configured and installed directories, and split
// the entries into .sun files, .moon imports and .proto schemas. Moon
// entries always name local .moon files. Configured dependencies are fetched
// lazily when their directory variables are used. Entries may reference path variables
// ("$LIBS/util.moon"), defined by --path-var (which wins), the nearest sun-config.json,
// project / language-server defaults, or the environment.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ast/block_expr_ast.h"
#include "ast/manifest_ast.h"
#include "driver/sun_config.h"
#include "moon_bundling/moon_import.h"

/** Coordinates compilation, dependency loading, linking, and program execution.
 */
namespace sun::driver {

/** Dependencies and settings after manifest paths and targets are resolved. */
struct ResolvedManifest {
  std::vector<std::string> sunFiles;  // resolved .sun paths (manifest order)
  std::vector<sun::moon_bundling::MoonImport>
      moonImports;                        // resolved .moon imports
  std::vector<std::string> protoFiles;    // resolved .proto paths
  std::vector<std::string> archiveFiles;  // resolved native .a archives
  // resolved `test_files:` paths; merged into the source set only when
  // compiling the test binary, never for production builds
  std::vector<std::string> testSunFiles;
  std::string baseDir;  // directory the paths were resolved in
};

/** Resolves manifest dependencies and prepares their compiler inputs. */
class ManifestProcessor {
 public:
  /**
   * The manifest block among a program's top-level statements, or nullptr
   */
  static const sun::ast::ManifestAST* findManifest(
      const sun::ast::BlockExprAST& program);

  /**
   * Resolve a manifest path: absolute as-is, else relative to baseDir, else
   * through the config's sunPath dirs, --lib-path, SUN_PATH and installation
   * directories, else returned unchanged (errors surface later)
   */
  static std::string resolvePath(const std::string& path,
                                 const std::string& baseDir,
                                 const SunConfig* config = nullptr,
                                 const std::string& targetTriple = "");

  /**
   * Define an explicit override for manifest entries (--path-var NAME=DIR).
   * Explicit values take precedence over source configuration files.
   */
  static void setPathVariable(const std::string& name,
                              const std::string& value);

  /** Defines a project or editor fallback, used only when neither an explicit
   * override nor the source's config defines the variable. */
  static void setDefaultPathVariable(const std::string& name,
                                     const std::string& value);

  /** Replaces the consuming project's overrides for the current Git build
   * and returns the previous values so a caller can restore them. */
  static std::map<std::string, std::string> exchangeDependencyPathVariables(
      std::map<std::string, std::string> values);

  /**
   * Drop all defined path variables (used by tests)
   */
  static void clearPathVariables();

  /**
   * Replace every $NAME in a manifest entry with the variable's value —
   * explicit --path-var definitions first, then the consuming project's Git
   * overrides, then the source config's pathVariables,
   * project / language-server defaults, then the environment. Throws SunError for a variable
   * defined nowhere.
   */
  static std::string expandPathVariables(const std::string& input,
                                         const SunConfig* config = nullptr);

  /**
   * Resolve every entry of a manifest against baseDir. `targetTriple` (the
   * --target value, host when empty) selects which of the manifest's
   * `target: { <os>: ... }` blocks contribute their entries — the manifest
   * itself never decides the target, the compilation does.
   */
  static ResolvedManifest process(const sun::ast::ManifestAST& manifest,
                                  const std::string& baseDir,
                                  const std::string& targetTriple = "");

  /**
   * Parse the entrypoint file and resolve its manifest (paths relative to the
   * file's directory). nullopt if the file cannot be read, does not parse, or
   * has no manifest block.
   */
  static std::optional<ResolvedManifest> fromEntrypointFile(
      const std::string& entrypointPath, const std::string& targetTriple = "");
};

}  // namespace sun::driver
