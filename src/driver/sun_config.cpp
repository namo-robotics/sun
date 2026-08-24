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
        // parent search dirs append after the child's.
        for (const auto& [name, value] : config.pathVariables) {
          merged->pathVariables.emplace(name, value);
        }
        merged->sunPath.insert(merged->sunPath.end(), config.sunPath.begin(),
                               config.sunPath.end());
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
    if (name == "sunPath") {
      const llvm::json::Array* dirs = value.getAsArray();
      if (!dirs) {
        logAndThrowError("'sunPath' must be an array of directories in " +
                         file.string());
      }
      for (const auto& dir : *dirs) {
        auto str = dir.getAsString();
        if (!str) {
          logAndThrowError("'sunPath' entries must be strings in " +
                           file.string());
        }
        config.sunPath.push_back(
            anchorAtConfigDir(str->str(), config.configDir));
      }
    } else if (name == "pathVariables") {
      const llvm::json::Object* vars = value.getAsObject();
      if (!vars) {
        logAndThrowError("'pathVariables' must be an object of NAME: dir "
                         "pairs in " +
                         file.string());
      }
      for (const auto& [varName, varValue] : *vars) {
        auto str = varValue.getAsString();
        if (!str) {
          logAndThrowError("path variable '" +
                           llvm::StringRef(varName).str() +
                           "' must be a string in " + file.string());
        }
        config.pathVariables[llvm::StringRef(varName).str()] =
            anchorAtConfigDir(str->str(), config.configDir);
      }
    } else if (name == "root") {
      auto flag = value.getAsBoolean();
      if (!flag) {
        logAndThrowError("'root' must be true or false in " + file.string());
      }
      config.root = *flag;
    } else {
      logAndThrowError("unknown key '" + name + "' in " + file.string() +
                       "; expected 'sunPath', 'pathVariables' or 'root'");
    }
  }

  return config;
}

}  // namespace sun
