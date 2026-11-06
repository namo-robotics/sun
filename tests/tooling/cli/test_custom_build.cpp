#include <gtest/gtest.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "driver/sun_config.h"

/** Exercises custom scripts through the compiler executable and real child
 * processes. */
namespace {
/** Shortens fixture filesystem operations. */
namespace fs = std::filesystem;

/** Reads a fixture file, including binary artifacts used for publication
 * checks. */
std::string read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

/** Parses a fixture or compiler response and reports malformed JSON as a test
 * failure. */
llvm::json::Object document(const std::string& text) {
  auto value = llvm::json::parse(text);
  if (!value) {
    throw std::runtime_error(llvm::toString(value.takeError()));
  }
  if (!value->getAsObject()) {
    throw std::runtime_error("expected a JSON object");
  }
  return std::move(*value->getAsObject());
}

/** Owns a composed project and restores environment changes after each test. */
class CustomBuild : public testing::Test {
 protected:
  fs::path root, compiler;
  llvm::json::Object config;
  std::map<std::string, std::optional<std::string>> environment;

  /** Creates a local library and an app built by three ordered scripts. */
  void SetUp() override {
    compiler = fs::absolute("build/sun");
    root = fs::current_path() / "tmp/custom build tests" /
           testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::remove_all(root);
    fs::create_directories(root);
    for (const auto* name : {"SUN_BUILD_CONTEXT", "SUN_DEPENDENCY_CACHE"}) {
      const auto* previous = std::getenv(name);
      environment[name] =
          previous ? std::optional<std::string>(previous) : std::nullopt;
    }
    unsetenv("SUN_BUILD_CONTEXT");
    setenv("SUN_DEPENDENCY_CACHE", (root / "cache").c_str(), 1);
    write("sun-config.json",
          R"({"projects":["lib/sun-config.json","app/sun-config.json"]})");
    write("lib/sun-config.json",
          R"({"package_id":"example/helper","entrypoints":[
      {"name":"helper","path":"helper.sun","type":"library","export":true}]})");
    write("lib/helper.sun", R"(manifest { id: "example/helper" }
      /** Provides a fixture value. */ public module helper {
        /** Returns the expected value. */ public function answer() i32 { return 42; }
      })");
    config = document(R"({"package_id":"example/app",
      "dependencies":[{"package":"example/helper","library":"example/helper","path":"missing.moon"}],
      "entrypoints":[{"name":"app","path":"main.sun","export":true,
        "build":["scripts/prepare.sh","scripts/compile.sh","scripts/finish.sh"]}]})");
    save();
    write("app/main.sun",
          R"(manifest { libraries: ["${example/helper}/lib/helper.moon"] }
      /** Verifies the local dependency. */ function main() i32 { return helper.answer() - 42; })");
    script("prepare.sh", R"sh(test -f main.sun
"$SUN_EXECUTABLE" dependency-path example/helper > "$SUN_BUILD/dependency"
printf prepare > "$SUN_BUILD/order"
export SCRIPT_PRIVATE=changed
)sh");
    script("compile.sh", R"sh(test "${SCRIPT_PRIVATE:-}" != changed
test "$(cat "$SUN_BUILD/order")" = prepare
"$SUN_EXECUTABLE" build-compile --dynamic
printf compile >> "$SUN_BUILD/order"
)sh");
    script("finish.sh",
           "test -f \"$SUN_OUTPUT\"\nprintf finish >> \"$SUN_BUILD/order\"\n");
  }

  /** Restores the parent environment and removes this test's generated outputs.
   */
  void TearDown() override {
    for (const auto& [name, value] : environment) {
      if (value) {
        setenv(name.c_str(), value->c_str(), 1);
      } else {
        unsetenv(name.c_str());
      }
    }
    if (!HasFailure()) {
      fs::remove_all(root);
    }
  }

  /** Writes a file beneath the isolated fixture root. */
  void write(const fs::path& path, const std::string& contents) {
    fs::create_directories((root / path).parent_path());
    std::ofstream(root / path) << contents;
  }

  /** Creates an executable script with an explicit interpreter. */
  void script(const std::string& name, const std::string& body) {
    write("app/scripts/" + name, "#!/bin/sh\nset -eu\n" + body);
    fs::permissions(root / "app/scripts" / name, fs::perms::owner_all);
  }

  /** Returns the consumer entrypoint so individual tests can adjust its build.
   */
  llvm::json::Object& entry() {
    return *config.getArray("entrypoints")->front().getAsObject();
  }

  /** Persists the consumer configuration for the next invocation. */
  void save() {
    std::string text;
    llvm::raw_string_ostream(text)
        << llvm::json::Value(llvm::json::Object(config));
    write("app/sun-config.json", text);
  }

  /** Runs a child without shell parsing, with a timeout and captured
   * diagnostics. */
  int invoke(const fs::path& program,
             const std::vector<std::string>& arguments) {
    const auto executable = program.string();
    std::vector<llvm::StringRef> refs{executable};
    for (const auto& argument : arguments) {
      refs.push_back(argument);
    }
    const auto out = (root / "stdout").string(),
               err = (root / "stderr").string();
    std::ofstream(out).close();
    std::ofstream(err).close();
    std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), out,
                                                  err};
    return llvm::sys::ExecuteAndWait(executable, refs, std::nullopt, redirects,
                                     60);
  }

  /** Combines captured output for assertion diagnostics. */
  std::string diagnostics() {
    return read(root / "stdout") + read(root / "stderr");
  }

  /** Runs an operation on the composed consumer project. */
  int command(const std::string& operation,
              const std::vector<std::string>& flags = {}) {
    std::vector<std::string> args{operation, "--config",
                                  (root / "sun-config.json").string(),
                                  "--package", "example/app"};
    if (operation == "build") {
      args.push_back("--no-test");
    }
    args.insert(args.end(), flags.begin(), flags.end());
    return invoke(compiler, args);
  }

  /** Reads output locations for the selected target and configuration.
   */
  llvm::json::Object describe(const std::vector<std::string>& flags = {}) {
    if (command("describe", flags) != 0) {
      throw std::runtime_error(diagnostics());
    }
    return document(read(root / "stdout"));
  }

  /** Selects a supported foreign target distinct from the host architecture. */
  std::string foreignTarget() {
    return sun::driver::configTargetKey(llvm::sys::getDefaultTargetTriple()) ==
                   sun::driver::configTargetKey("x86_64-linux-gnu")
               ? "aarch64-linux-gnu"
               : "x86_64-linux-gnu";
  }

  /** Replaces the provider with a generated executable for cross-build checks.
   */
  void toolProvider(bool targetOnly) {
    auto provider = document(R"({"package_id":"example/helper","entrypoints":[
      {"name":"helper","path":"helper.sun","export":true,"build":["make_tool.sh"]}]})");
    if (targetOnly) {
      (*provider.getArray("entrypoints")->front().getAsObject())["enabled"] =
          llvm::json::Object{
              {"default", false},
              {"target", llvm::json::Object{{foreignTarget(), true}}}};
    }
    std::string text;
    llvm::raw_string_ostream(text) << llvm::json::Value(std::move(provider));
    write("lib/sun-config.json", text);
    write("lib/make_tool.sh", R"sh(#!/bin/sh
set -eu
printf '#!/bin/sh\nprintf "%%s\\n" "%s"\n' "$SUN_TARGET" > "$SUN_OUTPUT"
)sh");
    fs::permissions(root / "lib/make_tool.sh", fs::perms::owner_all);
    config["dependencies"] =
        llvm::json::Array{llvm::json::Object{{"package", "example/helper"},
                                             {"entrypoints", llvm::json::Array{"helper"}},
                                             {"path", "missing.moon"}}};
    entry()["type"] = "library";
    save();
    write("app/main.sun", "/** A target library. */ public module api {}\n");
  }
};

