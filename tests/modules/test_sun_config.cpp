// tests/modules/test_sun_config.cpp - Per-folder sun-config.json: path
// variables, explicit overrides, and library search paths.

#include <gtest/gtest.h>
#include <llvm/TargetParser/Host.h>

#include <filesystem>
#include <fstream>

#include "driver/manifest_processor.h"
#include "driver/sun_config.h"
#include "support/error.h"

using sun::driver::ConfigEntrypoint;
using sun::driver::ManifestProcessor;
using sun::driver::SunConfig;

using sun::support::SunError;

namespace fs = std::filesystem;

/** Keeps test fixtures and helpers local to this source file. */
namespace {

/** Creates a fresh temporary directory for the test's files. */
fs::path freshDir(const std::string& name) {
  fs::path dir = fs::temp_directory_path() / "sun_config_tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  // Canonical, so expected paths match what the config walk produces:
  // findFrom resolves symlinks, and macOS's temp directory is one
  // (/var -> /private/var).
  return fs::canonical(dir);
}

/** Writes source or fixture data to a test file. */
void writeFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

}  // namespace

TEST(Modules_SunConfig, cli_variables_override_config_and_environment) {
  fs::path dir = freshDir("override");
  writeFile(dir / "sun-config.json",
            "{ \"path_variables\": { \"LIBS\": \"conflibs\" } }\n");
  writeFile(dir / "main.sun",
            "manifest { libraries: [\"$LIBS/lib.moon\"] }\n"
            "function main() i32 { return 0; }\n");

  ManifestProcessor::setPathVariable("LIBS", "/from-cli");
  setenv("LIBS", "/from-env", 1);
  auto resolved =
      ManifestProcessor::fromEntrypointFile((dir / "main.sun").string());
  ManifestProcessor::clearPathVariables();
  unsetenv("LIBS");

  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved->moonImports.size(), 1u);
  EXPECT_EQ(fs::path(resolved->moonImports[0].path).lexically_normal(),
            fs::path("/from-cli/lib.moon"));
}

TEST(Modules_SunConfig, config_is_found_in_a_parent_folder) {
  fs::path dir = freshDir("parent");
  writeFile(dir / "sun-config.json",
            "{ \"path_variables\": { \"SHARED\": \"common\" } }\n");
  writeFile(dir / "src" / "main.sun",
            "manifest { source_files: [\"$SHARED/util.sun\"] }\n"
            "function main() i32 { return 0; }\n");

  auto resolved = ManifestProcessor::fromEntrypointFile(
      (dir / "src" / "main.sun").string());

  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved->sunFiles.size(), 1u);
  // The value is anchored at the config's folder, not the entrypoint's
  EXPECT_EQ(fs::path(resolved->sunFiles[0]).lexically_normal(),
            (dir / "common" / "util.sun").lexically_normal());
}

TEST(Modules_SunConfig, config_sun_path_resolves_manifest_entries) {
  fs::path dir = freshDir("sunpath");
  writeFile(dir / "sun-config.json", "{ \"sun_path\": [\"deps\"] }\n");
  writeFile(dir / "deps" / "util.moon", "not a real bundle\n");
  writeFile(dir / "main.sun",
            "manifest { libraries: [\"util.moon\"] }\n"
            "function main() i32 { return 0; }\n");

  auto resolved =
      ManifestProcessor::fromEntrypointFile((dir / "main.sun").string());

  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved->moonImports.size(), 1u);
  EXPECT_EQ(fs::path(resolved->moonImports[0].path).lexically_normal(),
            (dir / "deps" / "util.moon").lexically_normal());
}

