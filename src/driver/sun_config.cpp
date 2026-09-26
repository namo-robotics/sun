// sun_config.cpp — target-aware project configuration.
#include "driver/sun_config.h"

#include <llvm/Support/JSON.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

#include "driver/git_source.h"
#include "support/error.h"
#include "support/target_os.h"

using sun::support::logAndThrowError;

/** Resolves project configuration without accessing dependency contents. */
namespace sun::driver {

std::string configTargetKey(const std::string& targetTriple) {
  auto triple = sun::support::resolvedTargetTriple(targetTriple);
  triple.setVendor(llvm::Triple::UnknownVendor);
  if (triple.getArch() == llvm::Triple::aarch64) triple.setArchName("aarch64");
  if (triple.getOS() == llvm::Triple::Darwin ||
      triple.getOS() == llvm::Triple::MacOSX)
    triple.setOSName("darwin");
  return triple.str();
}

/** Keeps parsing and validation helpers private. */
namespace {
/** Names the JSON objects used by the configuration grammar. */
using Object = llvm::json::Object;
/** Names a schema check that never resolves paths or downloads files. */
using Check = void (*)(const llvm::json::Value&);

/** Reports a malformed configuration field. */
[[noreturn]] void invalid(const std::string& message) {
  logAndThrowError("sun-config.json: " + message);
}

/** Requires an object and returns its fields. */
const Object& object(const llvm::json::Value& value) {
  auto* result = value.getAsObject();
  if (!result) invalid("expected an object");
  return *result;
}

/** Rejects unknown fields rather than silently ignoring misspellings. */
void keys(const Object& obj, std::initializer_list<std::string> allowed) {
  for (const auto& [key, value] : obj) {
    if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end())
      invalid("unknown key '" + key.str() + "'");
  }
}

/** Requires a nonempty string. */
void stringValue(const llvm::json::Value& value) {
  if (!value.getAsString() || value.getAsString()->empty())
    invalid("expected a nonempty string");
}

/** Requires a Boolean. */
void boolValue(const llvm::json::Value& value) {
  if (!value.getAsBoolean()) invalid("expected a boolean");
}

/** Requires an array of strings. */
void stringsValue(const llvm::json::Value& value) {
  auto* array = value.getAsArray();
  if (!array) invalid("expected an array");
  for (const auto& item : *array) stringValue(item);
}

/** Returns whether a setting uses the default/target wrapper. */
bool wrapped(const llvm::json::Value& value) {
  auto* obj = value.getAsObject();
  return obj && (obj->get("default") || obj->get("target"));
}

/** Validates target names using the same normalization used for selection. */
void targetKeys(const Object& targets) {
  std::set<std::string> seen;
  for (const auto& [key, value] : targets) {
    const auto triple = sun::support::resolvedTargetTriple(key.str());
    if (key.str().empty() || triple.getArch() == llvm::Triple::UnknownArch ||
        triple.getOS() == llvm::Triple::UnknownOS)
      invalid("invalid target triple '" + key.str() + "'");
    if (!seen.insert(configTargetKey(key.str())).second)
      invalid("duplicate target triple");
  }
}

/** Checks every branch and selects a setting without touching the filesystem.
 */
const llvm::json::Value* select(const llvm::json::Value& value,
                                const std::string& target, Check check,
                                bool required = true) {
  if (!wrapped(value)) {
    check(value);
    return &value;
  }
  const auto& obj = object(value);
  keys(obj, {"default", "target"});
  const auto* selected = obj.get("default");
  if (selected) check(*selected);
  if (const auto* branches = obj.get("target")) {
    const auto& targets = object(*branches);
    targetKeys(targets);
    for (const auto& [key, branch] : targets) {
      check(branch);
      if (configTargetKey(key.str()) == target) selected = &branch;
    }
  }
  if (!selected && required)
    invalid("no default or matching value for target " + target);
  return selected;
}

/** Anchors local paths while leaving dependency variables for lazy expansion.
 */
std::string anchor(const std::string& value, const std::filesystem::path& dir) {
  if (value.find('$') != std::string::npos) return value;
  return std::filesystem::absolute(dir / value).lexically_normal().string();
}

/** Requires a portable name safe for cache and output directory components. */
void validName(const std::string& name) {
  if (name.empty() || name == "." || name == "..")
    invalid("invalid name '" + name + "'");
  for (unsigned char c : name)
    if (!std::isalnum(c) && c != '_' && c != '-')
      invalid("invalid name '" + name + "'");
}

/** Checks resource mappings without examining their source files. */
void resourcesValue(const llvm::json::Value& value) {
  const auto* array = value.getAsArray();
  if (!array) invalid("resources must be an array");
  for (const auto& item : *array) {
    const auto& obj = object(item);
    keys(obj, {"path", "destination"});
    for (const auto* key : {"path", "destination"}) {
      if (!obj.get(key))
        invalid(std::string("resource requires '") + key + "'");
      stringValue(*obj.get(key));
    }
  }
}

/** Converts selected resource mappings into config-relative inputs. */
std::vector<ConfigResource> resources(const llvm::json::Value& value,
                                      const std::filesystem::path& dir) {
  std::vector<ConfigResource> result;
  for (const auto& item : *value.getAsArray()) {
    const auto& obj = object(item);
    result.push_back({anchor(obj.getString("path")->str(), dir),
                      obj.getString("destination")->str()});
  }
  return result;
}

/** Requires one of the supported production artifact kinds. */
void entrypointTypeValue(const llvm::json::Value& value) {
  stringValue(value);
  if (*value.getAsString() != "binary" && *value.getAsString() != "library")
    invalid("entrypoint type must be 'binary' or 'library'");
}

/** Checks and selects fields, resolving enabled before other values. */
Object fields(const Object& obj, const std::string& target, bool package) {
  keys(obj, package
                ? std::initializer_list<std::string>{"name", "enabled",
                                                     "entrypoints", "resources",
                                                     "output_name"}
                : std::initializer_list<std::string>{
                      "name", "enabled", "path", "git", "version", "type",
                      "output_name", "test_binary_name", "resources"});
  bool enabled = true;
  if (auto* value = obj.get("enabled"))
    enabled = *select(*value, target, boolValue)->getAsBoolean();
  Object result;
  result["enabled"] = enabled;
  for (const auto& [key, value] : obj) {
    const auto name = key.str();
    if (name == "enabled") continue;
    if (name == "name") {
      stringValue(value);
      validName(value.getAsString()->str());
      result[name] = value;
      continue;
    }
    Check check = stringValue;
    if (name == "resources") check = resourcesValue;
    if (name == "entrypoints") check = stringsValue;
    if (name == "type") check = entrypointTypeValue;
    const auto* selected = select(value, target, check, enabled);
    if (enabled && selected) result[name] = *selected;
  }
  if (package && !result.getString("name")) invalid("package requires name");
  if (enabled) {
    if (package && !result.getArray("entrypoints"))
      invalid("package requires entrypoints");
    if (!package && !result.getString("path"))
      invalid("entrypoint requires path");
  }
  return result;
}

/** Validates complete source descriptors, keeping hashes with their URLs. */
void dependencyValue(const llvm::json::Value& value) {
  const auto& obj = object(value);
  keys(obj, {"moon", "package"});
  if (bool(obj.get("moon")) == bool(obj.get("package")))
    invalid("dependency requires exactly one of moon or package");
  const bool package = bool(obj.get("package"));
  const auto& source = object(*obj.get(package ? "package" : "moon"));
  keys(source, package
                   ? std::initializer_list<std::string>{"url", "path", "hash"}
                   : std::initializer_list<std::string>{"url", "path", "hash",
                                                        "filename"});
  if (bool(source.get("url")) == bool(source.get("path")))
    invalid("dependency requires exactly one of url or path");
  for (const auto& [key, v] : source) stringValue(v);
  if (source.get("url") && !source.get("hash"))
    invalid("remote dependency requires hash");
  if (auto hash = source.getString("hash")) {
    if (hash->size() != 64 ||
        hash->find_first_not_of("0123456789abcdef") != llvm::StringRef::npos)
      invalid("hash must be a lowercase SHA-256 digest");
  }
  if (!package) {
    auto filename = source.getString("filename");
    if (!filename ||
        std::filesystem::path(filename->str()).filename() != filename->str() ||
        std::filesystem::path(filename->str()).extension() != ".moon" ||
        filename->contains('\\'))
      invalid("moon filename must be a simple .moon filename");
  }
}

/** Detects wrapper settings inside a legacy-replaced collection. */
bool containsWrapper(const llvm::json::Value& value) {
  if (wrapped(value)) return true;
  if (auto* obj = value.getAsObject())
    for (const auto& [key, v] : *obj)
      if (containsWrapper(v)) return true;
  if (auto* arr = value.getAsArray())
    for (const auto& v : *arr)
      if (containsWrapper(v)) return true;
  return false;
}

/** Converts a validated, target-selected root object into build settings. */
SunConfig parse(const Object& root, const std::filesystem::path& file,
                const std::string& target) {
  keys(root, {"root", "sun_path", "path_variables", "entrypoints", "packages",
              "dependencies", "target"});
  SunConfig config;
  config.configDir = std::filesystem::weakly_canonical(
      std::filesystem::absolute(file).parent_path());
  config.targetTriple = target;
  if (auto* value = root.get("root")) {
    boolValue(*value);
    config.root = *value->getAsBoolean();
  }
  if (auto* value = root.get("sun_path"))
    for (const auto& path : *select(*value, target, stringsValue)->getAsArray())
      config.sunPath.push_back(
          anchor(path.getAsString()->str(), config.configDir));
  if (auto* vars = root.get("path_variables"))
    for (const auto& [key, value] : object(*vars))
      config.pathVariables[key.str()] =
          anchor(select(value, target, stringValue)->getAsString()->str(),
                 config.configDir);
  if (auto* deps = root.get("dependencies")) {
    for (const auto& [key, value] : object(*deps)) {
      validName(key.str());
      for (unsigned char c : key.str())
        if (!std::isalnum(c) && c != '_')
          invalid("dependency name must be a path variable name");
      if (config.pathVariables.count(key.str()))
        invalid("dependency and path variable collide: " + key.str());
      ConfigDependency dep;
      auto* selected = select(value, target, dependencyValue, false);
      dep.available = selected != nullptr;
      if (selected) {
        const auto& obj = object(*selected);
        dep.package = bool(obj.get("package"));
        const auto& source = object(*obj.get(dep.package ? "package" : "moon"));
        if (auto v = source.getString("url")) dep.url = v->str();
        if (auto v = source.getString("path"))
          dep.path = anchor(v->str(), config.configDir);
        if (auto v = source.getString("hash")) dep.hash = v->str();
        if (auto v = source.getString("filename")) dep.filename = v->str();
      }
      config.dependencies[key.str()] = std::move(dep);
    }
  }
  std::set<std::string> names;
  if (auto* value = root.get("entrypoints")) {
    auto* array = value->getAsArray();
    if (!array) invalid("entrypoints must be an array");
    for (const auto& item : *array) {
      auto obj = fields(object(item), target, false);
      ConfigEntrypoint entry;
      if (auto name = obj.getString("name")) {
        entry.name = name->str();
        if (!names.insert(entry.name).second)
          invalid("duplicate entrypoint name " + entry.name);
      }
      if (!*obj.getBoolean("enabled")) continue;
      entry.path = obj.getString("path")->str();
      if (auto v = obj.getString("git")) entry.git = v->str();
      if (auto v = obj.getString("version")) entry.version = v->str();
      if (auto v = obj.getString("type"))
        entry.type = *v == "library" ? ConfigEntrypoint::Type::Library
                                     : ConfigEntrypoint::Type::Binary;
      if (auto v = obj.getString("output_name"))
        entry.outputName = anchor(v->str(), config.configDir);
      if (auto v = obj.getString("test_binary_name"))
        entry.testBinaryName = anchor(v->str(), config.configDir);
      if (auto* v = obj.get("resources"))
        entry.resources = resources(*v, config.configDir);
      if (!entry.resources.empty() && entry.name.empty())
        invalid("entrypoints with resources require a name");
      if (!entry.git.empty()) {
        validateGitSource(entry);
        if (entry.outputName.empty())
          entry.outputName =
              (config.configDir / std::filesystem::path(entry.path).stem())
                  .string();
      } else {
        if (!entry.version.empty()) invalid("entrypoint version requires git");
        entry.path = anchor(entry.path, config.configDir);
      }
      config.entrypoints.push_back(std::move(entry));
    }
  }
  std::set<std::string> packageNames;
  if (auto* value = root.get("packages")) {
    auto* array = value->getAsArray();
    if (!array) invalid("packages must be an array");
    for (const auto& item : *array) {
      auto obj = fields(object(item), target, true);
      ConfigPackage pkg;
      pkg.name = obj.getString("name")->str();
      if (!packageNames.insert(pkg.name).second)
        invalid("duplicate package name " + pkg.name);
      if (!*obj.getBoolean("enabled")) continue;
      for (const auto& v : *obj.getArray("entrypoints")) {
        const auto name = v.getAsString()->str();
        if (!names.count(name)) invalid("unknown package entrypoint " + name);
        pkg.entrypoints.push_back(name);
      }
      if (auto* v = obj.get("resources"))
        pkg.resources = resources(*v, config.configDir);
      if (auto v = obj.getString("output_name"))
        pkg.outputName = anchor(v->str(), config.configDir);
      config.packages.push_back(std::move(pkg));
    }
  }
  return config;
}
}  // namespace

