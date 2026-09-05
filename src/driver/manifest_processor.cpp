// manifest_processor.cpp — see manifest_processor.h

#include "driver/manifest_processor.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "moon_bundling/moon_cache.h"
#include "parsing/parser.h"
#include "support/error.h"
#include "support/sun_path.h"
#include "support/target_os.h"

namespace sun {

namespace {
std::map<std::string, std::string>& pathVariables() {
  static std::map<std::string, std::string> vars;
  return vars;
}
}  // namespace

void ManifestProcessor::setPathVariable(const std::string& name,
                                        const std::string& value) {
  pathVariables()[name] = value;
}

void ManifestProcessor::clearPathVariables() { pathVariables().clear(); }

std::string ManifestProcessor::expandPathVariables(const std::string& input,
                                                   const SunConfig* config) {
  std::string out;
  size_t i = 0;
  while (i < input.size()) {
    if (input[i] != '$') {
      out += input[i++];
      continue;
    }
    size_t nameStart = i + 1;
    size_t nameEnd = nameStart;
    while (nameEnd < input.size() &&
           (std::isalnum(static_cast<unsigned char>(input[nameEnd])) ||
            input[nameEnd] == '_')) {
      ++nameEnd;
    }
    if (nameEnd == nameStart) {
      logAndThrowError(
          "expected a variable name after '$' in manifest entry '" + input +
          "'");
    }
    std::string name = input.substr(nameStart, nameEnd - nameStart);
    const std::string* value = nullptr;
    if (config) {
      auto it = config->pathVariables.find(name);
      if (it != config->pathVariables.end()) {
        value = &it->second;
      }
    }
    if (!value) {
      auto it = pathVariables().find(name);
      if (it != pathVariables().end()) {
        value = &it->second;
      }
    }
    if (value) {
      out += *value;
    } else if (const char* env = std::getenv(name.c_str())) {
      out += env;
    } else {
      logAndThrowError(
          "undefined path variable '$" + name + "' in manifest entry '" +
          input + "'; define it in sun-config.json, with --path-var " + name +
          "=<dir>, the sun.pathVariables editor setting, or in "
          "the environment");
    }
    i = nameEnd;
  }
  return out;
}

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
                                           const std::string& baseDir,
                                           const SunConfig* config) {
  std::filesystem::path p(path);
  if (p.is_absolute()) {
    return path;
  }
  auto relative = std::filesystem::path(baseDir) / p;
  if (std::filesystem::exists(relative)) {
    return relative.lexically_normal().string();
  }
  if (config) {
    for (const auto& dir : config->sunPath) {
      auto candidate = std::filesystem::path(dir) / p;
      if (std::filesystem::exists(candidate)) {
        return candidate.lexically_normal().string();
      }
    }
  }
  auto resolved = SunPath::resolve(path);
  if (!resolved.empty()) {
    return resolved.string();
  }
  return path;
}

ResolvedManifest ManifestProcessor::process(const ManifestAST& manifest,
                                            const std::string& baseDir,
                                            const std::string& targetTriple) {
  ResolvedManifest out;
  out.baseDir = baseDir;

  // The nearest sun-config.json overrides configuration supplied from
  // outside the folder (--path-var, editor settings, environment).
  auto configOpt = SunConfig::findFrom(baseDir);
  const SunConfig* config = configOpt ? &*configOpt : nullptr;

  auto addSuns = [&](const std::vector<ManifestSunDependency>& suns) {
    for (const auto& sunDep : suns) {
      out.sunFiles.push_back(resolvePath(
          expandPathVariables(sunDep.path, config), baseDir, config));
    }
  };
  auto addMoons = [&](const std::vector<ManifestMoonDependency>& moons) {
    for (const auto& moonDep : moons) {
      std::string resolved =
          moonDep.url
              ? MoonCache::fetch(expandPathVariables(*moonDep.url, config),
                                 moonDep.hash)
                    .string()
              : resolvePath(expandPathVariables(moonDep.path, config), baseDir,
                            config);
      if (moonDep.rename.has_value()) {
        out.moonImports.emplace_back(resolved, moonDep.rename.value(),
                                     moonDep.rename.value());
      } else {
        out.moonImports.emplace_back(resolved);
      }
    }
  };
  auto addProtos = [&](const std::vector<ManifestProtoDependency>& protos) {
    for (const auto& protoDep : protos) {
      out.protoFiles.push_back(resolvePath(
          expandPathVariables(protoDep.path, config), baseDir, config));
    }
  };
  auto addArchives =
      [&](const std::vector<ManifestArchiveDependency>& archives) {
        for (const auto& archiveDep : archives) {
          out.archiveFiles.push_back(resolvePath(
              expandPathVariables(archiveDep.path, config), baseDir, config));
        }
      };

  // Entries from the target block matching the OS being compiled for. The
  // compilation's --target (host when absent) makes the choice, never the
  // manifest itself. Target-specific .sun files come before the shared list:
  // they define the per-OS primitives (constants, errno, socket options)
  // that the shared files consume, and later files may reference earlier
  // ones but not the other way round.
  auto addTestSuns = [&](const std::vector<ManifestSunDependency>& suns) {
    for (const auto& sunDep : suns) {
      out.testSunFiles.push_back(resolvePath(
          expandPathVariables(sunDep.path, config), baseDir, config));
    }
  };

  auto osName = targetOsName(resolvedTargetTriple(targetTriple));
  for (const auto& block : manifest.getTargets()) {
    if (!osName || block.os != *osName) continue;
    addSuns(block.suns);
    addTestSuns(block.testSuns);
  }

  addSuns(manifest.getSuns());
  addTestSuns(manifest.getTestSuns());
  addMoons(manifest.getMoons());
  addProtos(manifest.getProtos());
  addArchives(manifest.getArchives());

  for (const auto& block : manifest.getTargets()) {
    if (!osName || block.os != *osName) continue;
    addMoons(block.moons);
    addProtos(block.protos);
    addArchives(block.archives);
  }

  return out;
}

std::optional<ResolvedManifest> ManifestProcessor::fromEntrypointFile(
    const std::string& entrypointPath, const std::string& targetTriple) {
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
  return process(*manifest, baseDir, targetTriple);
}

}  // namespace sun