TEST(Modules_SunConfig, configs_merge_up_the_parent_chain) {
  fs::path dir = freshDir("merge");
  writeFile(dir / "sun-config.json",
            "{ \"sun_path\": [\"pdeps\"], "
            "\"path_variables\": { \"SHARED\": \"common\", \"LIBS\": "
            "\"parentlibs\" } }\n");
  writeFile(dir / "sub" / "sun-config.json",
            "{ \"sun_path\": [\"cdeps\"], "
            "\"path_variables\": { \"LIBS\": \"libs\" } }\n");

  auto config = SunConfig::findFrom(dir / "sub");
  ASSERT_TRUE(config.has_value());
  // The nearest definition of a variable wins; others are inherited
  EXPECT_EQ(config->pathVariables.at("LIBS"),
            (dir / "sub" / "libs").lexically_normal().string());
  EXPECT_EQ(config->pathVariables.at("SHARED"),
            (dir / "common").lexically_normal().string());
  // Search dirs concatenate nearest-first
  ASSERT_EQ(config->sunPath.size(), 2u);
  EXPECT_EQ(config->sunPath[0],
            (dir / "sub" / "cdeps").lexically_normal().string());
  EXPECT_EQ(config->sunPath[1], (dir / "pdeps").lexically_normal().string());
}

TEST(Modules_SunConfig, root_true_stops_the_parent_walk) {
  fs::path dir = freshDir("root_stop");
  writeFile(dir / "sun-config.json",
            "{ \"path_variables\": { \"SHARED\": \"common\" } }\n");
  writeFile(dir / "sub" / "sun-config.json",
            "{ \"root\": true, \"path_variables\": { \"LIBS\": \"libs\" } }\n");

  auto config = SunConfig::findFrom(dir / "sub");
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->pathVariables.count("LIBS"), 1u);
  EXPECT_EQ(config->pathVariables.count("SHARED"), 0u);
}

TEST(Modules_SunConfig, root_must_be_boolean) {
  fs::path dir = freshDir("root_type");
  writeFile(dir / "sun-config.json", "{ \"root\": \"yes\" }\n");
  EXPECT_THROW(SunConfig::loadFile(dir / "sun-config.json"), SunError);
}

TEST(Modules_SunConfig, malformed_config_is_an_error) {
  fs::path dir = freshDir("malformed");
  writeFile(dir / "sun-config.json", "{ not json\n");
  writeFile(dir / "main.sun",
            "manifest { libraries: [\"lib.moon\"] }\n"
            "function main() i32 { return 0; }\n");

  EXPECT_THROW(
      ManifestProcessor::fromEntrypointFile((dir / "main.sun").string()),
      SunError);
}

TEST(Modules_SunConfig, unknown_config_key_is_an_error) {
  fs::path dir = freshDir("unknown_key");
  writeFile(dir / "sun-config.json", "{ \"pathVars\": {} }\n");

  try {
    SunConfig::loadFile(dir / "sun-config.json");
    FAIL() << "expected an unknown-key error";
  } catch (const SunError& e) {
    EXPECT_NE(std::string(e.what()).find("pathVars"), std::string::npos);
  }
}

TEST(Modules_SunConfig, entrypoints_parse_with_anchored_paths) {
  fs::path dir = freshDir("entrypoints");
  writeFile(dir / "sun-config.json",
            "{ \"entrypoints\": ["
            "{ \"path\": \"stdlib/stdlib.sun\", \"type\": \"library\", "
            "\"output_name\": \"build/stdlib\", "
            "\"test_binary_name\": \"build/stdlib_test\" },"
            "{ \"path\": \"app.sun\" }"
            "] }\n");

  auto config = SunConfig::loadFile(dir / "sun-config.json");
  ASSERT_EQ(config.entrypoints.size(), 2u);
  EXPECT_EQ(config.entrypoints[0].path,
            (dir / "stdlib" / "stdlib.sun").lexically_normal().string());
  EXPECT_EQ(config.entrypoints[0].type, ConfigEntrypoint::Type::Library);
  EXPECT_EQ(config.entrypoints[0].outputName,
            (dir / "build" / "stdlib").lexically_normal().string());
  EXPECT_EQ(config.entrypoints[0].testBinaryName,
            (dir / "build" / "stdlib_test").lexically_normal().string());
  // Defaults: binary type, names derived later from the entrypoint
  EXPECT_EQ(config.entrypoints[1].type, ConfigEntrypoint::Type::Binary);
  EXPECT_TRUE(config.entrypoints[1].outputName.empty());
  EXPECT_TRUE(config.entrypoints[1].testBinaryName.empty());
}

TEST(Modules_SunConfig, entrypoint_without_path_is_an_error) {
  fs::path dir = freshDir("entrypoint_no_path");
  writeFile(dir / "sun-config.json",
            "{ \"entrypoints\": [{ \"type\": \"library\" }] }\n");
  EXPECT_THROW(SunConfig::loadFile(dir / "sun-config.json"), SunError);
}

