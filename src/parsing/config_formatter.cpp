/** Canonical, target-independent formatting for sun-config.json. */
#include "parsing/config_formatter.h"

#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "support/error.h"

/** Formats configuration text without resolving project settings. */
namespace sun::parsing {
/** Keeps schema-ordering helpers private to the formatter. */
namespace {
/** Retains user-defined object key order during parsing and printing. */
using Json = nlohmann::ordered_json;

/** Selects the schema of an object or the values of a user-defined map. */
enum class Schema {
  Opaque,
  Value,
  Root,
  Entrypoint,
  Package,
  Resource,
  Dependency,
  Check,
  Step,
  Binding,
  Fmt,
  DependencyMap,
  BindingMap,
  GitPackageMap,
  GitSelection,
  ValueMap
};

/** Returns the canonical field order for a structured object. */
std::vector<std::string> fields(Schema schema) {
  switch (schema) {
    case Schema::Root:
      return {"package_id", "root",           "supported_targets",
              "sun_path",   "path_variables", "exception_trace_depth",
              "sun_fmt",    "projects",       "entrypoints",
              "packages",   "exports",        "dependencies",
              "c_bindings", "target",         "steps"};
    case Schema::Entrypoint:
      return {"name",     "enabled",   "path",       "git",      "version",
              "type",     "export",    "steps",      "build",    "compile_args",
              "requires", "resources", "pkg_config", "link_args"};
    case Schema::Package:
      return {"name", "enabled", "entrypoints", "resources", "output_name"};
    case Schema::Resource:
      return {"path", "destination"};
    case Schema::Dependency:
      return {"type",  "package", "library", "binary",
              "git",   "version", "config",  "packages",
              "url",   "path",    "sha256",  "entrypoints",
              "check", "apt",     "brew",    "supported_targets",
              "target"};
    case Schema::Check:
      return {"type", "name", "version"};
    case Schema::Step:
      return {"name",    "enabled",  "command",           "inputs",
              "outputs", "requires", "working_directory", "environment"};
    case Schema::Binding:
      return {"header", "output", "allow", "clang_args", "skip_if_unavailable"};
    case Schema::Fmt:
      return {"ignore"};
    case Schema::GitSelection:
      return {"entrypoints"};
    default:
      return {};
  }
}

/** Identifies the schema of a field without treating user map keys as fields.
 */
Schema child(Schema parent, const std::string& key) {
  if (parent == Schema::DependencyMap) {
    return Schema::Dependency;
  }
  if (parent == Schema::BindingMap) {
    return Schema::Binding;
  }
  if (parent == Schema::GitPackageMap) {
    return Schema::GitSelection;
  }
  if (parent == Schema::Root) {
    if (key == "path_variables") {
      return Schema::ValueMap;
    }
    if (key == "entrypoints") {
      return Schema::Entrypoint;
    }
    if (key == "packages") {
      return Schema::Package;
    }
    if (key == "exports") {
      return Schema::Resource;
    }
    if (key == "dependencies") {
      return Schema::DependencyMap;
    }
    if (key == "c_bindings") {
      return Schema::BindingMap;
    }
    if (key == "steps") {
      return Schema::Step;
    }
    if (key == "sun_fmt") {
      return Schema::Fmt;
    }
  }
  if ((parent == Schema::Entrypoint || parent == Schema::Package) &&
      key == "resources") {
    return Schema::Resource;
  }
  if (parent == Schema::Dependency) {
    if (key == "check") {
      return Schema::Check;
    }
    if (key == "packages") {
      return Schema::GitPackageMap;
    }
  }
  if (parent == Schema::Step && key == "environment") {
    return Schema::Opaque;
  }
  return Schema::Value;
}

/** Orders structured fields recursively, retaining unknown fields and map keys.
 */
Json order(const Json& value, Schema schema, bool selectable = true) {
  if (schema == Schema::Opaque) {
    return value;
  }
  if (value.is_array()) {
    Json result = Json::array();
    for (const auto& entry : value) {
      result.push_back(order(entry, schema));
    }
    return result;
  }
  if (!value.is_object()) {
    return value;
  }
  const bool map =
      schema == Schema::DependencyMap || schema == Schema::BindingMap ||
      schema == Schema::GitPackageMap || schema == Schema::ValueMap;
  const bool wrapper =
      selectable && !map && schema != Schema::Root &&
      (value.contains("target") || value.contains("default")) &&
      (value.size() == 1 || (value.size() == 2 && value.contains("default") &&
                             value.contains("target")));
  Json result = Json::object();
  /** Adds a field with the schema appropriate to its context. */
  const auto append = [&](const std::string& key) {
    const auto& item = value.at(key);
    if ((wrapper || schema == Schema::Root || schema == Schema::Dependency) &&
        key == "target" && item.is_object()) {
      Json targets = Json::object();
      for (auto it = item.begin(); it != item.end(); ++it) {
        targets[it.key()] = order(it.value(), schema, false);
      }
      result[key] = std::move(targets);
    } else if (wrapper && key == "default") {
      result[key] = order(item, schema, false);
    } else {
      result[key] = order(item, child(schema, key));
    }
  };
  for (const auto& key : wrapper ? std::vector<std::string>{"default", "target"}
                                 : fields(schema)) {
    if (value.contains(key)) {
      append(key);
    }
  }
  for (auto it = value.begin(); it != value.end(); ++it) {
    if (!result.contains(it.key())) {
      // Map entries have a known value schema; unknown fields remain opaque.
      result[it.key()] =
          map ? order(it.value(), child(schema, it.key())) : it.value();
    }
  }
  return result;
}
}  // namespace

/** Parses and formats config text without loading or executing the project. */
std::string formatConfig(const std::string& source) {
  try {
    std::vector<std::set<std::string>> objects;
    /** Rejects duplicate keys before the JSON parser can overwrite them. */
    const auto callback = [&](int, Json::parse_event_t event, Json& value) {
      if (event == Json::parse_event_t::object_start) {
        objects.emplace_back();
      }
      if (event == Json::parse_event_t::key &&
          !objects.back().insert(value.get<std::string>()).second) {
        sun::support::logAndThrowError("sun-config.json: duplicate key '" +
                                       value.get<std::string>() + "'");
      }
      if (event == Json::parse_event_t::object_end) {
        objects.pop_back();
      }
      return true;
    };
    const auto parsed = Json::parse(source, callback);
    if (!parsed.is_object()) {
      sun::support::logAndThrowError("sun-config.json: expected an object");
    }
    return order(parsed, Schema::Root).dump(2) + "\n";
  } catch (const Json::exception& error) {
    sun::support::logAndThrowError(std::string("sun-config.json: ") +
                                   error.what());
  }
}
}  // namespace sun::parsing
