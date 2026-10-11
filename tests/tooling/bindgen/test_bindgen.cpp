/** Exercises C binding generation and configured compilation through the CLI.
 */
#include <gtest/gtest.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

/** Keeps binding fixtures isolated from other tooling tests. */
namespace {
/** Names filesystem operations on fixture paths. */
namespace fs = std::filesystem;
/** Reads source or binary artifacts without interpreting their contents. */
std::string read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}
/** Writes a fixture, creating its parent directory when necessary. */
void write(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
}
/** Captures exit status and separate compiler output streams. */
struct Result {
  int status;
  std::string out;
  std::string err;
  /** Combines output for assertion diagnostics. */
  std::string diagnostics() const { return out + err; }
};
/** Owns one temporary project and restores its environment after each test. */
class Tooling_Bindgen : public testing::Test {
 protected:
  fs::path dir;
  const fs::path compiler = SUN_BINDGEN_TEST_COMPILER;
  std::map<std::string, std::optional<std::string>> environment;

  /** Creates fixtures beneath the workspace, isolated by process and test. */
  void SetUp() override {
    ASSERT_TRUE(fs::exists(compiler));
    dir = fs::current_path() / "tmp" /
          ("bindgen-" + std::to_string(getpid()) + "-" +
           testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::create_directories(dir);
    for (const auto* name :
         {"SUN_PATH", "PATH", "SUN_BINDGEN_TEST_OPTIONAL_SDK"}) {
      const auto* value = std::getenv(name);
      environment[name] =
          value ? std::optional<std::string>(value) : std::nullopt;
    }
    setenv("SUN_PATH", fs::current_path().c_str(), 1);
    write(dir / "sun-config.json", R"({"root":true})");
  }
  /** Restores environment variables and removes artifacts after failures too.
   */
  void TearDown() override {
    for (const auto& [name, value] : environment) {
      if (value)
        setenv(name.c_str(), value->c_str(), 1);
      else
        unsetenv(name.c_str());
    }
    if (!dir.empty()) fs::remove_all(dir);
  }
  /** Executes the built compiler directly, with captured output and a timeout.
   */
  Result run(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), compiler.string());
    std::vector<llvm::StringRef> args(arguments.begin(), arguments.end());
    const auto out = (dir / "stdout").string();
    const auto err = (dir / "stderr").string();
    write(out, "");
    write(err, "");
    std::string error;
    const int status = llvm::sys::ExecuteAndWait(
        compiler.string(), args, std::nullopt,
        {llvm::StringRef(""), llvm::StringRef(out), llvm::StringRef(err)}, 60,
        0, &error);
    EXPECT_GE(status, 0) << error;
    return {status, read(out), read(err) + error};
  }
  /** Generates selected declarations for one explicit target ABI. */
  Result generate(const std::string& header,
                  const std::vector<std::string>& names,
                  const std::string& target = "x86_64-unknown-linux-gnu") {
    write(dir / "input.h", header);
    std::vector<std::string> args = {
        "bindgen",  (dir / "input.h").string(),
        "--target", target,
        "--module", "native",
        "--output", (dir / "bindings.sun").string()};
    for (const auto& name : names) {
      args.push_back("--allow");
      args.push_back(name);
    }
    return run(std::move(args));
  }
  /** Compiles bindings without requiring a target linker or runner. */
  Result compile(const fs::path& source, const std::string& target) {
    return run({"--target", target, "--emit-obj", "-o",
                (dir / "bindings.o").string(), source.string()});
  }
  /** Writes JSON without shell quoting or escaping fixture paths manually. */
  void config(llvm::json::Object object, const fs::path& folder = {}) {
    std::string text;
    llvm::raw_string_ostream stream(text);
    stream << llvm::json::Value(std::move(object));
    write((folder.empty() ? dir : folder) / "sun-config.json", text);
  }
  /** Configures a module, supplying a default output for each selected branch.
   */
  void configure(llvm::json::Object binding) {
    if (binding.get("header") && !binding.get("output"))
      binding["output"] = "generated/native.sun";
    if (auto* targets = binding.getObject("target"))
      for (auto& [target, value] : *targets)
        if (!value.getAsObject()->get("output"))
          (*value.getAsObject())["output"] = "generated/native.sun";
    config(llvm::json::Object{
        {"root", true},
        {"c_bindings", llvm::json::Object{{"native", std::move(binding)}}}});
  }
  /** Provides a minimal binding config for a named record. */
  llvm::json::Object binding(const std::string& header = "native.h",
                             const std::string& record = "Reading") {
    return llvm::json::Object{{"header", header},
                              {"allow", llvm::json::Array{record}}};
  }
  /** Writes a program that either consumes the generated record or exits. */
  fs::path mainSource(bool usesBinding = false) {
    const auto source = dir / "main.sun";
    write(source,
          usesBinding
              ? "manifest { source_files: [\"generated/native.sun\"] }\n"
                "/** Reports the native record size. */\n"
                "function main() i32 { return "
                "_convert<i32>(_sizeof<native.Reading>()); }\n"
              : "/** Exits successfully. */\nfunction main() i32 { return 0; "
                "}\n");
    return source;
  }
  /** Builds a native object, optionally bypassing the build cache. */
  Result object(const fs::path& source, bool force = false) {
    std::vector<std::string> args = {
        "--emit-obj", "-o", (dir / "main.o").string(), source.string()};
    if (force) args.push_back("--force-rebuild");
    return run(std::move(args));
  }
};

