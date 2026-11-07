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
  if (triple.getArch() == llvm::Triple::aarch64) {
    triple.setArchName("aarch64");
  }
  if (triple.getOS() == llvm::Triple::Darwin ||
      triple.getOS() == llvm::Triple::MacOSX) {
    triple.setOSName("darwin");
  }
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
  if (!result) {
    invalid("expected an object");
  }
  return *result;
}

/** Rejects unknown fields rather than silently ignoring misspellings. */
void keys(const Object& obj, std::initializer_list<std::string> allowed) {
  for (const auto& [key, value] : obj) {
    if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
      invalid("unknown key '" + key.str() + "'");
    }
  }
}

/** Requires a nonempty string. */
void stringValue(const llvm::json::Value& value) {
  if (!value.getAsString() || value.getAsString()->empty()) {
    invalid("expected a nonempty string");
  }
}

/** Requires a Boolean. */
void boolValue(const llvm::json::Value& value) {
  if (!value.getAsBoolean()) {
    invalid("expected a boolean");
  }
}

/** Requires an array of strings. */
void stringsValue(const llvm::json::Value& value) {
  auto* array = value.getAsArray();
  if (!array) {
    invalid("expected an array");
  }
  for (const auto& item : *array) {
    stringValue(item);
  }
}

/** Checks the declarations and compiler options for a generated C module. */
void cBindingValue(const llvm::json::Value& value) {
  const auto& obj = object(value);
  keys(obj, {"header", "output", "allow", "clang_args", "skip_if_unavailable"});
  if (!obj.get("header") || !obj.get("output") || !obj.get("allow")) {
    invalid("c_bindings entries require header, output and allow");
  }
  stringValue(*obj.get("header"));
  stringValue(*obj.get("output"));
  if (obj.getString("output")->find('$') == llvm::StringRef::npos &&
      std::filesystem::path(obj.getString("output")->str()).extension() !=
          ".sun") {
    invalid("c_bindings output must name a .sun file");
  }
  stringsValue(*obj.get("allow"));
  if (obj.getArray("allow")->empty()) {
    invalid("c_bindings allow must not be empty");
  }
  if (auto* args = obj.get("clang_args")) {
    stringsValue(*args);
  }
  if (auto* skip = obj.get("skip_if_unavailable")) {
    boolValue(*skip);
  }
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
        triple.getOS() == llvm::Triple::UnknownOS) {
      invalid("invalid target triple '" + key.str() + "'");
    }
    if (!seen.insert(configTargetKey(key.str())).second) {
      invalid("duplicate target triple");
    }
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
  if (selected) {
    check(*selected);
  }
  if (const auto* branches = obj.get("target")) {
    const auto& targets = object(*branches);
    targetKeys(targets);
    for (const auto& [key, branch] : targets) {
      check(branch);
      if (configTargetKey(key.str()) == target) {
        selected = &branch;
      }
    }
  }
  if (!selected && required) {
    invalid("no default or matching value for target " + target);
  }
  return selected;
}

/** Anchors local paths while leaving dependency variables for lazy expansion.
 */
std::string anchor(const std::string& value, const std::filesystem::path& dir) {
  if (value.find('$') != std::string::npos) {
    return value;
  }
  return std::filesystem::absolute(dir / value).lexically_normal().string();
}

/** Requires a portable name safe for cache and output directory components. */
void validName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") {
    invalid("invalid name '" + name + "'");
  }
  for (unsigned char c : name) {
    if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') {
      invalid("invalid name '" + name + "'");
    }
  }
}

