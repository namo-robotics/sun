/** Exercises checked library revisions through packaging, imports, and JIT. */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "abi/abi_compatibility_checker.h"
#include "abi/library_abi_snapshot.h"
#include "driver/driver.h"
#include "driver/execution_utils.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_builder.h"
#include "support/error.h"

/** Keeps revision fixtures isolated from other compiler tests. */
namespace {
/** Builds independent revisions and consumers without modifying old bundles. */
class ABI_LibraryAbiSnapshot : public testing::Test {
 protected:
  std::filesystem::path directory;
  /** Creates a per-test workspace and initializes compiler dependencies. */
  void SetUp() override {
    sun::driver::initTestEnvironment();
    directory = std::filesystem::current_path() / "tmp" / "library_abis" /
                testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "sun-config.json") << R"({"root":true})";
  }
  /** Packages source with an explicit dependency list. */
  std::string build(const std::string& name, const std::string& source,
                    const std::vector<std::string>& dependencies = {},
                    const std::string& identity = "") {
    const auto path = directory / (name + ".sun");
    std::ofstream(path) << source;
    sun::moon_bundling::MoonBuildOptions options;
    options.artifactId = identity;
    for (const auto& dependency : dependencies) {
      options.extraMoons.emplace_back(dependency);
    }
    const auto output = directory / (name + ".moon");
    sun::moon_bundling::MoonBuilder::build(path.string(), output, options);
    return output.string();
  }
  /** Runs a consumer using exactly the selected revisions. */
  sun::driver::SunValue run(const std::vector<std::string>& dependencies,
                            const std::string& source) {
    auto driver = sun::driver::Driver::createForJIT("revision");
    std::vector<sun::moon_bundling::MoonImport> imports;
    for (const auto& dependency : dependencies) {
      imports.emplace_back(dependency);
    }
    driver->setMoonImports(imports);
    return driver->executeString(source);
  }
  /** Builds a Point revision with a chosen identity, body, and layout. */
  std::string buildShapes(const std::string& name, int value,
                          const std::string& identity = "example/shapes",
                          bool extraField = false) {
    const auto source =
        "/** Exports a point. */ public module shapes { "
        "/** Stores coordinates. */ public class Point { public var x: "
        "i32; " +
        (extraField ? std::string("public var y: i32; ") : std::string()) +
        "/** Initializes coordinates. */ init() { this.x = " +
        std::to_string(value) + "; " + (extraField ? "this.y = 0; " : "") +
        "} } }";
    return build(name, source, {}, identity);
  }
  /** Builds a precompiled borrower of the original Point declaration. */
  std::string consumer(const std::string& original) {
    return build("reader", R"(
      /** Reads an imported point without rebuilding its implementation. */
      public module reader {
        /** Returns the stored coordinate. */
        public function read(point: ref shapes.Point) i32 { return point.x; }
        /** Constructs and reads using the embedded revision. */
        public function local() i32 { var p = shapes.Point(); return p.x; }
      }
    )",
                 {original});
  }
  /** Uses the selected constructor and the previously compiled reader. */
  static std::string application() {
    return R"(
      /** Passes the replacement point across an existing compiled boundary. */
      function main() i32 { var p = shapes.Point(); return reader.read(p); }
    )";
  }
};
}  // namespace

/** Legacy hashes tolerate advisory differences but retain semantic
 * requirements. */