/** Allows macOS version aliases while rejecting incompatible target ABIs. */
TEST_F(Tooling_Bindgen, DarwinTargetVersionsAndArchitectures) {
  const auto source = dir / "binding.sun";
  for (const std::string arch : {"arm64", "x86_64"}) {
    SCOPED_TRACE(arch);
    write(source,
          read("stdlib/std/process/bindings/process_darwin_" + arch + ".sun"));
    for (const auto& target :
         {arch + "-apple-darwin25.0.0", arch + "-apple-macosx14.0"}) {
      const auto result = compile(source, target);
      ASSERT_EQ(result.status, 0) << result.diagnostics();
    }
    const std::string other = arch == "arm64" ? "x86_64" : "arm64";
    for (const auto& target :
         {other + "-apple-darwin", arch + "-unknown-linux-gnu",
          arch + "-apple-ios17.0", arch + "-apple-macosx14.0-simulator",
          arch + "-unknown-darwin",
          arch == "arm64" ? std::string("arm64e-apple-darwin")
                          : std::string("x86_64h-apple-darwin")}) {
      SCOPED_TRACE(target);
      const auto result = compile(source, target);
      EXPECT_NE(result.status, 0);
      EXPECT_NE(result.diagnostics().find("C binding target mismatch"),
                std::string::npos);
    }
  }
  write(source, "_assert_c_target(\"arm64-apple-darwin\");\n");
  const auto result = compile(source, "aarch64-apple-macosx14.0");
  EXPECT_EQ(result.status, 0) << result.diagnostics();
}

/** Checks fixture declarations on supported OS and architecture pairs. */
TEST_F(Tooling_Bindgen, SupportedTargets) {
  for (const auto* target :
       {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu",
        "x86_64-apple-darwin", "arm64-apple-darwin"}) {
    SCOPED_TRACE(target);
    auto result = generate(read("tests/fixtures/ffi_alignment.h"),
                           {"SunFfiAlignedOuter", "SunFfiReading",
                            "sun_ffi_aligned_make", "sun_ffi_aligned_take"},
                           target);
    ASSERT_EQ(result.status, 0) << result.diagnostics();
    EXPECT_NE(read(dir / "bindings.sun").find("alignment: 64"),
              std::string::npos);
    result = compile(dir / "bindings.sun", target);
    EXPECT_EQ(result.status, 0) << result.diagnostics();
  }
}

