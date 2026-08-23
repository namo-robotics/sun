// manifest_processor.h — Shared manifest handling for the driver, the
// --emit-moon path and the LSP: locate the manifest block, resolve entry
// paths (relative to the entrypoint's directory, then SUN_PATH), and split
// the entries into .sun files, .moon imports and .proto schemas. Moon
// entries with a url are fetched into the download cache (MoonCache) and
// resolved to the cached file.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ast/block_expr_ast.h"
#include "ast/manifest_ast.h"
#include "moon_bundling/moon_import.h"

namespace sun {

struct ResolvedManifest {
  std::vector<std::string> sunFiles;    // resolved .sun paths (manifest order)
  std::vector<MoonImport> moonImports;  // resolved .moon imports
  std::vector<std::string> protoFiles;  // resolved .proto paths
  std::string baseDir;                  // directory the paths were resolved in
};

class ManifestProcessor {
 public:
  // The manifest block among a program's top-level statements, or nullptr
  static const ManifestAST* findManifest(const BlockExprAST& program);

  // Resolve a manifest path: absolute as-is, else relative to baseDir, else
  // through SUN_PATH, else returned unchanged (errors surface later)
  static std::string resolvePath(const std::string& path,
                                 const std::string& baseDir);

  // Resolve every entry of a manifest against baseDir
  static ResolvedManifest process(const ManifestAST& manifest,
                                  const std::string& baseDir);

  // Parse the entrypoint file and resolve its manifest (paths relative to the
  // file's directory). nullopt if the file cannot be read, does not parse, or
  // has no manifest block.
  static std::optional<ResolvedManifest> fromEntrypointFile(
      const std::string& entrypointPath);
};

}  // namespace sun
