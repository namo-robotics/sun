// manifest_processor.cpp — see manifest_processor.h

#include "driver/manifest_processor.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "moon_bundling/moon_cache.h"
#include "parsing/parser.h"
#include "support/sun_path.h"

namespace sun {

const ManifestAST* ManifestProcessor::findManifest(
    const BlockExprAST& program) {
  for (const auto& stmt : program.getBody()) {
    if (stmt && stmt->getType() == ASTNodeType::MANIFEST) {
      return static_cast<const ManifestAST*>(stmt.get());
    }
  }
  return nullptr;
}

std::string ManifestProcessor::resolvePath(const std::string& path,
                                           const std::string& baseDir) {
  std::filesystem::path p(path);
  if (p.is_absolute()) {
    return path;
  }
  auto relative = std::filesystem::path(baseDir) / p;
  if (std::filesystem::exists(relative)) {
    return relative.lexically_normal().string();
  }
  auto resolved = SunPath::resolve(path);
  if (!resolved.empty()) {
    return resolved.string();
  }
  return path;
}

ResolvedManifest ManifestProcessor::process(const ManifestAST& manifest,
                                            const std::string& baseDir) {
  ResolvedManifest out;
  out.baseDir = baseDir;

  for (const auto& sunDep : manifest.getSuns()) {
    out.sunFiles.push_back(resolvePath(sunDep.path, baseDir));
  }

  for (const auto& moonDep : manifest.getMoons()) {
    std::string resolved =
        moonDep.url ? MoonCache::fetch(*moonDep.url, moonDep.hash).string()
                    : resolvePath(moonDep.path, baseDir);
    if (moonDep.rename.has_value()) {
      out.moonImports.emplace_back(resolved, moonDep.rename.value(),
                                   moonDep.rename.value());
    } else {
      out.moonImports.emplace_back(resolved);
    }
  }

  for (const auto& protoDep : manifest.getProtos()) {
    out.protoFiles.push_back(resolvePath(protoDep.path, baseDir));
  }

  return out;
}

std::optional<ResolvedManifest> ManifestProcessor::fromEntrypointFile(
    const std::string& entrypointPath) {
  std::filesystem::path filePath = std::filesystem::absolute(entrypointPath);
  std::string baseDir = filePath.parent_path().string();

  std::ifstream file(entrypointPath);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  auto parser = Parser::createStringParser(source);
  parser.setFilePath(entrypointPath);
  std::unique_ptr<BlockExprAST> ast;
  try {
    ast = parser.parseProgram();
  } catch (...) {
    return std::nullopt;
  }

  const auto* manifest = findManifest(*ast);
  if (!manifest) {
    return std::nullopt;
  }
  return process(*manifest, baseDir);
}

}  // namespace sun
