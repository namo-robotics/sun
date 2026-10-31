/** Exercises checked library revisions through packaging, imports, and JIT. */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <utility>

#include "abi/abi_compatibility_checker.h"
#include "abi/library_abi_snapshot.h"
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
  }
  /** Packages source with an explicit dependency list. */
  std::string build(const std::string& name, const std::string& source,
                    const std::vector<std::string>& dependencies = {}) {
    const auto path = directory / (name + ".sun");
    std::ofstream(path) << source;
    sun::moon_bundling::MoonBuildOptions options;
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
  /** Defines the same nominal Point with a replaceable constructor body. */
  std::string shapes(int value, const std::string& identity = "example/shapes",
                     bool extraField = false) {
    return "manifest { library_id: \"" + identity + "\" }\n" +
           "/** Exports a point. */ public module shapes { "
           "/** Stores coordinates. */ public class Point { public var x: "
           "i32; " +
           (extraField ? std::string("public var y: i32; ") : std::string()) +
           "/** Initializes coordinates. */ init() { this.x = " +
           std::to_string(value) + "; " + (extraField ? "this.y = 0; " : "") +
           "} } }";
  }
  /** Builds a precompiled borrower of the original Point declaration. */
  std::string consumer(const std::string& original) {
    return build("reader", R"(
      /** Reads an imported point without rebuilding its implementation. */
      public module reader {
        /** Returns the stored coordinate. */
        public function read(point: ref shapes.Point) i32 { return point.x; }
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

/** Body-only revisions work in both import orders with distinct exact symbols.
 */
TEST_F(ABI_LibraryAbiSnapshot, AcceptsImplementationChangeInBothImportOrders) {
  const auto original = build("original", shapes(42));
  const auto reader = consumer(original);
  const auto replacement = build("replacement", shapes(43));
  EXPECT_EQ(run({reader, replacement}, application()), 43);
  EXPECT_EQ(run({replacement, reader}, application()), 43);
  auto first = sun::moon_bundling::MoonReader::open(original);
  auto second = sun::moon_bundling::MoonReader::open(replacement);
  ASSERT_TRUE(first && second);
  const auto& a = first->getLibrary().bundle_contract().library_abi();
  const auto& b = second->getLibrary().bundle_contract().library_abi();
  EXPECT_EQ(a.abi_hash(), b.abi_hash());
  EXPECT_NE(a.artifact_hash(), b.artifact_hash());
  EXPECT_NE(a.declarations(0).key(), b.declarations(0).key());
}

/** Legacy hashes tolerate advisory differences but retain semantic requirements. */
TEST_F(ABI_LibraryAbiSnapshot, MatchesLegacyHashesWithoutWeakeningDeclarations) {
  const auto original = build("original", shapes(42));
  const auto changed = build("changed", shapes(43, "example/shapes", true));
  auto originalReader = sun::moon_bundling::MoonReader::open(original);
  auto changedReader = sun::moon_bundling::MoonReader::open(changed);
  ASSERT_TRUE(originalReader && changedReader);
  std::vector<const sun::moon::ModuleMetadata*> modules;
  for (const auto& key : originalReader->listModules()) {
    modules.push_back(originalReader->getMetadata(key));
  }
  auto selected = originalReader->getLibrary().bundle_contract();
  *selected.mutable_target() = sun::abi::compatibilityTarget("arm64-apple-darwin");
  auto retaining = selected;
  retaining.mutable_target()->set_platform_abi("darwin25.6.0:");
  for (auto& rule : *retaining.mutable_requirements()) {
    if (rule.name() == "sun.experimental.semantics") {
      rule.set_implementation(std::string(64, 'a'));
    }
  }
  const auto artifact = originalReader->getLibrary().content_hash();
  const auto required = sun::abi::describeLibraryAbiSnapshot(
      retaining, artifact, modules);
  EXPECT_NE(required.abi_hash(), sun::abi::describeLibraryAbiSnapshot(
                                     selected, artifact, modules).abi_hash());
  EXPECT_TRUE(sun::abi::matchesLibraryAbiSnapshot(
      required, selected, artifact, modules, retaining));

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

/** Compatible artifacts warn once per compilation and exact imports stay quiet.
 */
TEST_F(ABI_LibraryAbiSnapshot, WarnsForIndependentCompatibleArtifacts) {
  const auto original = build("original", shapes(42));
  const auto consumerPath = consumer(original);
  const auto otherConsumerPath = build("other_reader", R"(
    /** Provides another import retaining the original dependency. */
    public module other_reader {
      /** Reads the original point layout. */
      public function read(point: ref shapes.Point) i32 { return point.x; }
    }
  )",
                                       {original});
  const auto revised = build("revised", shapes(43));
  auto originalReader = sun::moon_bundling::MoonReader::open(original);
  auto consumerReader = sun::moon_bundling::MoonReader::open(consumerPath);
  auto revisedReader = sun::moon_bundling::MoonReader::open(revised);
  auto otherConsumerReader =
      sun::moon_bundling::MoonReader::open(otherConsumerPath);
  ASSERT_TRUE(originalReader && consumerReader && revisedReader &&
              otherConsumerReader);
  sun::abi::CompilationContext context;
  context.producerWarnings = false;
  sun::abi::AbiCompatibilityChecker checker(context);
  testing::internal::CaptureStderr();
  EXPECT_NO_THROW(
      checker.validateImports({originalReader.get(), consumerReader.get()}));
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
  checker.beginCompilation("");
  testing::internal::CaptureStderr();
  EXPECT_NO_THROW(checker.validateImports(
      {revisedReader.get(), consumerReader.get(), otherConsumerReader.get()},
      (directory / "main.sun").string()));
  EXPECT_NO_THROW(checker.validateImports(
      {otherConsumerReader.get(), consumerReader.get(), revisedReader.get()},
      (directory / "main.sun").string()));
  const auto warning = testing::internal::GetCapturedStderr();
  const std::string marker =
      "Library 'example/shapes' is used through different artifacts";
  const auto first = warning.find(marker);
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(warning.find(marker, first + marker.size()), std::string::npos);
  EXPECT_NE(warning.find("\n  main.sun\n"), std::string::npos);
  EXPECT_NE(
      warning.find(std::filesystem::path(consumerPath).filename().string()),
      std::string::npos);
  EXPECT_NE(warning.find(
                std::filesystem::path(otherConsumerPath).filename().string()),
            std::string::npos);
  EXPECT_NE(warning.find("└── library: example/shapes [selected]"),
            std::string::npos);
  EXPECT_NE(warning.find("bundle: revised.moon"), std::string::npos);
  EXPECT_EQ(warning.find(directory.string()), std::string::npos);
  EXPECT_EQ(warning.find("\033["), std::string::npos);
  EXPECT_NE(warning.find("│   └── library: example/shapes [built against]\n"),
            std::string::npos);
  EXPECT_NE(warning.find(originalReader->getLibrary().content_hash()),
            std::string::npos);
  EXPECT_NE(warning.find(revisedReader->getLibrary().content_hash()),
            std::string::npos);
  checker.beginCompilation("");
  testing::internal::CaptureStderr();
  EXPECT_NO_THROW(checker.validateImports(
      {revisedReader.get(), consumerReader.get(), otherConsumerReader.get()},
      (directory / "main.sun").string()));
  EXPECT_NE(testing::internal::GetCapturedStderr().find(marker),
            std::string::npos);
}

/** Layout changes and unrelated namespaces cannot authorize a correspondence.
 */
TEST_F(ABI_LibraryAbiSnapshot, RejectsLayoutChangesAndUnrelatedLibraries) {
  const auto original = build("original", shapes(42));
  const auto reader = consumer(original);
  const auto changed = build("changed", shapes(43, "example/shapes", true));
  const auto unrelated = build("unrelated", shapes(43, "other/shapes"));
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({reader, changed}, application()),
                                "Library ABI mismatch");
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({reader, unrelated}, application()),
                                "moon exact dependency");
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({reader}, application()),
                                "moon exact dependency");
}

/** ABI errors identify both artifacts and the imports retaining requirements. */
TEST_F(ABI_LibraryAbiSnapshot, ShowsConflictingArtifactsAndDependencyTree) {
  const auto original = build("original", shapes(42));
  const auto consumerPath = consumer(original);
  const auto changed = build("changed", shapes(43, "example/shapes", true));
  auto originalReader = sun::moon_bundling::MoonReader::open(original);
  auto consumerReader = sun::moon_bundling::MoonReader::open(consumerPath);
  auto changedReader = sun::moon_bundling::MoonReader::open(changed);
  ASSERT_TRUE(originalReader && consumerReader && changedReader);
  for (const bool reversed : {false, true}) {
    sun::abi::AbiCompatibilityChecker checker;
    std::vector<sun::moon_bundling::MoonReader*> imports{
        consumerReader.get(), changedReader.get()};
    if (reversed) {
      std::swap(imports[0], imports[1]);
    }
    try {
      checker.validateImports(imports, (directory / "main.sun").string());
      FAIL() << "Expected an ABI mismatch";
    } catch (const sun::support::SunError& error) {
      const std::string message = error.what();
      EXPECT_NE(message.find("Library ABI mismatch for 'example/shapes'"),
                std::string::npos);
      EXPECT_NE(message.find("\n  main.sun\n"), std::string::npos);
      EXPECT_NE(message.find("├── reader.moon [imported bundle]\n"),
                std::string::npos);
      EXPECT_NE(
          message.find("│   └── library: example/shapes [built against]\n"),
          std::string::npos);
      EXPECT_NE(message.find("└── library: example/shapes [selected]\n"),
                std::string::npos);
      EXPECT_NE(message.find("artifact: " +
                             originalReader->getLibrary().content_hash()),
                std::string::npos);
      EXPECT_NE(message.find("artifact: " +
                             changedReader->getLibrary().content_hash()),
                std::string::npos);
      EXPECT_NE(message.find("bundle: changed.moon"), std::string::npos);
      EXPECT_EQ(message.find(directory.string()), std::string::npos);
      EXPECT_EQ(message.find("\033["), std::string::npos);
    }
  }
}

/** Multiple explicitly supplied revisions are ambiguous even with equal ABIs.
 */
TEST_F(ABI_LibraryAbiSnapshot, RejectsAmbiguousRevisions) {
  const auto original = build("original", shapes(42));
  const auto reader = consumer(original);
  const auto replacement = build("replacement", shapes(43));
  const auto another = build("another", shapes(44));
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      run({another, replacement, reader}, application()),
      "Ambiguous library revisions");
}