TEST_F(ABI_LibraryAbiSnapshot,
       MatchesLegacyHashesWithoutWeakeningDeclarations) {
  const auto original = buildShapes("original", 42);
  const auto changed = buildShapes("changed", 43, "example/shapes", true);
  auto originalReader = sun::moon_bundling::MoonReader::open(original);
  auto changedReader = sun::moon_bundling::MoonReader::open(changed);
  ASSERT_TRUE(originalReader && changedReader);
  std::vector<const sun::moon::ModuleMetadata*> modules;
  for (const auto& key : originalReader->listModules()) {
    modules.push_back(originalReader->getMetadata(key));
  }
  auto selected = originalReader->getLibrary().bundle_contract();
  *selected.mutable_target() =
      sun::abi::compatibilityTarget("arm64-apple-darwin");
  auto retaining = selected;
  retaining.mutable_target()->set_platform_abi("darwin25.6.0:");
  for (auto& rule : *retaining.mutable_requirements()) {
    if (rule.name() == "sun.experimental.semantics") {
      rule.set_implementation(std::string(64, 'a'));
    }
  }
  const auto artifact = originalReader->getLibrary().content_hash();
  const auto required =
      sun::abi::describeLibraryAbiSnapshot(retaining, artifact, modules);
  EXPECT_NE(required.abi_hash(),
            sun::abi::describeLibraryAbiSnapshot(selected, artifact, modules)
                .abi_hash());
  EXPECT_TRUE(sun::abi::matchesLibraryAbiSnapshot(required, selected, artifact,
                                                  modules, retaining));

  std::vector<const sun::moon::ModuleMetadata*> changedModules;
  for (const auto& key : changedReader->listModules()) {
    changedModules.push_back(changedReader->getMetadata(key));
  }
  EXPECT_FALSE(sun::abi::matchesLibraryAbiSnapshot(
      required, selected, changedReader->getLibrary().content_hash(),
      changedModules, retaining));
  auto incompatible = selected;
  incompatible.mutable_target()->add_cpu_features("neon");
  EXPECT_FALSE(sun::abi::matchesLibraryAbiSnapshot(
      required, incompatible, artifact, modules, retaining));
  incompatible = selected;
  for (auto& rule : *incompatible.mutable_requirements()) {
    if (rule.name() == "moon.interface") {
      rule.set_implementation("unsupported-interface");
    }
  }
  EXPECT_FALSE(sun::abi::matchesLibraryAbiSnapshot(
      required, incompatible, artifact, modules, retaining));
}

/** Exact artifacts share one declaration set regardless of manifest order. */
TEST_F(ABI_LibraryAbiSnapshot, SharesExactDependencyInBothOrders) {
  const auto original = buildShapes("original", 42);
  const auto reader = consumer(original);
  EXPECT_EQ(run({reader, original}, application()), 42);
  EXPECT_EQ(run({original, reader}, application()), 42);
}

/** Internal revisions do not constrain an independently listed library. */
TEST_F(ABI_LibraryAbiSnapshot, KeepsDifferentVersionsIndependent) {
  const auto original = buildShapes("original", 42);
  const auto reader = consumer(original);
  for (bool changedLayout : {false, true}) {
    const auto newer = buildShapes(changedLayout ? "changed" : "newer", 43,
                                   "example/shapes", changedLayout);
    const std::string app = R"(
      /** Uses each revision without passing its types to the other. */
      function main() i32 {
        var p = shapes.Point();
        return reader.local() + p.x;
      }
    )";
    EXPECT_EQ(run({reader, newer}, app), 85);
    EXPECT_EQ(run({newer, reader}, app), 85);
  }
}

/** Compatible arguments cross an API without replacing embedded
 * implementations. */
TEST_F(ABI_LibraryAbiSnapshot, AcceptsCompatibleArgumentsAtCallSite) {
  const auto original = buildShapes("original", 42);
  const auto reader = consumer(original);
  const auto newer = buildShapes("newer", 43);
  for (const auto& imports : {std::vector<std::string>{reader, newer},
                              std::vector<std::string>{newer, reader}}) {
    EXPECT_EQ(run(imports, application()), 43);
    EXPECT_EQ(run(imports, R"(
      /** Reads the new value while preserving the original constructor. */
      function main() i32 {
        var p = shapes.Point();
        return reader.read(p) + reader.local();
      }
    )"),
              85);
  }
}

/** Layout changes and unrelated library identities fail at the call site. */
TEST_F(ABI_LibraryAbiSnapshot, RejectsIncompatibleArgumentsAtCallSite) {
  const auto original = buildShapes("original", 42);
  const auto reader = consumer(original);
  const auto changed = buildShapes("changed", 43, "example/shapes", true);
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({reader, changed}, application()), "read");
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({changed, reader}, application()), "read");
  const auto unrelated = buildShapes("unrelated", 43, "other/shapes");
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({reader, unrelated}, application()),
                                "read");
}