TEST(Modules_SunConfig, entrypoint_type_must_be_binary_or_library) {
  fs::path dir = freshDir("entrypoint_bad_type");
  writeFile(dir / "sun-config.json",
            "{ \"entrypoints\": "
            "[{ \"path\": \"a.sun\", \"type\": \"plugin\" }] }\n");
  EXPECT_THROW(SunConfig::loadFile(dir / "sun-config.json"), SunError);
}

TEST(Modules_SunConfig, unknown_entrypoint_key_is_an_error) {
  fs::path dir = freshDir("entrypoint_unknown_key");
  writeFile(dir / "sun-config.json",
            "{ \"entrypoints\": "
            "[{ \"path\": \"a.sun\", \"binaryName\": \"a\" }] }\n");
  try {
    SunConfig::loadFile(dir / "sun-config.json");
    FAIL() << "expected an unknown-key error";
  } catch (const SunError& e) {
    EXPECT_NE(std::string(e.what()).find("binaryName"), std::string::npos);
  }
}

TEST(Modules_SunConfig, entrypoints_concatenate_up_the_parent_chain) {
  fs::path dir = freshDir("entrypoint_merge");
  writeFile(dir / "sun-config.json",
            "{ \"entrypoints\": [{ \"path\": \"parent.sun\" }] }\n");
  writeFile(dir / "sub" / "sun-config.json",
            "{ \"entrypoints\": [{ \"path\": \"child.sun\" }] }\n");

  auto config = SunConfig::findFrom(dir / "sub");
  ASSERT_TRUE(config.has_value());
  ASSERT_EQ(config->entrypoints.size(), 2u);
  EXPECT_EQ(config->entrypoints[0].path,
            (dir / "sub" / "child.sun").lexically_normal().string());
  EXPECT_EQ(config->entrypoints[1].path,
            (dir / "parent.sun").lexically_normal().string());
}

TEST(Modules_SunConfig, absolute_config_entries_are_kept_as_is) {
  fs::path dir = freshDir("absolute");
  writeFile(dir / "sun-config.json",
            "{ \"sun_path\": [\"/opt/sun\"], "
            "\"path_variables\": { \"LIBS\": \"/opt/libs\" } }\n");

  auto config = SunConfig::loadFile(dir / "sun-config.json");
  ASSERT_EQ(config.sunPath.size(), 1u);
  EXPECT_EQ(config.sunPath[0], "/opt/sun");
  EXPECT_EQ(config.pathVariables.at("LIBS"), "/opt/libs");
}

TEST(Modules_SunConfig, target_paths_and_outputs_use_normalized_triples) {
  auto dir = freshDir("target_paths");
  writeFile(dir / "sun-config.json", R"({
  "root": true,
  "sun_path": [
    "native"
  ],
  "path_variables": {
    "SSL": "native-ssl",
    "SHARED": "shared"
  },
  "target": {
    "aarch64-linux-gnu": {
      "sun_path": [
        "arm"
      ],
      "path_variables": {
        "SSL": "arm-ssl"
      },
      "entrypoints": [
        {
          "path": "main.sun",
          "output_name": "arm/main"
        }
      ]
    }
  },
  "entrypoints": [
    {
      "path": "main.sun",
      "output_name": "native/main",
      "test_binary_name": "native/tests"
    }
  ]
})");
  auto native =
      SunConfig::loadFile(dir / "sun-config.json", "x86_64-linux-gnu");
  EXPECT_EQ(native.entrypoints[0].outputName, (dir / "native/main").string());
  for (const auto& target :
       {"aarch64-linux-gnu", "aarch64-unknown-linux-gnu"}) {
    auto config = SunConfig::loadFile(dir / "sun-config.json", target);
    ASSERT_EQ(config.entrypoints.size(), 1u);
    EXPECT_EQ(config.sunPath, std::vector<std::string>{(dir / "arm").string()});
    EXPECT_EQ(config.pathVariables.at("SSL"), (dir / "arm-ssl").string());
    EXPECT_EQ(config.pathVariables.at("SHARED"), (dir / "shared").string());
    EXPECT_EQ(config.entrypoints[0].outputName, (dir / "arm/main").string());
    EXPECT_TRUE(config.entrypoints[0].testBinaryName.empty());
  }
  auto other =
      SunConfig::loadFile(dir / "sun-config.json", "aarch64-linux-musl");
  EXPECT_EQ(other.entrypoints[0].outputName, native.entrypoints[0].outputName);
}

