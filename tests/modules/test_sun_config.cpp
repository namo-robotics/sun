// tests/modules/test_sun_config.cpp - Per-folder sun-config.json: path
// variables and library search paths that override outside configuration.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "driver/manifest_processor.h"
#include "driver/sun_config.h"
#include "support/error.h"

namespace fs = std::filesystem;

namespace {

fs::path freshDir(const std::string& name) {
  fs::path dir = fs::temp_directory_path() / "sun_config_tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void writeFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

}  // namespace

TEST(Modules_SunConfig, config_variables_override_cli_and_environment) {
  fs::path dir = freshDir("override");
  writeFile(dir / "sun-config.json",
            "{ \"pathVariables\": { \"LIBS\": \"conflibs\" } }\n");
  writeFile(dir / "main.sun",
            "manifest { moons: [\"$LIBS/lib.moon\"] }\n"
            "function main() i32 { return 0; }\n");

  sun::ManifestProcessor::setPathVariable("LIBS", "/from-cli");
  setenv("LIBS", "/from-env", 1);
  auto resolved =
      sun::ManifestProcessor::fromEntrypointFile((dir / "main.sun").string());
  sun::ManifestProcessor::clearPathVariables();
  unsetenv("LIBS");

  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved->moonImports.size(), 1u);
  EXPECT_EQ(fs::path(resolved->moonImports[0].path).lexically_normal(),
            (dir / "conflibs" / "lib.moon").lexically_normal());
}

TEST(Modules_SunConfig, config_is_found_in_a_parent_folder) {
  fs::path dir = freshDir("parent");
  writeFile(dir / "sun-config.json",
            "{ \"pathVariables\": { \"SHARED\": \"common\" } }\n");
  writeFile(dir / "src" / "main.sun",
            "manifest { suns: [\"$SHARED/util.sun\"] }\n"
            "function main() i32 { return 0; }\n");

  auto resolved = sun::ManifestProcessor::fromEntrypointFile(
      (dir / "src" / "main.sun").string());

  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved->sunFiles.size(), 1u);
  // The value is anchored at the config's folder, not the entrypoint's
  EXPECT_EQ(fs::path(resolved->sunFiles[0]).lexically_normal(),
            (dir / "common" / "util.sun").lexically_normal());
}

TEST(Modules_SunConfig, config_sun_path_resolves_manifest_entries) {
  fs::path dir = freshDir("sunpath");
  writeFile(dir / "sun-config.json", "{ \"sunPath\": [\"deps\"] }\n");
  writeFile(dir / "deps" / "util.moon", "not a real bundle\n");
  writeFile(dir / "main.sun",
            "manifest { moons: [\"util.moon\"] }\n"
            "function main() i32 { return 0; }\n");

  auto resolved =
      sun::ManifestProcessor::fromEntrypointFile((dir / "main.sun").string());

  ASSERT_TRUE(resolved.has_value());
  ASSERT_EQ(resolved->moonImports.size(), 1u);
  EXPECT_EQ(fs::path(resolved->moonImports[0].path).lexically_normal(),
            (dir / "deps" / "util.moon").lexically_normal());
}

TEST(Modules_SunConfig, configs_merge_up_the_parent_chain) {
  fs::path dir = freshDir("merge");
  writeFile(dir / "sun-config.json",
            "{ \"sunPath\": [\"pdeps\"], "
            "\"pathVariables\": { \"SHARED\": \"common\", \"LIBS\": "
            "\"parentlibs\" } }\n");
  writeFile(dir / "sub" / "sun-config.json",
            "{ \"sunPath\": [\"cdeps\"], "
            "\"pathVariables\": { \"LIBS\": \"libs\" } }\n");

  auto config = sun::SunConfig::findFrom(dir / "sub");
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
            "{ \"pathVariables\": { \"SHARED\": \"common\" } }\n");
  writeFile(dir / "sub" / "sun-config.json",
            "{ \"root\": true, \"pathVariables\": { \"LIBS\": \"libs\" } }\n");

  auto config = sun::SunConfig::findFrom(dir / "sub");
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->pathVariables.count("LIBS"), 1u);
  EXPECT_EQ(config->pathVariables.count("SHARED"), 0u);
}

TEST(Modules_SunConfig, root_must_be_boolean) {
  fs::path dir = freshDir("root_type");
  writeFile(dir / "sun-config.json", "{ \"root\": \"yes\" }\n");
  EXPECT_THROW(sun::SunConfig::loadFile(dir / "sun-config.json"), SunError);
}

TEST(Modules_SunConfig, malformed_config_is_an_error) {
  fs::path dir = freshDir("malformed");
  writeFile(dir / "sun-config.json", "{ not json\n");
  writeFile(dir / "main.sun",
            "manifest { moons: [\"lib.moon\"] }\n"
            "function main() i32 { return 0; }\n");

  EXPECT_THROW(
      sun::ManifestProcessor::fromEntrypointFile((dir / "main.sun").string()),
      SunError);
}

TEST(Modules_SunConfig, unknown_config_key_is_an_error) {
  fs::path dir = freshDir("unknown_key");
  writeFile(dir / "sun-config.json", "{ \"pathVars\": {} }\n");

  try {
    sun::SunConfig::loadFile(dir / "sun-config.json");
    FAIL() << "expected an unknown-key error";
  } catch (const SunError& e) {
    EXPECT_NE(std::string(e.what()).find("pathVars"), std::string::npos);
  }
}

TEST(Modules_SunConfig, absolute_config_entries_are_kept_as_is) {
  fs::path dir = freshDir("absolute");
  writeFile(dir / "sun-config.json",
            "{ \"sunPath\": [\"/opt/sun\"], "
            "\"pathVariables\": { \"LIBS\": \"/opt/libs\" } }\n");

  auto config = sun::SunConfig::loadFile(dir / "sun-config.json");
  ASSERT_EQ(config.sunPath.size(), 1u);
  EXPECT_EQ(config.sunPath[0], "/opt/sun");
  EXPECT_EQ(config.pathVariables.at("LIBS"), "/opt/libs");
}