std::optional<SunConfig> SunConfig::findFrom(
    const std::filesystem::path& startDir, const std::string& targetTriple) {
  std::error_code ec;
  auto dir = std::filesystem::weakly_canonical(startDir, ec);
  if (ec) dir = startDir;
  std::optional<SunConfig> merged;
  while (true) {
    auto candidate = dir / kFileName;
    if (std::filesystem::exists(candidate)) {
      auto config = loadFile(candidate, targetTriple);
      const bool stop = config.root;
      if (!merged)
        merged = std::move(config);
      else {
        for (const auto& [name, value] : config.pathVariables)
          merged->pathVariables.emplace(name, value);
        for (const auto& [name, value] : config.dependencies)
          merged->dependencies.emplace(name, value);
        merged->sunPath.insert(merged->sunPath.end(), config.sunPath.begin(),
                               config.sunPath.end());
        merged->entrypoints.insert(merged->entrypoints.end(),
                                   config.entrypoints.begin(),
                                   config.entrypoints.end());
      }
      for (const auto& [name, value] : merged->dependencies)
        if (merged->pathVariables.count(name))
          invalid("dependency and path variable collide: " + name);
      if (stop) break;
    }
    if (std::filesystem::is_directory(dir / ".sun-git-source")) break;
    auto parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return merged;
}

SunConfig SunConfig::loadFile(const std::filesystem::path& file,
                              const std::string& targetTriple) {
  std::ifstream in(file);
  if (!in) invalid("could not read " + file.string());
  std::stringstream buffer;
  buffer << in.rdbuf();
  auto parsed = llvm::json::parse(buffer.str());
  if (!parsed)
    invalid("malformed JSON in " + file.string() + ": " +
            llvm::toString(parsed.takeError()));
  auto root = object(*parsed);
  const auto target = configTargetKey(targetTriple);
  if (auto* value = root.get("target")) {
    const auto& targets = object(*value);
    targetKeys(targets);
    const Object* selected = nullptr;
    for (const auto& [key, branch] : targets) {
      const auto& settings = object(branch);
      keys(settings, {"sun_path", "path_variables", "entrypoints", "packages"});
      for (const auto& [field, setting] : settings) {
        if (containsWrapper(setting))
          invalid("legacy target blocks cannot contain target wrappers");
        if (const auto* base = root.get(field)) {
          if (field == "path_variables") {
            for (const auto& [var, v] : object(setting)) {
              if (auto* original = object(*base).get(var);
                  original && wrapped(*original))
                invalid("overlapping legacy and per-setting target overrides");
            }
          } else if (containsWrapper(*base))
            invalid("overlapping legacy and per-setting target overrides");
        }
      }
      // Legacy collections are complete values even when their target is
      // inactive.
      for (const auto* field : {"entrypoints", "packages"}) {
        if (const auto* value = settings.get(field)) {
          const auto* entries = value->getAsArray();
          if (!entries) invalid(std::string(field) + " must be an array");
          for (const auto& entry : *entries) {
            const bool package = std::string(field) == "packages";
            fields(object(entry), target, package);
          }
        }
      }
      if (auto* paths = settings.get("sun_path")) stringsValue(*paths);
      if (auto* vars = settings.get("path_variables"))
        for (const auto& [name, v] : object(*vars)) stringValue(v);
      if (configTargetKey(key.str()) == target) selected = &settings;
    }
    if (selected) {
      Object copy = *selected;
      for (const auto& [field, value] : copy) {
        if (field == "path_variables" && root.get("path_variables")) {
          Object vars = object(*root.get("path_variables"));
          for (const auto& [name, v] : object(value)) vars[name] = v;
          root[field] = std::move(vars);
        } else
          root[field] = value;
      }
    }
    root.erase("target");
  }
  return parse(root, file, target);
}
}  // namespace sun::driver