/** Source order does not determine nominal correspondence. */
TEST_F(ABI_LibraryAbiSnapshot, AcceptsDeclarationReordering) {
  const std::string marker =
      "/** Exports an independent type. */ public module marker { "
      "/** Stores another value. */ public class Other { public var x: i32; } "
      "}";
  const auto original = build("original", shapes(42) + marker);
  const auto reader = consumer(original);
  const auto replacement = build("replacement", marker + shapes(43));
  EXPECT_EQ(run({reader, replacement}, application()), 43);
}

/** A named transitive consumer retains the original dependency requirement. */
TEST_F(ABI_LibraryAbiSnapshot, PreservesTransitiveRequirements) {
  const auto original = build("original", shapes(42));
  const auto reader = consumer(original);
  const auto wrapper = build("wrapper", R"(
    /** Wraps an existing compiled point reader. */
    public module wrapper {
      /** Reads through a dependency. */
      public function read(point: ref shapes.Point) i32 { return reader.read(point); }
    }
  )",
                             {reader, original});
  const auto replacement = build("replacement", shapes(43));
  EXPECT_EQ(run({wrapper, replacement}, R"(
    /** Uses the transitive ABI requirement with a replacement point. */
    function main() i32 { var p = shapes.Point(); return wrapper.read(p); }
  )"),
            43);
}