/** Checks resource mappings without examining their source files. */
void resourcesValue(const llvm::json::Value& value) {
  const auto* array = value.getAsArray();
  if (!array) {
    invalid("resources must be an array");
  }
  for (const auto& item : *array) {
    const auto& obj = object(item);
    keys(obj, {"path", "destination"});
    for (const auto* key : {"path", "destination"}) {
      if (!obj.get(key)) {
        invalid(std::string("resource requires '") + key + "'");
      }
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
  if (*value.getAsString() != "binary" && *value.getAsString() != "library") {
    invalid("entrypoint type must be 'binary' or 'library'");
  }
}

/** Checks and selects fields, resolving enabled before other values. */
Object fields(const Object& obj, const std::string& target, bool package) {
  keys(obj, package
                ? std::initializer_list<std::string>{"name", "enabled",
                                                     "entrypoints", "resources",
                                                     "output_name"}
                : std::initializer_list<std::string>{
                      "name", "enabled", "path", "git", "version", "type",
                      "export", "steps", "build", "compile_args", "requires",
                      "resources", "pkg_config", "link_args"});
  bool enabled = true;
  if (auto* value = obj.get("enabled")) {
    enabled = *select(*value, target, boolValue)->getAsBoolean();
  }
  Object result;
  result["enabled"] = enabled;
  for (const auto& [key, value] : obj) {
    const auto name = key.str();
    if (name == "enabled") {
      continue;
    }
    if (name == "name") {
      stringValue(value);
      validName(value.getAsString()->str());
      result[name] = value;
      continue;
    }
    Check check = stringValue;
    if (name == "resources") {
      check = resourcesValue;
    }
    if (name == "entrypoints") {
      check = stringsValue;
    }
    if (name == "export") {
      check = boolValue;
    }
    if (name == "build" || name == "steps" || name == "compile_args" ||
        name == "requires" || name == "pkg_config" || name == "link_args") {
      check = stringsValue;
    }
    if (name == "type") {
      check = entrypointTypeValue;
    }
    const auto* selected = select(value, target, check, enabled);
    if (enabled && selected) {
      result[name] = *selected;
    }
  }
  if (package && !result.getString("name")) {
    invalid("package requires name");
  }
  if (enabled) {
    if (package && !result.getArray("entrypoints")) {
      invalid("package requires entrypoints");
    }
    if (!package && !result.getString("path")) {
      invalid("entrypoint requires path");
    }
  }
  return result;
}

/** Requires a portable package or product identity, including namespace
 * components. */
void identityValue(const llvm::json::Value& value) {
  stringValue(value);
  std::istringstream parts(value.getAsString()->str());
  std::string part;
  while (std::getline(parts, part, '/')) {
    validName(part);
  }
  if (value.getAsString()->ends_with("/")) {
    invalid("identity must not end with '/'");
  }
}

/** Requires a nonempty list of distinct entrypoint names. */
void entrypointNamesValue(const llvm::json::Value& value) {
  stringsValue(value);
  const auto& entries = *value.getAsArray();
  if (entries.empty()) {
    invalid("dependency entrypoints must not be empty");
  }
  std::set<std::string> names;
  for (const auto& entry : entries) {
    auto name = entry.getAsString()->str();
    if (name.empty() || !names.insert(name).second) {
      invalid("dependency entrypoints must be nonempty and unique");
    }
  }
}

/** Validates an artifact selection and its external fallback without
 * downloading. */
void dependencyValue(const llvm::json::Value& value) {
  const auto& obj = object(value);
  if (obj.get("packages")) {
    keys(obj, {"git", "version", "config", "packages"});
    if (!obj.get("git")) {
      invalid("dependency packages require git");
    }
    ConfigEntrypoint source;
    source.type = ConfigEntrypoint::Type::Library;
    stringValue(*obj.get("git"));
    source.git = obj.getString("git")->str();
    for (const auto* key : {"version", "config"}) {
      if (auto* value = obj.get(key)) {
        stringValue(*value);
      }
    }
    source.version = obj.getString("version").value_or("").str();
    source.path = obj.getString("config").value_or("sun-config.json").str();
    validateGitSource(source);
    const auto& packages = object(*obj.get("packages"));
    if (packages.empty()) {
      invalid("dependency packages must not be empty");
    }
    for (const auto& [id, value] : packages) {
      identityValue(llvm::json::Value(id.str()));
      const auto& selection = object(value);
      keys(selection, {"entrypoints"});
      if (!selection.get("entrypoints")) {
        invalid("package selection requires entrypoints");
      }
      entrypointNamesValue(*selection.get("entrypoints"));
    }
    return;
  }
  if (obj.get("git")) {
    if (!obj.get("path")) {
      invalid("configured Git dependencies require a packages dictionary");
    }
    keys(obj, {"package", "git", "version", "path"});
    if (!obj.get("package")) {
      invalid("source dependency requires package identity");
    }
    identityValue(*obj.get("package"));
    for (const auto& [key, value] : obj) {
      stringValue(value);
    }
    ConfigEntrypoint source;
    source.type = ConfigEntrypoint::Type::Library;
    source.git = obj.getString("git")->str();
    source.version = obj.getString("version").value_or("").str();
    source.path = obj.getString("path")->str();
    validateGitSource(source);
    return;
  }
  keys(obj,
       {"package", "library", "binary", "url", "path", "hash", "entrypoints"});
  if (!obj.get("package")) {
    invalid("dependency requires package identity");
  }
  identityValue(*obj.get("package"));
  if (obj.get("library") && obj.get("binary")) {
    invalid("dependency cannot select both library and binary");
  }
  for (const auto* key : {"library", "binary"}) {
    if (auto* value = obj.get(key)) {
      identityValue(*value);
    }
  }
  for (const auto& [key, value] : obj) {
    if (key == "entrypoints") {
      entrypointNamesValue(value);
    } else {
      stringValue(value);
    }
  }
  if (!obj.get("url") && !obj.get("path") && obj.get("entrypoints")) {
    return;
  }
  if (bool(obj.get("url")) == bool(obj.get("path"))) {
    invalid("dependency requires exactly one of url or path");
  }
  if (auto hash = obj.getString("hash")) {
    if (hash->size() != 64 ||
        hash->find_first_not_of("0123456789abcdef") != llvm::StringRef::npos) {
      invalid("hash must be a lowercase SHA-256 digest");
    }
  }
}

/** Detects wrapper settings inside a legacy-replaced collection. */
bool containsWrapper(const llvm::json::Value& value) {
  if (wrapped(value)) {
    return true;
  }
  if (auto* obj = value.getAsObject()) {
    for (const auto& [key, v] : *obj) {
      if (containsWrapper(v)) {
        return true;
      }
    }
  }
  if (auto* arr = value.getAsArray()) {
    for (const auto& v : *arr) {
      if (containsWrapper(v)) {
        return true;
      }
    }
  }
  return false;
}

/** Converts a validated, target-selected root object into build settings. */
SunConfig parse(const Object& root, const std::filesystem::path& file,
                const std::string& target) {
  keys(root, {"package_id", "root", "sun_path", "path_variables", "entrypoints",
              "packages", "dependencies", "target", "c_bindings", "sun_fmt",
              "steps", "exports", "projects"});
  SunConfig config;
  config.configDir = std::filesystem::weakly_canonical(
      std::filesystem::absolute(file).parent_path());
  if (auto* value = root.get("projects")) {
    for (const auto& path :
         *select(*value, target, stringsValue)->getAsArray()) {
      config.projects.push_back(
          anchor(path.getAsString()->str(), config.configDir));
    }
  }
  if (auto* value = root.get("package_id")) {
    identityValue(*value);
    config.packageId = value->getAsString()->str();
  }
  if (auto* value = root.get("root")) {
    boolValue(*value);
    config.root = *value->getAsBoolean();
  }
  if (auto* value = root.get("sun_fmt")) {
    const auto& fmt = object(*value);
    keys(fmt, {"ignore"});
    if (auto* ignore = fmt.get("ignore")) {
      stringsValue(*ignore);
      for (const auto& pattern : *ignore->getAsArray()) {
        config.fmtIgnore.push_back(
            {config.configDir, pattern.getAsString()->str()});
      }
    }
  }
  if (auto* value = root.get("sun_path")) {
    for (const auto& path :
         *select(*value, target, stringsValue)->getAsArray()) {
      config.sunPath.push_back(
          anchor(path.getAsString()->str(), config.configDir));
    }
  }
  if (auto* bindings = root.get("c_bindings")) {
    for (const auto& [name, value] : object(*bindings)) {
      const std::string module = name.str();
      if (module.empty() ||
          (!std::isalpha(static_cast<unsigned char>(module[0])) &&
           module[0] != '_') ||
          !std::all_of(module.begin(), module.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_';
          })) {
        invalid("c_bindings keys must be Sun module identifiers");
      }
      ConfigCBinding binding;
      binding.baseDir = config.configDir.string();
      const auto* selected = select(value, target, cBindingValue, false);
      binding.enabled = selected != nullptr;
      if (selected) {
        const auto& obj = object(*selected);
        binding.skipIfUnavailable =
            obj.getBoolean("skip_if_unavailable").value_or(false);
        binding.header =
            anchor(obj.getString("header")->str(), config.configDir);
        binding.output =
            anchor(obj.getString("output")->str(), config.configDir);
        for (const auto& name : *obj.getArray("allow")) {
          binding.allow.push_back(name.getAsString()->str());
        }
        if (const auto* args = obj.getArray("clang_args")) {
          for (const auto& arg : *args) {
            binding.clangArgs.push_back(arg.getAsString()->str());
          }
        }
      }
      config.cBindings.emplace(module, std::move(binding));
    }
  }
  if (auto* vars = root.get("path_variables")) {
    for (const auto& [key, value] : object(*vars)) {
      config.pathVariables[key.str()] =
          anchor(select(value, target, stringValue)->getAsString()->str(),
                 config.configDir);
    }
  }
  if (auto* value = root.get("exports")) {
    resourcesValue(*value);
    config.exports = resources(*value, config.configDir);
  }
  if (auto* value = root.get("steps")) {
    const auto* array = value->getAsArray();
    if (!array) {
      invalid("steps must be an array");
    }
    std::set<std::string> names;
    for (const auto& item : *array) {
      const auto& obj = object(item);
      keys(obj, {"name", "command", "inputs", "outputs", "requires",
                 "working_directory", "environment", "enabled"});
      if (!obj.get("name")) {
        invalid("step requires name");
      }
      stringValue(*obj.get("name"));
      ConfigBuildStep step;
      step.name = obj.getString("name")->str();
      validName(step.name);
      if (!names.insert(step.name).second) {
        invalid("duplicate step: " + step.name);
      }
      bool enabled = true;
      if (auto* v = obj.get("enabled")) {
        enabled = *select(*v, target, boolValue)->getAsBoolean();
      }
      for (const auto* key : {"command", "inputs", "outputs", "requires"}) {
        const auto* field = obj.get(key);
        if (!field &&
            (std::string(key) == "command" || std::string(key) == "outputs")) {
          invalid("step requires command and outputs");
        }
        if (!field) {
          continue;
        }
        auto* selected = select(*field, target, stringsValue, enabled);
        if (!selected) {
          continue;
        }
        auto* values = &step.command;
        if (std::string(key) == "inputs") {
          values = &step.inputs;
        }
        if (std::string(key) == "outputs") {
          values = &step.outputs;
        }
        if (std::string(key) == "requires") {
          values = &step.prerequisites;
        }
        for (const auto& v : *selected->getAsArray()) {
          values->push_back(v.getAsString()->str());
        }
      }
      if (enabled && (step.command.empty() || step.outputs.empty())) {
        invalid("step command and outputs cannot be empty");
      }
      step.workingDirectory = config.configDir.string();
      if (auto* v = obj.get("working_directory")) {
        auto* selected = select(*v, target, stringValue, enabled);
        if (selected) {
          step.workingDirectory =
              anchor(selected->getAsString()->str(), config.configDir);
        }
      }
      if (auto* v = obj.get("environment")) {
        for (const auto& [key, value] : object(*v)) {
          stringValue(value);
          step.environment[key.str()] = value.getAsString()->str();
        }
      }
      if (enabled) {
        config.steps.push_back(std::move(step));
      }
    }
  }
  if (auto* deps = root.get("dependencies")) {
    const auto* array = deps->getAsArray();
    if (!array) {
      invalid(
          "dependencies must be an array; replace named moon/package wrappers "
          "with {package, library or binary, url or path, hash} entries");
    }
    std::set<std::pair<std::string, std::string>> selections;
    for (const auto& value : *array) {
      const auto* selected = select(value, target, dependencyValue, false);
      if (!selected) {
        continue;
      }
      const auto& obj = object(*selected);
      if (auto* packages = obj.getObject("packages")) {
        ConfigDependency repository;
        repository.git = obj.getString("git")->str();
        repository.version = obj.getString("version").value_or("").str();
        repository.config =
            obj.getString("config").value_or("sun-config.json").str();
        for (const auto& [id, value] : *packages) {
          for (const auto& entry :
               *value.getAsObject()->getArray("entrypoints")) {
            repository.gitPackages[id.str()].push_back(
                entry.getAsString()->str());
          }
        }
        for (const auto& [id, entries] : repository.gitPackages) {
          if (config.dependencies.count(id) || config.pathVariables.count(id)) {
            invalid("duplicate dependency or path variable: " + id);
          }
          auto dependency = repository;
          dependency.entrypoints = entries;
          config.dependencies.emplace(id, std::move(dependency));
        }
        continue;
      }
      const auto name = obj.getString("package")->str();
      if (config.pathVariables.count(name)) {
        invalid("dependency and path variable collide: " + name);
      }
      ConfigDependency dep;
      dep.library = obj.getString("library").value_or("").str();
      dep.binary = obj.getString("binary").value_or("").str();
      dep.package = dep.library.empty() && dep.binary.empty();
      const auto id = dep.package
                          ? std::string()
                          : (dep.binary.empty() ? "library:" + dep.library
                                                : "binary:" + dep.binary);
      if (!selections.emplace(name, id).second) {
        invalid("duplicate dependency selection: " + name + " " + id);
      }
      dep.url = obj.getString("url").value_or("").str();
      dep.hash = obj.getString("hash").value_or("").str();
      dep.git = obj.getString("git").value_or("").str();
      dep.version = obj.getString("version").value_or("").str();
      dep.config = obj.getString("config").value_or("sun-config.json").str();
      if (const auto* entries = obj.getArray("entrypoints")) {
        for (const auto& entry : *entries) {
          dep.entrypoints.push_back(entry.getAsString()->str());
        }
      }
      if (auto path = obj.getString("path")) {
        dep.path = dep.git.empty() ? anchor(path->str(), config.configDir)
                                   : path->str();
      }
      if (!dep.package) {
        dep.filename =
            std::filesystem::path(dep.binary.empty() ? dep.library : dep.binary)
                .filename()
                .string();
        if (dep.binary.empty()) {
          dep.filename += ".moon";
        }
      }
      auto& group = config.dependencies[name];
      if (!group.gitPackages.empty()) {
        invalid("duplicate dependency: " + name);
      }
      if (!group.selections.empty() &&
          (dep.package || group.selections.front().package)) {
        invalid("whole package and individual selections cannot be mixed: " +
                name);
      }
      group.entrypoints.insert(group.entrypoints.end(), dep.entrypoints.begin(),
                               dep.entrypoints.end());
      group.selections.push_back(std::move(dep));
    }
  }
  std::set<std::string> names, declaredNames;
  if (auto* value = root.get("entrypoints")) {
    auto* array = value->getAsArray();
    if (!array) {
      invalid("entrypoints must be an array");
    }
    for (const auto& item : *array) {
      auto obj = fields(object(item), target, false);
      ConfigEntrypoint entry;
      if (auto* scripts = obj.getArray("build")) {
        if (scripts->empty()) {
          invalid("build must contain at least one executable path");
        }
        for (const auto& script : *scripts) {
          auto path = script.getAsString()->str();
          if (path.empty() || std::filesystem::path(path).is_absolute() ||
              path.find('$') != std::string::npos) {
            invalid(
                "build scripts must be nonempty config-relative executable "
                "paths");
          }
          entry.build.push_back(anchor(path, config.configDir));
        }
        for (const auto* field :
             {"steps", "compile_args", "pkg_config", "link_args", "git"}) {
          if (obj.get(field)) {
            invalid(std::string("build cannot be combined with ") + field);
          }
        }
      }
      entry.exported = obj.getBoolean("export").value_or(false);
      if (auto* value = obj.get("resources")) {
        entry.resources = resources(*value, config.configDir);
      }
      if (auto* values = obj.getArray("link_args")) {
        for (const auto& value : *values) {
          entry.linkArgs.push_back(value.getAsString()->str());
        }
      }
      if (auto* values = obj.getArray("pkg_config")) {
        for (const auto& value : *values) {
          entry.pkgConfig.push_back(value.getAsString()->str());
        }
      }
      if (auto* values = obj.getArray("steps")) {
        for (const auto& value : *values) {
          entry.steps.push_back(value.getAsString()->str());
        }
      }
      if (auto* values = obj.getArray("requires")) {
        for (const auto& value : *values) {
          entry.prerequisites.push_back(value.getAsString()->str());
        }
      }
      if (auto* values = obj.getArray("compile_args")) {
        for (const auto& value : *values) {
          entry.compileArgs.push_back(value.getAsString()->str());
        }
      }
      if (auto name = obj.getString("name")) {
        entry.name = name->str();
        declaredNames.insert(entry.name);
      }
      if (!*obj.getBoolean("enabled")) {
        continue;
      }
      entry.path = obj.getString("path")->str();
      if (auto v = obj.getString("git")) {
        entry.git = v->str();
      }
      if (auto v = obj.getString("version")) {
        entry.version = v->str();
      }
      if (auto v = obj.getString("type")) {
        entry.type = *v == "library" ? ConfigEntrypoint::Type::Library
                                     : ConfigEntrypoint::Type::Binary;
      }
      if (!entry.git.empty()) {
        validateGitSource(entry);
      } else {
        if (!entry.version.empty()) {
          invalid("entrypoint version requires git");
        }
        entry.path = anchor(entry.path, config.configDir);
      }
      if (entry.name.empty()) {
        entry.name = std::filesystem::path(entry.path).stem().string();
        validName(entry.name);
      }
      declaredNames.insert(entry.name);
      if (!names.insert(entry.name).second) {
        invalid("duplicate entrypoint name " + entry.name);
      }
      config.entrypoints.push_back(std::move(entry));
    }
  }
  std::set<std::string> packageNames;
  if (auto* value = root.get("packages")) {
    auto* array = value->getAsArray();
    if (!array) {
      invalid("packages must be an array");
    }
    for (const auto& item : *array) {
      auto obj = fields(object(item), target, true);
      ConfigPackage pkg;
      pkg.name = obj.getString("name")->str();
      if (!packageNames.insert(pkg.name).second) {
        invalid("duplicate package name " + pkg.name);
      }
      if (!*obj.getBoolean("enabled")) {
        continue;
      }
      for (const auto& v : *obj.getArray("entrypoints")) {
        const auto name = v.getAsString()->str();
        if (!declaredNames.count(name)) {
          invalid("unknown package entrypoint " + name);
        }
        pkg.entrypoints.push_back(name);
      }
      if (auto* v = obj.get("resources")) {
        pkg.resources = resources(*v, config.configDir);
      }
      if (auto v = obj.getString("output_name")) {
        pkg.outputName = anchor(v->str(), config.configDir);
      }
      config.packages.push_back(std::move(pkg));
    }
  }
  if (!config.packageId.empty()) {
    const auto project = config.packageId.empty()
                             ? config.configDir.filename().string()
                             : config.packageId;
    const std::string buildConfig = "release";
    config.pathVariables.try_emplace(
        "SUN_BUILD",
        (config.configDir / ".sun/build" / target / buildConfig / project)
            .string());
    config.pathVariables.try_emplace(
        "SUN_INSTALL",
        (config.configDir / ".sun/install" / target / buildConfig / project)
            .string());
  }
  return config;
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
      auto config = loadFile(candidate, targetTriple);
      const bool stop = config.root;
      if (!merged) {
        merged = std::move(config);
      } else {
        merged->fmtIgnore.insert(merged->fmtIgnore.begin(),
                                 config.fmtIgnore.begin(),
                                 config.fmtIgnore.end());
        for (const auto& [name, value] : config.pathVariables) {
          merged->pathVariables.emplace(name, value);
        }
        for (const auto& [name, value] : config.dependencies) {
          merged->dependencies.emplace(name, value);
        }
        for (const auto& [name, value] : config.cBindings) {
          merged->cBindings.emplace(name, value);
        }
        merged->sunPath.insert(merged->sunPath.end(), config.sunPath.begin(),
                               config.sunPath.end());
        merged->entrypoints.insert(merged->entrypoints.end(),
                                   config.entrypoints.begin(),
                                   config.entrypoints.end());
      }
      for (const auto& [name, value] : merged->dependencies) {
        if (merged->pathVariables.count(name)) {
          invalid("dependency and path variable collide: " + name);
        }
      }
      if (stop) {
        break;
      }
    }
    if (std::filesystem::is_directory(dir / ".sun-git-source")) {
      break;
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
  if (!in) {
    invalid("could not read " + file.string());
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  auto parsed = llvm::json::parse(buffer.str());
  if (!parsed) {
    invalid("malformed JSON in " + file.string() + ": " +
            llvm::toString(parsed.takeError()));
  }
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
        if (containsWrapper(setting)) {
          invalid("legacy target blocks cannot contain target wrappers");
        }
        if (const auto* base = root.get(field)) {
          if (field == "path_variables") {
            for (const auto& [var, v] : object(setting)) {
              if (auto* original = object(*base).get(var);
                  original && wrapped(*original)) {
                invalid("overlapping legacy and per-setting target overrides");
              }
            }
          } else if (containsWrapper(*base)) {
            invalid("overlapping legacy and per-setting target overrides");
          }
        }
      }
      // Legacy collections are complete values even when their target is
      // inactive.
      for (const auto* field : {"entrypoints", "packages"}) {
        if (const auto* value = settings.get(field)) {
          const auto* entries = value->getAsArray();
          if (!entries) {
            invalid(std::string(field) + " must be an array");
          }
          for (const auto& entry : *entries) {
            const bool package = std::string(field) == "packages";
            fields(object(entry), target, package);
          }
        }
      }
      if (auto* paths = settings.get("sun_path")) {
        stringsValue(*paths);
      }
      if (auto* vars = settings.get("path_variables")) {
        for (const auto& [name, v] : object(*vars)) {
          stringValue(v);
        }
      }
      if (configTargetKey(key.str()) == target) {
        selected = &settings;
      }
    }
    if (selected) {
      Object copy = *selected;
      for (const auto& [field, value] : copy) {
        if (field == "path_variables" && root.get("path_variables")) {
          Object vars = object(*root.get("path_variables"));
          for (const auto& [name, v] : object(value)) {
            vars[name] = v;
          }
          root[field] = std::move(vars);
        } else {
          root[field] = value;
        }
      }
    }
    root.erase("target");
  }
  auto config = parse(root, file, target);
  config.configFile =
      std::filesystem::weakly_canonical(std::filesystem::absolute(file));
  // Config matching ignores vendor and OS version; compilation must retain
  // both.
  config.targetTriple = sun::support::resolvedTargetTriple(targetTriple).str();
  if (!config.packageId.empty()) {
    const auto project = config.packageId.empty()
                             ? config.configDir.filename().string()
                             : config.packageId;
    const std::string buildConfig = "release";
    config.pathVariables.try_emplace(
        "SUN_BUILD",
        (config.configDir / ".sun/build" / target / buildConfig / project)
            .string());
    config.pathVariables.try_emplace(
        "SUN_INSTALL",
        (config.configDir / ".sun/install" / target / buildConfig / project)
            .string());
  }
  return config;
}
}  // namespace sun::driver