/** Scripts share context and files, preserve flags, and retain incremental
 * compilation. */
TEST_F(CustomBuild, OrderContextAndLocalDependency) {
  ASSERT_EQ(command("build", {"-g", "-O0"}), 0) << diagnostics();
  auto selected = describe({"-g", "-O0"});
  const fs::path output(selected.getString("build_root")->str());
  EXPECT_EQ(read(output / "order"), "preparecompilefinish");
  const auto dependency =
      llvm::StringRef(read(output / "dependency")).trim().str();
  EXPECT_TRUE(fs::is_regular_file(fs::path(dependency) / "lib/helper.moon"));
  auto context = document(read(output / ".contexts/app.json"));
  EXPECT_NE(read(output / ".contexts/app.json").find("-O0"), std::string::npos);
  EXPECT_NE(read(output / ".contexts/app.json").find("-g"), std::string::npos);
  const fs::path artifact(context.getString("output")->str());
  EXPECT_EQ(
      invoke(fs::path(selected.getString("install_root")->str()) / "bin/app",
             {}),
      0);
  const auto timestamp = fs::last_write_time(artifact);
  ASSERT_EQ(command("build", {"-g", "-O0"}), 0) << diagnostics();
  EXPECT_EQ(fs::last_write_time(artifact), timestamp);
}