/** Exposes command help, argument errors, paths with spaces, and stdout. */
TEST_F(Tooling_Bindgen, CommandHelpErrorsAndStdout) {
  auto result = run({"bindgen", "--help"});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  EXPECT_NE(result.out.find("usage: sun bindgen"), std::string::npos);
  EXPECT_NE(result.out.find("--allow"), std::string::npos);
  result = run({"bindgen"});
  EXPECT_EQ(result.status, 2);
  EXPECT_NE(result.err.find("required"), std::string::npos);
  EXPECT_TRUE(result.out.empty());
  write(dir / "header with spaces.h", "struct Reading { int value; };\n");
  result = run({"bindgen", (dir / "header with spaces.h").string(), "--target",
                "aarch64-unknown-linux-gnu", "--module", "native", "--allow",
                "Reading", "--output", "-"});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  EXPECT_NE(result.out.find("public class Reading"), std::string::npos);
  EXPECT_NE(result.out.find("_assert_c_layout<Reading>"), std::string::npos);
  EXPECT_TRUE(result.err.empty());
}

/** Resolves canonical types and dependencies without Sun typedef syntax. */
TEST_F(Tooling_Bindgen, TypedefsArraysAndSelfPointers) {
  auto result = generate(R"(
    typedef unsigned long count_t;
    struct Item { count_t count; struct Item *next; int values[3]; };
    int consume(const struct Item *item, count_t count);
  )",
                         {"consume"});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  const auto text = read(dir / "bindings.sun");
  for (const auto* fragment : {"count: u64", "raw_ptr<Item>", "array<i32, 3>"})
    EXPECT_NE(text.find(fragment), std::string::npos);
  result = compile(dir / "bindings.sun", "x86_64-unknown-linux-gnu");
  EXPECT_EQ(result.status, 0) << result.diagnostics();
}

/** Generates bindings with no interpreter or compiler executable on PATH. */
TEST_F(Tooling_Bindgen, GenerationWithoutExternalPrograms) {
  write(dir / "native.h", "struct Reading { int value; };\n");
  setenv("PATH", "", 1);
  const auto result = run({"bindgen", (dir / "native.h").string(),
                           "--target=x86_64-unknown-linux-gnu",
                           "--module=native", "--allow=Reading", "--output=-"});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  EXPECT_NE(result.out.find("public class Reading"), std::string::npos);
  EXPECT_NE(result.out.find("_assert_c_layout<Reading>(4, 4, 0)"),
            std::string::npos);
}

/** Rejects unsupported selected constructs with header locations and no output.
 */
TEST_F(Tooling_Bindgen, Rejections) {
  for (const auto* header :
       {"union Bad { int a; float b; };", "struct Bad { int a: 3; };",
        "struct __attribute__((packed)) Bad { char a; int b; };",
        "struct Bad { _Alignas(32) int a; };", "int Bad(int count, ...);",
        "int Bad();", "int Bad(int (*callback)(int));",
        "struct Bad { long double value; };",
        "struct Bad { int size; int values[]; };",
        "struct Bad { int ptr; int c_ptr; };"}) {
    SCOPED_TRACE(header);
    const auto result = generate(header, {"Bad"});
    EXPECT_NE(result.status, 0);
    EXPECT_NE(result.err.find("input.h"), std::string::npos);
    EXPECT_FALSE(fs::exists(dir / "bindings.sun"));
  }
}

/** Escapes compiler keywords in records, fields, and foreign functions. */
TEST_F(Tooling_Bindgen, ReservedNames) {
  auto result = generate(R"(
    struct class {
      int ptr; int static_ptr; int test_function; int throws;
      int extends; int this; int def;
    };
    int test_function(struct class value);
  )",
                         {"class", "test_function"});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  EXPECT_NE(read(dir / "bindings.sun").find("as \"test_function\""),
            std::string::npos);
  result = compile(dir / "bindings.sun", "x86_64-unknown-linux-gnu");
  EXPECT_EQ(result.status, 0) << result.diagnostics();
}