/** Borrow compatibility preserves constness and applies to retained generics.
 */
TEST_F(ABI_LibraryAbiSnapshot,
       CompatibleBorrowPreservesMutabilityAndGenericCode) {
  const auto original = buildShapes("original", 42);
  const auto api = build("api", R"(
    /** Reads through generic and const borrowed boundaries. */
    public module api {
      /** Reads a point through its original identity when specialized. */
      public function read<T>(point: const ref shapes.Point) i32 { return point.x; }
      /** Mutates a compatible caller-owned point. */
      public function update(point: ref shapes.Point) void { point.x = 44; }
    }
  )",
                         {original});
  const auto newer = buildShapes("newer", 43);
  EXPECT_EQ(run({api, newer}, R"(
    /** Borrows the newer value through the original generic API. */
    function main() i32 {
      var p = shapes.Point();
      api.update(p);
      return api.read<i32>(p);
    }
  )"),
            44);
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({api, newer}, R"(
    /** A const borrow must not gain mutation rights across revisions. */
    function modify(point: const ref shapes.Point) void { api.update(point); }
    /** Supplies a point to the checked boundary. */
    function main() i32 { var p = shapes.Point(); modify(p); return 0; }
  )"),
                                "update");
}

/** Two hidden compatible revisions can borrow without exposing their names. */
TEST_F(ABI_LibraryAbiSnapshot, CompatibleBorrowBetweenEmbeddedRevisions) {
  const auto first = buildShapes("first", 42);
  const auto second = buildShapes("second", 43);
  const auto reader = consumer(first);
  const auto factory = build("factory", R"(
    /** Returns a value from its own embedded revision. */
    public module factory {
      /** Constructs with the second revision's implementation. */
      public function make() shapes.Point { return shapes.Point(); }
    }
  )",
                             {second});
  std::filesystem::remove(first);
  std::filesystem::remove(second);
  EXPECT_EQ(run({reader, factory}, R"(
    /** Passes an inferred value without naming the hidden dependency. */
    function main() i32 { var p = factory.make(); return reader.read(p); }
  )"),
            43);
}

/** Import validation accepts distinct revisions without creating aliases. */
TEST_F(ABI_LibraryAbiSnapshot, ValidatesCoexistingRevisionsWithoutWarnings) {
  const auto original = buildShapes("original", 42);
  const auto consumerPath = consumer(original);
  const auto changed = buildShapes("changed", 43, "example/shapes", true);
  auto consumerReader = sun::moon_bundling::MoonReader::open(consumerPath);
  auto changedReader = sun::moon_bundling::MoonReader::open(changed);
  ASSERT_TRUE(consumerReader && changedReader);
  sun::abi::CompilationContext context;
  context.producerWarnings = false;
  sun::abi::AbiCompatibilityChecker checker(context);
  testing::internal::CaptureStderr();
  EXPECT_NO_THROW(
      checker.validateImports({consumerReader.get(), changedReader.get()}));
  EXPECT_NO_THROW(
      checker.validateImports({changedReader.get(), consumerReader.get()}));
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
}

/** Independent wrappers retain different implementations of the same library.
 */
TEST_F(ABI_LibraryAbiSnapshot,
       KeepsTwoEmbeddedVersionsAfterDependenciesAreRemoved) {
  const auto first = buildShapes("first", 42);
  const auto second = buildShapes("second", 43, "example/shapes", true);
  const auto left = consumer(first);
  const auto right = build("right", R"(
    /** Uses a second embedded revision. */
    public module right {
      /** Constructs using this wrapper's revision. */
      public function local() i32 { var p = shapes.Point(); return p.x; }
    }
  )",
                           {second});
  std::filesystem::remove(first);
  std::filesystem::remove(second);
  EXPECT_EQ(run({left, right}, R"(
    /** Calls wrappers whose implementation types have different layouts. */
    function main() i32 { return reader.local() + right.local(); }
  )"),
            85);
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({left, right}, R"(
    /** Attempts to name an unlisted implementation module. */
    function main() i32 { var p = shapes.Point(); return p.x; }
  )"),
                                "shapes");
}