/** Script failure stops the sequence; missing outputs cannot replace the
 * installed artifact. */
TEST_F(CustomBuild, FailurePreservesInstallation) {
  ASSERT_EQ(command("build"), 0) << diagnostics();
  auto selected = describe();
  const fs::path output(selected.getString("build_root")->str());
  const auto installed =
      fs::path(selected.getString("install_root")->str()) / "bin/app";
  const auto previous = read(installed);
  script("prepare.sh", "printf stopped > \"$SUN_BUILD/order\"\nexit 7\n");
  EXPECT_NE(command("build"), 0);
  EXPECT_NE(diagnostics().find("build script failed"), std::string::npos);
  EXPECT_EQ(read(output / "order"), "stopped");
  EXPECT_EQ(read(installed), previous);
  entry()["build"] = llvm::json::Array{"scripts/prepare.sh"};
  save();
  script("prepare.sh", "exit 0\n");
  fs::remove(output / "app");
  EXPECT_NE(command("build"), 0);
  EXPECT_NE(diagnostics().find("build did not produce"), std::string::npos);
  EXPECT_EQ(read(installed), previous);
}

/** Shell queries return selected project values without building products. */
TEST_F(CustomBuild, DescribeFieldPrintsPlainText) {
  const std::vector<std::string> flags{"--target", foreignTarget(), "-g", "-O0"};
  auto selected = describe(flags);
  for (const auto* field : {"build_root", "install_root", "package_id"}) {
    auto arguments = flags;
    arguments.insert(arguments.end(), {"--field", field});
    ASSERT_EQ(command("describe", arguments), 0) << diagnostics();
    EXPECT_EQ(read(root / "stdout"), selected.getString(field)->str() + "\n");
  }
  EXPECT_FALSE(fs::exists(selected.getString("build_root")->str()));
  EXPECT_NE(command("describe", {"--field", "entrypoints"}), 0);
  EXPECT_NE(diagnostics().find("--field expects"), std::string::npos);
  EXPECT_NE(command("describe", {"--field"}), 0);
  EXPECT_NE(diagnostics().find("--field requires a value"), std::string::npos);
}

/** Build scripts require an array, cannot mix build modes, and require an
 * inherited context. */
TEST_F(CustomBuild, InvalidSchemaAndMissingContext) {
  for (const auto* json :
       {"[]", "\"scripts/prepare.sh\"", "[\"\"]", "[\"/bin/sh\"]", "[42]"}) {
    auto value = llvm::json::parse(json);
    ASSERT_TRUE(static_cast<bool>(value));
    entry()["build"] = std::move(*value);
    save();
    EXPECT_NE(command("build"), 0) << json;
  }
  entry()["build"] = llvm::json::Array{"scripts/prepare.sh"};
  entry()["compile_args"] = llvm::json::Array{"--dynamic"};
  save();
  EXPECT_NE(command("build"), 0);
  EXPECT_NE(diagnostics().find("build cannot be combined"), std::string::npos);
  EXPECT_NE(invoke(compiler, {"dependency-path", "example/helper"}), 0);
  EXPECT_NE(diagnostics().find("requires a custom build context"),
            std::string::npos);
}

/** The compiler never silently chooses an interpreter for a non-executable
 * script. */