/** Emits formatted, stable output and ignores unrelated declarations. */
TEST_F(Tooling_Bindgen, DeterminismAndSelection) {
  const std::string header =
      "union Other { int x; float y; }; struct A { int value; }; int b(struct "
      "A value);";
  auto result = generate(header, {"A", "b"});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  const auto expected = read(dir / "bindings.sun");
  EXPECT_TRUE(expected.starts_with(
      "/** Auto-generated by sun bindgen. Do not edit manually. */\n\n"));
  EXPECT_EQ(expected.find("Other"), std::string::npos);
  result = run({"fmt", (dir / "bindings.sun").string()});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  EXPECT_EQ(read(dir / "bindings.sun"), expected);
  result = generate(header, {"b", "A"});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  EXPECT_EQ(read(dir / "bindings.sun"), expected);
}

/** Rejects edited layouts and bindings compiled for a different target. */
TEST_F(Tooling_Bindgen, LayoutAndTargetMismatches) {
  auto result = generate("struct A { char tag; int value; };", {"A"});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  result = compile(dir / "bindings.sun", "aarch64-unknown-linux-gnu");
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.diagnostics().find("C binding target mismatch"),
            std::string::npos);
  auto contents = read(dir / "bindings.sun");
  const auto position = contents.find("value: i32");
  ASSERT_NE(position, std::string::npos);
  contents.replace(position, std::string("value: i32").size(), "value: i64");
  write(dir / "bindings.sun", contents);
  result = compile(dir / "bindings.sun", "x86_64-unknown-linux-gnu");
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.diagnostics().find("C layout mismatch"), std::string::npos);
}

/** Preserves adjusted pointer parameters and assembler symbol names. */
TEST_F(Tooling_Bindgen, AdjustedParametersAndLinkNames) {
  const auto result = generate(
      "int consume(int data[3]) __asm__(\"native_consume\");", {"consume"});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  const auto text = read(dir / "bindings.sun");
  EXPECT_NE(text.find("arg0: raw_ptr<i32>"), std::string::npos);
  EXPECT_NE(text.find("as \"native_consume\""), std::string::npos);
}

/** Invalidates cached objects when nested headers change, preserving unchanged
 * outputs. */
TEST_F(Tooling_Bindgen, ConfigCompilationAndNestedHeaderChanges) {
  write(dir / "include space/reading.h", "struct Reading { int value; };\n");
  write(dir / "sensor.h", "#include <reading.h>\n");
  auto settings = binding("sensor.h");
  settings["clang_args"] = llvm::json::Array{"-Iinclude space"};
  configure(std::move(settings));
  const auto source = mainSource(true);
  auto result = object(source);
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  const auto before = read(dir / "main.o");
  const auto stamp = fs::last_write_time(dir / "main.o");
  const auto generatedStamp = fs::last_write_time(dir / "generated/native.sun");
  result = object(source);
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  EXPECT_EQ(fs::last_write_time(dir / "main.o"), stamp);
  EXPECT_EQ(fs::last_write_time(dir / "generated/native.sun"), generatedStamp);
  write(dir / "include space/reading.h",
        "struct Reading { long long value[3]; };\n");
  result = object(source);
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  EXPECT_NE(read(dir / "main.o"), before);
  result = run({source.string()});
  EXPECT_EQ(result.status, 24) << result.diagnostics();
  for (const auto& entry : fs::directory_iterator(dir))
    if (entry.path().extension() == ".sun") EXPECT_EQ(entry.path(), source);
  config(llvm::json::Object{{"root", true}});
  fs::remove(dir / "sensor.h");
  result = object(source, true);
  EXPECT_EQ(result.status, 0) << result.diagnostics();
}

/** Bundles generated declarations for the selected target without requiring
 * headers downstream. */
