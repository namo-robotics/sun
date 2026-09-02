// sun_config.cpp — see sun_config.h

#include "driver/sun_config.h"

#include <llvm/Support/JSON.h>

#include <fstream>
#include <sstream>

#include "support/error.h"

namespace sun {

namespace {

// Relative config entries are anchored at the config file's folder, so a
// committed sun-config.json works from any working directory.
std::string anchorAtConfigDir(const std::string& value,
                              const std::filesystem::path& configDir) {
  std::filesystem::path p(value);
  if (p.is_absolute()) {
    return value;
  }
  return (configDir / p).lexically_normal().string();
}

// One entry of the entrypoints array: an object naming the entrypoint file
// and, optionally, what kind of artifact it is and what its outputs are
// called. Every path-like value is anchored at the config's folder.
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

}  // namespace

std::optional<SunConfig> SunConfig::findFrom(
    const std::filesystem::path& startDir) {
  std::error_code ec;
  auto dir = std::filesystem::weakly_canonical(startDir, ec);
  if (ec) {
    dir = startDir;
  }
  std::optional<SunConfig> merged;
  while (true) {
    auto candidate = dir / kFileName;
    if (std::filesystem::exists(candidate)) {
      SunConfig config = loadFile(candidate);
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

SunConfig SunConfig::loadFile(const std::filesystem::path& file) {
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
    } else if (name == "root") {
      auto flag = value.getAsBoolean();
      if (!flag) {
        logAndThrowError("'root' must be true or false in " + file.string());
      }
      config.root = *flag;
    } else {
      logAndThrowError("unknown key '" + name + "' in " + file.string() +
                       "; expected 'sun_path', 'path_variables', "
                       "'entrypoints' or 'root'");
    }
  }

  return config;
}

}  // namespace sun