/** Transitive wrappers preserve original identities instead of selecting a
 * revision. */
TEST_F(ABI_LibraryAbiSnapshot, PreservesTransitiveDependencyIdentity) {
  const auto original = buildShapes("original", 42);
  const auto reader = consumer(original);
  const auto wrapper = build("wrapper", R"(
    /** Wraps a reader of the original Point declaration. */
    public module wrapper {
      /** Passes a point to the original reader. */
      public function read(point: ref shapes.Point) i32 { return reader.read(point); }
      /** Executes the original implementation. */
      public function local() i32 { return reader.local(); }
    }
  )",
                             {reader, original});
  const auto newer = buildShapes("newer", 43);
  EXPECT_EQ(run({wrapper, newer}, R"(
    /** Uses the explicit and embedded versions independently. */
    function main() i32 { var p = shapes.Point(); return wrapper.local() + p.x; }
  )"),
            85);
  EXPECT_EQ(run({wrapper, newer}, R"(
    /** Passes an ABI-compatible point to the original reader. */
    function main() i32 { var p = shapes.Point(); return wrapper.read(p); }
  )"),
            43);
}

/** Generic bodies use their original dependency despite an explicit newer
 * revision. */
TEST_F(ABI_LibraryAbiSnapshot, SpecializesAgainstTheEmbeddedRevision) {
  const auto original = buildShapes("original", 42);
  const auto factory = build("factory", R"(
    /** Creates original points when specialized by a consumer. */
    public module factory {
      /** Calls the constructor from this bundle's dependency. */
      public function make<T>() shapes.Point { return shapes.Point(); }
      /** Reads a point with the exact original identity. */
      public function read<T>(point: ref shapes.Point) i32 { return point.x; }
    }
  )",
                             {original});
  const auto newer = buildShapes("newer", 43, "example/shapes", true);
  EXPECT_EQ(run({factory, newer}, R"(
    /** Keeps both instantiated and explicitly constructed points separate. */
    function main() i32 {
      var old = factory.make<i32>();
      var current = shapes.Point();
      return factory.read<i32>(old) + current.x;
    }
  )"),
            85);
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({factory, newer}, R"(
    /** Attempts a cross-revision call to a generic function. */
    function main() i32 {
      var current = shapes.Point();
      return factory.read<i32>(current);
    }
  )"),
                                "read");
}

/** Explicit module aliases select names without merging different type
 * identities. */
TEST_F(ABI_LibraryAbiSnapshot, SupportsExplicitAliasesForDifferentRevisions) {
  const auto first = buildShapes("first", 42);
  const auto second = buildShapes("second", 43, "example/shapes", true);
  auto driver = sun::driver::Driver::createForJIT("aliased_revisions");
  driver->setMoonImports(
      {{first, "shapes", "old"}, {second, "shapes", "current"}});
  EXPECT_EQ(driver->executeString(R"(
    /** Constructs both explicitly named revisions. */
    function main() i32 {
      var a = old.Point();
      var b = current.Point();
      return a.x + b.x;
    }
  )"),
            85);
}

/** Bundle metadata records the package-qualified explicit build identity. */
TEST_F(ABI_LibraryAbiSnapshot, PackageQualifiesStoredBuildIdentity) {
  std::ofstream(directory / "sun-config.json")
      << R"({"root":true,"package_id":"example/package"})";
  const auto qualified = buildShapes("qualified", 1, "shapes");
  auto qualifiedReader = sun::moon_bundling::MoonReader::open(qualified);
  ASSERT_NE(qualifiedReader, nullptr);
  EXPECT_EQ(qualifiedReader->getLibrary().bundle_contract().library_id(),
            "example/package/shapes");
  std::ofstream(directory / "sun-config.json") << R"({"root":true})";
  const auto standalone = buildShapes("standalone", 1, "shapes");
  auto standaloneReader = sun::moon_bundling::MoonReader::open(standalone);
  ASSERT_NE(standaloneReader, nullptr);
  EXPECT_EQ(standaloneReader->getLibrary().bundle_contract().library_id(),
            "shapes");
}