TEST_F(Tooling_Bindgen, ConfigBundleAndTargetSelection) {
  write(dir / "native.h", "struct Reading { long value; };\n");
  configure(llvm::json::Object{
      {"target",
       llvm::json::Object{{"aarch64-unknown-linux-gnu", binding()}}}});
  const auto source = dir / "library.sun";
  write(
      source,
      "manifest { source_files: [\"generated/native.sun\"] }\n"
      "/** Exposes a native size. */\npublic module wrapper {\n"
      "/** Returns the record size. */\n"
      "public function size() i64 { return _sizeof<native.Reading>(); }\n}\n");
  auto result = run({"--target", "aarch64-unknown-linux-gnu", "--emit-moon",
                     "-o", (dir / "native.moon").string(), source.string()});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  result = compile(source, "x86_64-unknown-linux-gnu");
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.diagnostics().find("C binding target mismatch"),
            std::string::npos);
  config(llvm::json::Object{{"root", true}}, dir / "consumer");
  fs::remove(dir / "native.h");
  const auto imported = dir / "consumer/main.sun";
  write(imported,
        "manifest { libraries: [\"../native.moon\"] }\n"
        "/** Uses bindings bundled without their header. */\n"
        "function main() i32 { return _convert<i32>(wrapper.size()); }\n");
  result = compile(imported, "aarch64-unknown-linux-gnu");
  EXPECT_EQ(result.status, 0) << result.diagnostics();
}

/** Reports header errors and malformed configuration without building an
 * object. */
TEST_F(Tooling_Bindgen, ConfigGeneratorDiagnostics) {
  const auto source = mainSource();
  write(dir / "native.h", "union Bad { int x; float y; };\n");
  configure(binding("native.h", "Bad"));
  auto result = object(source);
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.diagnostics().find("native.h"), std::string::npos);
  EXPECT_FALSE(fs::exists(dir / "main.o"));
  auto settings = binding("native.h", "Bad");
  settings["allow"] = llvm::json::Array{};
  configure(std::move(settings));
  result = object(source);
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.diagnostics().find("sun-config.json"), std::string::npos);
  settings = binding("native.h", "Bad");
  settings["typo"] = true;
  configure(std::move(settings));
  result = object(source);
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.diagnostics().find("sun-config.json"), std::string::npos);
}

/** Reuses existing bindings without headers but keeps generation errors fatal.
 */
TEST_F(Tooling_Bindgen, OptionalConfigBindingAvailability) {
  write(dir / "native.h", "struct Reading { int value; };\n");
  auto settings = binding();
  settings["skip_if_unavailable"] = true;
  configure(settings);
  const auto source = mainSource(true);
  auto result = object(source);
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  const auto generated = dir / "generated/native.sun";
  const auto original = read(generated);
  const auto stamp = fs::last_write_time(generated);
  fs::remove(dir / "native.h");
  result = object(source, true);
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  EXPECT_EQ(read(generated), original);
  EXPECT_EQ(fs::last_write_time(generated), stamp);
  settings["skip_if_unavailable"] = false;
  configure(settings);
  EXPECT_NE(object(source).status, 0);
  settings["skip_if_unavailable"] = true;
  configure(settings);
  for (const auto* contents : {"union Reading { int x; float y; };\n",
                               "#include \"missing_dependency.h\"\n"}) {
    write(dir / "native.h", contents);
    EXPECT_NE(object(source).status, 0);
    EXPECT_EQ(read(generated), original);
  }
  fs::remove(dir / "native.h");
  fs::remove(generated);
  EXPECT_NE(object(source, true).status, 0);
  EXPECT_FALSE(fs::exists(generated));
}

/** Skips undefined SDK paths, honors overrides, and validates expanded outputs.
 */
TEST_F(Tooling_Bindgen, OptionalConfigBindingSdkVariable) {
  unsetenv("SUN_BINDGEN_TEST_OPTIONAL_SDK");
  auto settings = binding("$SUN_BINDGEN_TEST_OPTIONAL_SDK/native.h");
  settings["skip_if_unavailable"] = true;
  configure(settings);
  const auto source = mainSource();
  auto result = object(source);
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  EXPECT_FALSE(fs::exists(dir / "generated/native.sun"));
  write(dir / "native.h", "struct Reading { int value; };\n");
  result = run({"--emit-obj", "--path-var",
                "SUN_BINDGEN_TEST_OPTIONAL_SDK=" + dir.string(), "-o",
                (dir / "main.o").string(), source.string()});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  EXPECT_TRUE(fs::exists(dir / "generated/native.sun"));
  settings["output"] = "$SUN_BINDGEN_TEST_OUTPUT";
  configure(settings);
  for (const auto* output : {"other.sun", "other.txt"}) {
    result = run({"--emit-obj", "--path-var",
                  "SUN_BINDGEN_TEST_OPTIONAL_SDK=" + dir.string(), "--path-var",
                  "SUN_BINDGEN_TEST_OUTPUT=" + (dir / output).string(), "-o",
                  (dir / "main.o").string(), source.string()});
    EXPECT_EQ(result.status == 0, std::string(output) == "other.sun")
        << result.diagnostics();
    if (std::string(output) == "other.txt")
      EXPECT_NE(result.diagnostics().find("output must name a .sun file"),
                std::string::npos);
  }
  settings["output"] = "generated/native.sun";
  settings["skip_if_unavailable"] = false;
  configure(settings);
  result = object(source);
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.diagnostics().find("undefined path variable"),
            std::string::npos);
  settings["skip_if_unavailable"] = "true";
  configure(settings);
  result = object(source);
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.diagnostics().find("expected a boolean"), std::string::npos);
}

