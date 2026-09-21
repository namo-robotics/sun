// sun_config.cpp — see sun_config.h

#include "driver/sun_config.h"

#include <llvm/Support/JSON.h>

#include <fstream>
#include <set>
#include <sstream>

#include "support/error.h"
#include "support/target_os.h"

using sun::support::logAndThrowError;

/** Coordinates compilation, dependency loading, linking, and program execution.
 */
namespace sun::driver {

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/**
 * Relative config entries are anchored at the config file's folder, so a
 * committed sun-config.json works from any working directory.
 */
std::string anchorAtConfigDir(const std::string& value,
                              const std::filesystem::path& configDir) {
  std::filesystem::path p(value);
  if (p.is_absolute()) {
    return value;
  }
  return (configDir / p).lexically_normal().string();
}

/**
 * One entry of the entrypoints array: an object naming the entrypoint file
 * and, optionally, what kind of artifact it is and what its outputs are
 * called. Every path-like value is anchored at the config's folder.
 */
ConfigEntrypoint parseEntrypoint(const llvm::json::Value& value,
                                 const std::filesystem::path& configDir,
                                 const std::filesystem::path& file) {
  const llvm::json::Object* object = value.getAsObject();
  if (!object) {
    logAndThrowError("'entrypoints' entries must be objects in " +
                     file.string());
  }

  ConfigEntrypoint entrypoint;
  for (const auto& [key, entryValue] : *object) {
    std::string name = llvm::StringRef(key).str();
    auto str = entryValue.getAsString();
    if (!str) {
      logAndThrowError("entrypoint key '" + name + "' must be a string in " +
                       file.string());
    }
    if (name == "path") {
      entrypoint.path = anchorAtConfigDir(str->str(), configDir);
    } else if (name == "type") {
      if (*str == "binary") {
        entrypoint.type = ConfigEntrypoint::Type::Binary;
      } else if (*str == "library") {
        entrypoint.type = ConfigEntrypoint::Type::Library;
      } else {
        logAndThrowError("entrypoint type '" + str->str() + "' in " +
                         file.string() + "; expected 'binary' or 'library'");
      }
    } else if (name == "output_name") {
      entrypoint.outputName = anchorAtConfigDir(str->str(), configDir);
    } else if (name == "test_binary_name") {
      entrypoint.testBinaryName = anchorAtConfigDir(str->str(), configDir);
    } else {
      logAndThrowError("unknown entrypoint key '" + name + "' in " +
                       file.string() +
                       "; expected 'path', 'type', 'output_name' or "
                       "'test_binary_name'");
    }
  }
  if (entrypoint.path.empty()) {
    logAndThrowError("an entrypoints entry is missing 'path' in " +
                     file.string());
  }
  return entrypoint;
}

/**
 * Match platform spellings without depending on vendor or macOS version.
 */
std::string configTargetKey(llvm::Triple triple) {
  triple.setVendor(llvm::Triple::UnknownVendor);
  if (triple.getArch() == llvm::Triple::aarch64) triple.setArchName("aarch64");
  if (triple.getOS() == llvm::Triple::Darwin ||
      triple.getOS() == llvm::Triple::MacOSX)
    triple.setOSName("darwin");
  return triple.str();
}

/**
 * Validate every target block, then select the requested target or the host.
 */
const llvm::json::Object* targetSettings(const llvm::json::Object& owner,
                                         const std::string& targetTriple,
                                         const std::filesystem::path& file) {
  const auto* value = owner.get("target");
  if (!value) return nullptr;
  const auto* targets = value->getAsObject();
  if (!targets)
    logAndThrowError("'target' must be an object in " + file.string());
  const auto selectedKey =
      configTargetKey(sun::support::resolvedTargetTriple(targetTriple));
  std::set<std::string> seen;
  const llvm::json::Object* selected = nullptr;
  for (const auto& [name, settings] : *targets) {
    const std::string key = llvm::StringRef(name).str();
    const auto triple = sun::support::resolvedTargetTriple(key);
    if (key.empty() || triple.getArch() == llvm::Triple::UnknownArch ||
        triple.getOS() == llvm::Triple::UnknownOS) {
      logAndThrowError("invalid target triple '" + llvm::StringRef(name).str() +
                       "' in " + file.string());
    }
    if (!seen.insert(configTargetKey(triple)).second)
      logAndThrowError("duplicate target triple in " + file.string());
    const auto* object = settings.getAsObject();
    if (!object)
      logAndThrowError("target settings must be an object in " + file.string());
    for (const auto& [key, setting] : *object) {
      const std::string field = llvm::StringRef(key).str();
      if (field == "sun_path") {
        const auto* paths = setting.getAsArray();
        if (!paths)
          logAndThrowError("target sun_path must be an array in " +
                           file.string());
        for (const auto& path : *paths)
          if (!path.getAsString())
            logAndThrowError("target sun_path entries must be strings in " +
                             file.string());
      } else if (field == "path_variables") {
        const auto* vars = setting.getAsObject();
        if (!vars)
          logAndThrowError("target path_variables must be an object in " +
                           file.string());
        for (const auto& [var, path] : *vars)
          if (!path.getAsString())
            logAndThrowError("target path variables must be strings in " +
                             file.string());
      } else if (field == "entrypoints") {
        const auto* entries = setting.getAsArray();
        if (!entries)
          logAndThrowError("target entrypoints must be an array in " +
                           file.string());
        for (const auto& entry : *entries)
          parseEntrypoint(entry, file.parent_path(), file);
      } else {
        logAndThrowError("unknown target setting '" + field + "' in " +
                         file.string());
      }
    }
    if (configTargetKey(triple) == selectedKey) selected = object;
  }
  return selected;
}

}  // namespace