TEST(Modules_SunConfig,
     target_variables_reach_manifests_and_keep_nearest_precedence) {
  auto dir = freshDir("target_manifest");
  writeFile(dir / "sun-config.json", R"({
    "root": true,
    "target": {"aarch64-linux-gnu": {"path_variables": {"SSL": "arm-ssl", "OTHER": "parent"}}}
  })");
  writeFile(dir / "child/sun-config.json",
            R"({"path_variables": {"OTHER": "child"}})");
  writeFile(dir / "child/main.sun",
            "manifest { archives: [\"$SSL/libssl.a\"] }");
  auto config = SunConfig::findFrom(dir / "child", "aarch64-linux-gnu");
  ASSERT_TRUE(config);
  EXPECT_EQ(config->pathVariables.at("OTHER"), (dir / "child/child").string());
  ManifestProcessor::setDefaultPathVariable("SSL", "/editor");
  auto manifest = ManifestProcessor::fromEntrypointFile(
      (dir / "child/main.sun").string(), "aarch64-linux-gnu");
  ManifestProcessor::clearPathVariables();
  ASSERT_TRUE(manifest);
  ASSERT_EQ(manifest->archiveFiles.size(), 1u);
  EXPECT_EQ(manifest->archiveFiles[0], (dir / "arm-ssl/libssl.a").string());
}

TEST(Modules_SunConfig, target_can_name_test_binary_independently) {
  auto dir = freshDir("target_tests");
  writeFile(dir / "sun-config.json", R"({
  "entrypoints": [
    {
      "path": "main.sun",
      "output_name": "app"
    }
  ],
  "target": {
    "aarch64-linux-gnu": {
      "entrypoints": [
        {
          "path": "main.sun",
          "output_name": "app",
          "test_binary_name": "arm/tests"
        }
      ]
    }
  }
})");
  auto config =
      SunConfig::loadFile(dir / "sun-config.json", "aarch64-linux-gnu");
  EXPECT_EQ(config.entrypoints[0].outputName, (dir / "app").string());
  EXPECT_EQ(config.entrypoints[0].testBinaryName, (dir / "arm/tests").string());
}

TEST(Modules_SunConfig, invalid_target_settings_are_errors_even_when_inactive) {
  auto dir = freshDir("invalid_target");
  for (
      const auto& contents :
      {R"({"target": []})", R"({"target": {"typo": {}}})",
       R"({"target": {"aarch64-linux-gnu": {"root": true}}})",
       R"({"target": {"aarch64-linux-gnu": {"sun_path": [42]}}})",
       R"({"target": {"aarch64-linux-gnu": {"entrypoints": {}}}})",
       R"({"target": {"aarch64-linux-gnu": {"entrypoints": [42]}}})",
       R"({"target": {"aarch64-linux-gnu": {"entrypoints": [{"type": "library"}]}}})",
       R"({"target": {"aarch64-linux-gnu": {"path_variables": {"SSL": 42}}}})",
       R"({"target": {"aarch64-linux-gnu": {}, "aarch64-unknown-linux-gnu": {}}})",
       R"({"entrypoints": [{"path": "a.sun", "target": {"aarch64-linux-gnu": {"output_name": 42}}}]})",
       R"({"entrypoints": [{"path": "a.sun", "target": {"aarch64-linux-gnu": {"path": "b.sun"}}}]})"}) {
    writeFile(dir / "sun-config.json", contents);
    EXPECT_THROW(SunConfig::loadFile(dir / "sun-config.json"), SunError)
        << contents;
  }
}

