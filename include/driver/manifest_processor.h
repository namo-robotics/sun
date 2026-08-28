// manifest_processor.h — Shared manifest handling for the driver, the
// --emit-moon path and the LSP: locate the manifest block, resolve entry
// paths (relative to the entrypoint's directory, then SUN_PATH), and split
// the entries into .sun files, .moon imports and .proto schemas. Moon
// entries with a url are fetched into the download cache (MoonCache) and
// resolved to the cached file. Entries may reference path variables
// ("$LIBS/util.moon"), defined by the nearest sun-config.json (which wins),
// --path-var / language-server settings, or the environment.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ast/block_expr_ast.h"
#include "ast/manifest_ast.h"
#include "driver/sun_config.h"
#include "moon_bundling/moon_import.h"

namespace sun {

struct ResolvedManifest {
  std::vector<std::string> sunFiles;    // resolved .sun paths (manifest order)
  std::vector<MoonImport> moonImports;  // resolved .moon imports
  std::vector<std::string> protoFiles;  // resolved .proto paths
  std::vector<std::string> archiveFiles;  // resolved native .a archives
  std::string baseDir;  // directory the paths were resolved in
};

class ManifestProcessor {
 public:
  // The manifest block among a program's top-level statements, or nullptr
  static const ManifestAST* findManifest(const BlockExprAST& program);

  // Resolve a manifest path: absolute as-is, else relative to baseDir, else
  // through the config's sunPath dirs, else SUN_PATH, else returned
  // unchanged (errors surface later)
  static std::string resolvePath(const std::string& path,
                                 const std::string& baseDir,
                                 const SunConfig* config = nullptr);

  // Define a path variable for manifest entries (--path-var NAME=DIR)
  static void setPathVariable(const std::string& name,
                              const std::string& value);

  // Drop all defined path variables (used by tests)
  static void clearPathVariables();

  // Replace every $NAME in a manifest entry with the variable's value —
  // the config's pathVariables first, then --path-var / language-server
  // definitions, then the environment. Throws SunError for a variable
  // defined nowhere.
  static std::string expandPathVariables(const std::string& input,
                                         const SunConfig* config = nullptr);

  // Resolve every entry of a manifest against baseDir. `targetTriple` (the
  // --target value, host when empty) selects which of the manifest's
  // `target: { <os>: ... }` blocks contribute their entries — the manifest
  // itself never decides the target, the compilation does.
  static ResolvedManifest process(const ManifestAST& manifest,
                                  const std::string& baseDir,
                                  const std::string& targetTriple = "");

  // Parse the entrypoint file and resolve its manifest (paths relative to the
  // file's directory). nullopt if the file cannot be read, does not parse, or
  // has no manifest block.
  static std::optional<ResolvedManifest> fromEntrypointFile(
      const std::string& entrypointPath, const std::string& targetTriple = "");
};

}  // namespace sun