std::optional<SunConfig> SunConfig::findFrom(
    const std::filesystem::path& startDir, const std::string& targetTriple) {
  std::error_code ec;
  auto dir = std::filesystem::weakly_canonical(startDir, ec);
  if (ec) {
    dir = startDir;
  }
  std::optional<SunConfig> merged;
  while (true) {
    auto candidate = dir / kFileName;
    if (std::filesystem::exists(candidate)) {
      SunConfig config = loadFile(candidate, targetTriple);
      bool stop = config.root;
      if (!merged) {
        merged = std::move(config);
      } else {
        // Nearer definitions win: emplace keeps an existing variable, and
        // parent search dirs and entrypoints append after the child's.
        for (const auto& [name, value] : config.pathVariables) {
          merged->pathVariables.emplace(name, value);
        }
        merged->sunPath.insert(merged->sunPath.end(), config.sunPath.begin(),
                               config.sunPath.end());
        merged->entrypoints.insert(merged->entrypoints.end(),
                                   config.entrypoints.begin(),
                                   config.entrypoints.end());
      }
      if (stop) {
        break;
      }
    }
    auto parent = dir.parent_path();
    if (parent == dir) {
      break;
    }
    dir = parent;
  }
  return merged;
}

SunConfig SunConfig::loadFile(const std::filesystem::path& file,
                              const std::string& targetTriple) {
  std::ifstream in(file);
  if (!in.is_open()) {
    logAndThrowError("could not read " + file.string());
  }
  std::stringstream buffer;
  buffer << in.rdbuf();

  auto parsed = llvm::json::parse(buffer.str());
  if (!parsed) {
    logAndThrowError("malformed JSON in " + file.string() + ": " +
                     llvm::toString(parsed.takeError()));
  }
  const llvm::json::Object* root = parsed->getAsObject();
  if (!root) {
    logAndThrowError("expected a JSON object in " + file.string());
  }

  const auto* settings = targetSettings(*root, targetTriple, file);

  SunConfig config;
  config.configDir = file.parent_path();

  for (const auto& [key, value] : *root) {
    std::string name = llvm::StringRef(key).str();
    if (name == "sun_path") {
      const llvm::json::Array* dirs = value.getAsArray();
      if (!dirs) {
        logAndThrowError("'sun_path' must be an array of directories in " +
                         file.string());
      }
      for (const auto& dir : *dirs) {
        auto str = dir.getAsString();
        if (!str) {
          logAndThrowError("'sun_path' entries must be strings in " +
                           file.string());
        }
        config.sunPath.push_back(
            anchorAtConfigDir(str->str(), config.configDir));
      }
    } else if (name == "path_variables") {
      const llvm::json::Object* vars = value.getAsObject();
      if (!vars) {
        logAndThrowError(
            "'path_variables' must be an object of NAME: dir "
            "pairs in " +
            file.string());
      }
      for (const auto& [varName, varValue] : *vars) {
        auto str = varValue.getAsString();
        if (!str) {
          logAndThrowError("path variable '" + llvm::StringRef(varName).str() +
                           "' must be a string in " + file.string());
        }
        config.pathVariables[llvm::StringRef(varName).str()] =
            anchorAtConfigDir(str->str(), config.configDir);
      }
    } else if (name == "entrypoints") {
      const llvm::json::Array* entries = value.getAsArray();
      if (!entries) {
        logAndThrowError("'entrypoints' must be an array of objects in " +
                         file.string());
      }
      for (const auto& entry : *entries) {
        config.entrypoints.push_back(
            parseEntrypoint(entry, config.configDir, file));
      }
    } else if (name == "target") {
      // Target settings were validated above and are applied after defaults.
      continue;
    } else if (name == "root") {
      auto flag = value.getAsBoolean();
      if (!flag) {
        logAndThrowError("'root' must be true or false in " + file.string());
      }
      config.root = *flag;
    } else {
      logAndThrowError("unknown key '" + name + "' in " + file.string() +
                       "; expected 'sun_path', 'path_variables', "
                       "'entrypoints', 'target' or 'root'");
    }
  }

  if (settings) {
    if (const auto* entries = settings->getArray("entrypoints")) {
      config.entrypoints.clear();
      for (const auto& entry : *entries)
        config.entrypoints.push_back(
            parseEntrypoint(entry, config.configDir, file));
    }
    if (const auto* paths = settings->getArray("sun_path")) {
      config.sunPath.clear();
      for (const auto& path : *paths)
        config.sunPath.push_back(
            anchorAtConfigDir(path.getAsString()->str(), config.configDir));
    }
    if (const auto* vars = settings->getObject("path_variables")) {
      for (const auto& [name, path] : *vars)
        config.pathVariables[llvm::StringRef(name).str()] =
            anchorAtConfigDir(path.getAsString()->str(), config.configDir);
    }
  }
  return config;
}

}  // namespace sun::driver