TEST(Modules_SunConfig, target_entrypoint_lists_replace_defaults_in_order) {
  auto dir = freshDir("target_entrypoint_lists");
  writeFile(dir / "sun-config.json", R"({
    "entrypoints": [{"path": "native.sun"}],
    "target": {
      "aarch64-linux-gnu": {"entrypoints": [
        {"path": "library.sun", "type": "library", "output_name": "arm/library"},
        {"path": "app.sun", "output_name": "arm/app"}
      ]},
      "aarch64-linux-musl": {"entrypoints": []},
      "arm64-apple-darwin": {"sun_path": ["mac"]}
    }
  })");
  auto arm = SunConfig::loadFile(dir / "sun-config.json", "aarch64-linux-gnu");
  ASSERT_EQ(arm.entrypoints.size(), 2u);
  EXPECT_EQ(arm.entrypoints[0].path, (dir / "library.sun").string());
  EXPECT_EQ(arm.entrypoints[0].type, ConfigEntrypoint::Type::Library);
  EXPECT_EQ(arm.entrypoints[1].path, (dir / "app.sun").string());
  EXPECT_TRUE(SunConfig::loadFile(dir / "sun-config.json", "aarch64-linux-musl")
                  .entrypoints.empty());
  auto mac = SunConfig::loadFile(dir / "sun-config.json", "arm64-apple-darwin");
  ASSERT_EQ(mac.entrypoints.size(), 1u);
  EXPECT_EQ(mac.entrypoints[0].path, (dir / "native.sun").string());
}

TEST(Modules_SunConfig, target_only_config_selects_host_without_a_flag) {
  auto dir = freshDir("host_target_only");
  const auto host = llvm::sys::getDefaultTargetTriple();
  writeFile(dir / "sun-config.json",
            "{ \"root\": true, \"target\": {\"" + host +
                "\": {\"entrypoints\": [{\"path\": \"main.sun\", "
                "\"output_name\": \"build/main\"}]}}}");
  auto config = SunConfig::loadFile(dir / "sun-config.json");
  ASSERT_EQ(config.entrypoints.size(), 1u);
  EXPECT_EQ(config.entrypoints[0].outputName, (dir / "build/main").string());
}

TEST(Modules_SunConfig, target_selection_accepts_platform_aliases) {
  auto dir = freshDir("target_aliases");
  writeFile(dir / "sun-config.json", R"({
    "target": {
      "x86_64-linux-gnu": {"entrypoints": [{"path": "linux.sun"}]},
      "arm64-apple-darwin": {"entrypoints": [{"path": "mac.sun"}]}
    }
  })");
  auto linuxConfig =
      SunConfig::loadFile(dir / "sun-config.json", "x86_64-pc-linux-gnu");
  ASSERT_EQ(linuxConfig.entrypoints.size(), 1u);
  EXPECT_EQ(linuxConfig.entrypoints[0].path, (dir / "linux.sun").string());
  for (const auto& target :
       {"aarch64-apple-darwin24.0.0", "arm64-apple-macosx15.0.0"}) {
    auto mac = SunConfig::loadFile(dir / "sun-config.json", target);
    ASSERT_EQ(mac.entrypoints.size(), 1u);
    EXPECT_EQ(mac.entrypoints[0].path, (dir / "mac.sun").string());
  }
}

/** Source configuration overrides defaults and environment until explicitly overridden. */
TEST(Modules_SunConfig, explicit_overrides_preserve_default_precedence) {
  SunConfig config;
  config.pathVariables["SUN_TEST_PATH_PRECEDENCE"] = "/config";
  setenv("SUN_TEST_PATH_PRECEDENCE", "/environment", 1);
  ManifestProcessor::setDefaultPathVariable("SUN_TEST_PATH_PRECEDENCE", "/project");
  EXPECT_EQ(ManifestProcessor::expandPathVariables("$SUN_TEST_PATH_PRECEDENCE/lib", &config),
            "/config/lib");
  ManifestProcessor::setPathVariable("SUN_TEST_PATH_PRECEDENCE", "/explicit");
  ManifestProcessor::setDefaultPathVariable("SUN_TEST_PATH_PRECEDENCE", "/later-project");
  EXPECT_EQ(ManifestProcessor::expandPathVariables("$SUN_TEST_PATH_PRECEDENCE/lib", &config),
            "/explicit/lib");
  ManifestProcessor::clearPathVariables();
  unsetenv("SUN_TEST_PATH_PRECEDENCE");
}