/** Old embedded code keeps its original constructor implementation. */
TEST_F(ABI_LibraryAbiSnapshot, PreservesEmbeddedImplementationSymbols) {
  const auto original = build("original", shapes(42));
  const auto factory = build("factory", R"(
    /** Retains an implementation compiled against the first revision. */
    public module factory {
      /** Constructs using the original library code. */
      public function make() shapes.Point { return shapes.Point(); }
      /** Reads a value across the original boundary. */
      public function read(point: ref shapes.Point) i32 { return point.x; }
    }
  )",
                             {original});
  const auto replacement = build("replacement", shapes(43));
  EXPECT_EQ(run({factory, replacement}, R"(
    /** Compares old embedded construction with the selected revision. */
    function main() i32 {
      var old = factory.make();
      var revised = shapes.Point();
      return factory.read(revised) - factory.read(old);
    }
  )"),
            1);
}

/** Retained generic behavior is an importer requirement, unlike compiled
 * bodies. */
TEST_F(ABI_LibraryAbiSnapshot, RejectsChangedRetainedGenericBodies) {
  const std::string generic = R"(
    /** Supplies code instantiated by importing compilers. */
    public module generic {
      /** Returns a value from a retained body. */
      public function value<T>() i32 { return VALUE; }
    }
  )";
  auto first = generic;
  first.replace(first.find("VALUE"), 5, "1");
  auto second = generic;
  second.replace(second.find("VALUE"), 5, "2");
  const auto original = build("original", shapes(42) + first);
  const auto reader = consumer(original);
  const auto replacement = build("replacement", shapes(43) + second);
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({reader, replacement}, application()),
                                "Library ABI mismatch");
}