TEST_F(CustomBuild, ScriptMustBeExecutable) {
  fs::permissions(root / "app/scripts/prepare.sh",
                  fs::perms::owner_read | fs::perms::owner_write);
  EXPECT_NE(command("build"), 0);
  EXPECT_NE(diagnostics().find("build script failed"), std::string::npos);
}

/** A foreign library can run a host generator through the pre-resolved context.
 */
TEST_F(CustomBuild, HostToolAndForeignLibrary) {
  toolProvider(false);
  script("prepare.sh",
         R"sh(tool=$("$SUN_EXECUTABLE" dependency-path --host example/helper)
"$tool/bin/helper" > "$SUN_BUILD/tool_target"
printf prepare > "$SUN_BUILD/order"
)sh");
  ASSERT_EQ(command("build", {"--target", foreignTarget()}), 0)
      << diagnostics();
  auto selected = describe({"--target", foreignTarget()});
  const fs::path output(selected.getString("build_root")->str());
  auto context = document(read(output / ".contexts/app.json"));
  EXPECT_NE(llvm::StringRef(read(output / "tool_target")).trim(),
            *context.getString("target"));
  EXPECT_TRUE(fs::is_regular_file(
      fs::path(selected.getString("install_root")->str()) / "lib/app.moon"));
}

/** Shipping a target-only binary does not require an unused host variant. */
TEST_F(CustomBuild, TargetOnlyBinaryNeedsNoHostVariant) {
  toolProvider(true);
  script("prepare.sh", "printf prepare > \"$SUN_BUILD/order\"\n");
  ASSERT_EQ(command("build", {"--target", foreignTarget()}), 0)
      << diagnostics();
  auto selected = describe({"--target", foreignTarget()});
  auto context =
      document(read(fs::path(selected.getString("build_root")->str()) /
                    ".contexts/app.json"));
  EXPECT_TRUE(context.getObject("host_errors")->getString("example/helper"));
  EXPECT_FALSE(
      context.getObject("host_dependencies")->getString("example/helper"));
}

}  // namespace


/** Build scripts keep the workspace display base despite changing directories. */
TEST_F(CustomBuild, WorkspaceRelativeBuildPaths) {
  ASSERT_EQ(command("build"), 0) << diagnostics();
  const auto log = diagnostics();
  EXPECT_NE(log.find("Building library: lib/helper.sun ->"), std::string::npos) << log;
  EXPECT_NE(log.find("Building: app/main.sun ->"), std::string::npos) << log;
  EXPECT_EQ(log.find(root.string()), std::string::npos) << log;
  auto selected = describe();
  const fs::path buildRoot(selected.getString("build_root")->str());
  EXPECT_TRUE(buildRoot.is_absolute());
  auto context = document(read(buildRoot / ".contexts/app.json"));
  ASSERT_TRUE(context.getString("display_root"));
  EXPECT_EQ(context.getString("display_root")->str(), root.string());
}

/** Diagnostics from custom compiler children use the selected config base. */
TEST_F(CustomBuild, WorkspaceRelativeErrorPaths) {
  write("app/main.sun", "/** Requires an unavailable interface. */\n"
                        "class Broken implements Missing {}\n");
  EXPECT_NE(command("build"), 0);
  const auto log = diagnostics();
  EXPECT_NE(log.find("app/main.sun:2:25"), std::string::npos) << log;
  EXPECT_NE(log.find("class 'Broken'"), std::string::npos) << log;
  EXPECT_EQ(log.find((root / "app/main.sun").string()), std::string::npos) << log;
}

/** A direct source command displays files relative to its invocation directory. */
TEST_F(CustomBuild, DirectSourceUsesInvocationDirectory) {
  write("broken.sun", "/** Requires an unavailable interface. */\n"
                      "class Broken implements Missing {}\n");
  EXPECT_NE(invoke(compiler, {"-c", (root / "broken.sun").string(), "--no-test"}), 0);
  const auto expected = (root / "broken.sun").lexically_relative(fs::current_path()).string();
  const auto log = diagnostics();
  EXPECT_NE(log.find(expected + ":2:25"), std::string::npos) << log;
  EXPECT_EQ(log.find((root / "broken.sun").string()), std::string::npos) << log;
}