/** Resolves inherited paths and uses the nearest module definition in Sun
 * tests. */
TEST_F(Tooling_Bindgen, ConfigInheritanceAndTestCommand) {
  write(dir / "native.h", "struct Reading { int value; };\n");
  configure(binding());
  const auto child = dir / "src";
  const auto source = child / "tests.sun";
  write(source,
        "manifest { source_files: [\"../generated/native.sun\"] }\n"
        "/** Checks generated bindings in the test runner. */\n"
        "test_function reading_size() { var size = _sizeof<native.Reading>(); "
        "}\n");
  auto result =
      run({"test", "--moon", (compiler.parent_path() / "stdlib.moon").string(),
           source.string()});
  ASSERT_EQ(result.status, 0) << result.diagnostics();
  write(child / "native.h", "struct Other { int value; };\n");
  auto settings = binding("native.h", "Other");
  settings["output"] = "../generated/native.sun";
  config(
      llvm::json::Object{
          {"c_bindings", llvm::json::Object{{"native", std::move(settings)}}}},
      child);
  write(source,
        "manifest { source_files: [\"../generated/native.sun\"] }\n"
        "/** Uses the nearest config's module definition. */\n"
        "function main() i32 { return _convert<i32>(_sizeof<native.Other>()); "
        "}\n");
  result = run({source.string()});
  EXPECT_EQ(result.status, 4) << result.diagnostics();
}

/** Requires explicit manifest imports and preserves output after failed
 * generation. */
TEST_F(Tooling_Bindgen, GeneratedFileRequiresManifestEntry) {
  write(dir / "native.h", "struct Reading { int value; };\n");
  configure(binding());
  const auto source = dir / "main.sun";
  write(source,
        "/** Refers to a module absent from the manifest. */\n"
        "function main() i32 { return "
        "_convert<i32>(_sizeof<native.Reading>()); }\n");
  EXPECT_NE(object(source).status, 0);
  const auto generated = dir / "generated/native.sun";
  ASSERT_TRUE(fs::exists(generated));
  const auto original = read(generated);
  write(dir / "native.h", "union Reading { int value; float other; };\n");
  EXPECT_NE(object(source).status, 0);
  EXPECT_EQ(read(generated), original);
}

/** Rejects missing outputs and collisions before generating any files. */
TEST_F(Tooling_Bindgen, ConfigRequiresUniqueExplicitOutputs) {
  write(dir / "native.h", "struct Reading { int value; };\n");
  const auto source = mainSource();
  auto settings = binding();
  config(llvm::json::Object{
      {"root", true},
      {"c_bindings", llvm::json::Object{{"a", llvm::json::Object(settings)}}}});
  auto result = object(source);
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.diagnostics().find("require header, output and allow"),
            std::string::npos);
  settings["output"] = "generated/shared.sun";
  config(llvm::json::Object{
      {"root", true},
      {"c_bindings", llvm::json::Object{{"a", llvm::json::Object(settings)},
                                        {"b", llvm::json::Object(settings)}}}});
  result = object(source);
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.diagnostics().find("share the same output"),
            std::string::npos);
  EXPECT_FALSE(fs::exists(dir / "generated/shared.sun"));
}
}  // namespace