/** Constness changes the ownership contract even when storage stays identical.
 */
TEST_F(ABI_LibraryAbiSnapshot, RejectsChangedReferenceContract) {
  const std::string function = R"(
    /** Exposes a borrowing contract. */
    public module borrowing {
      /** Reads a borrowed point. */
      public function read(point: ref shapes.Point) i32 { return point.x; }
    }
  )";
  const auto original = build("original", shapes(42) + function);
  const auto reader = consumer(original);
  auto changed = function;
  changed.replace(changed.find("ref shapes.Point"), 3, "const ref");
  const auto replacement = build("replacement", shapes(43) + changed);
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({reader, replacement}, application()),
                                "Library ABI mismatch");
}

/** Retained consumer code resolves checked nominal types when instantiated. */
TEST_F(ABI_LibraryAbiSnapshot,
       InstantiatesRetainedConsumerWithRevisedNominalType) {
  const auto original = build("original", shapes(42));
  const auto reader = build("reader", R"(
    /** Keeps a generic body referring to the dependency's nominal type. */
    public module reader {
      /** Reads a point while retaining an independent type parameter. */
      public function read<T>(point: ref shapes.Point) i32 { return point.x; }
    }
  )",
                            {original});
  const auto replacement = build("replacement", shapes(43));
  EXPECT_EQ(run({reader, replacement}, R"(
    /** Instantiates retained code with a validated replacement declaration. */
    function main() i32 { var p = shapes.Point(); return reader.read<i32>(p); }
  )"),
            43);
}

/** Generic class origins correspond without merging exact specialization keys.
 */
TEST_F(ABI_LibraryAbiSnapshot, AcceptsUnchangedGenericClassContract) {
  const std::string box = R"(
    /** Exports a generic container with unchanged retained code. */
    public module boxes {
      /** Stores one value of the caller's type. */
      public class Box<T> { public var value: T; }
    }
  )";
  const auto original = build("original", shapes(42) + box);
  const auto reader = build("reader", R"(
    /** Reads a concrete instance compiled against the original generic class. */
    public module reader {
      /** Reads the stored integer. */
      public function read(box: ref boxes.Box<i32>) i32 { return box.value; }
    }
  )",
                            {original});
  const auto replacement = build("replacement", shapes(43) + box);
  EXPECT_EQ(run({reader, replacement}, R"(
    /** Passes a replacement specialization across a compiled boundary. */
    function main() i32 {
      var box: boxes.Box<i32> = { value: 43 };
      return reader.read(box);
    }
  )"),
            43);
}
