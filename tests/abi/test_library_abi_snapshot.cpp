/** Exercises checked library revisions through packaging, imports, and JIT. */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <utility>

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
    return "manifest { id: \"" + identity + "\" }\n" +
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
  const auto original = build("original", shapes(42));
  const auto reader = consumer(original);
  EXPECT_EQ(run({reader, original}, application()), 42);
  EXPECT_EQ(run({original, reader}, application()), 42);
}

/** Internal revisions do not constrain an independently listed library. */
TEST_F(ABI_LibraryAbiSnapshot, KeepsDifferentVersionsIndependent) {
  const auto original = build("original", shapes(42));
  const auto reader = consumer(original);
  for (bool changedLayout : {false, true}) {
    const auto newer = build(changedLayout ? "changed" : "newer",
                             shapes(43, "example/shapes", changedLayout));
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
  const auto original = build("original", shapes(42));
  const auto reader = consumer(original);
  const auto newer = build("newer", shapes(43));
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
  const auto original = build("original", shapes(42));
  const auto reader = consumer(original);
  const auto changed = build("changed", shapes(43, "example/shapes", true));
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({reader, changed}, application()), "read");
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({changed, reader}, application()), "read");
  const auto unrelated = build("unrelated", shapes(43, "other/shapes"));
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({reader, unrelated}, application()),
                                "read");
}

/** Borrow compatibility preserves constness and applies to retained generics.
 */
TEST_F(ABI_LibraryAbiSnapshot,
       CompatibleBorrowPreservesMutabilityAndGenericCode) {
  const auto original = build("original", shapes(42));
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
  const auto newer = build("newer", shapes(43));
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
  const auto first = build("first", shapes(42));
  const auto second = build("second", shapes(43));
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
  const auto original = build("original", shapes(42));
  const auto consumerPath = consumer(original);
  const auto changed = build("changed", shapes(43, "example/shapes", true));
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
  const auto first = build("first", shapes(42));
  const auto second = build("second", shapes(43, "example/shapes", true));
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
  const auto original = build("original", shapes(42));
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
  const auto newer = build("newer", shapes(43));
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
  const auto original = build("original", shapes(42));
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
  const auto newer = build("newer", shapes(43, "example/shapes", true));
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
  const auto first = build("first", shapes(42));
  const auto second = build("second", shapes(43, "example/shapes", true));
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

/** Moves scalar aggregates through every owned boundary without reconstructing
 * them. */
TEST_F(ABI_LibraryAbiSnapshot, TransfersOwnedValuesAcrossBoundaries) {
  const auto original = build("original", shapes(1));
  const auto producer = build("producer", R"(
    /** Exposes the embedded point by value. */
    public module producer {
      /** Constructs the original point. */
      public function make() shapes.Point { return shapes.Point(); }
      /** Consumes a point in the original revision. */
      public function consume(point: shapes.Point) i32 { return point.x; }
    }
  )",
                              {original});
  const auto newer = build("newer", shapes(2));
  const std::vector<std::string> imports{newer, producer};
  EXPECT_EQ(run(imports, R"(
    /** Returns an owned value through the selected declaration. */
    function transfer() shapes.Point { return producer.make(); }
    /** Observes the source constructor's value after returning. */
    function main() i32 { var p = transfer(); return p.x; }
  )"),
            1);
  EXPECT_EQ(run(imports, R"(
    /** Transfers an initialized variable to a compiled owned parameter. */
    function main() i32 {
      var p: shapes.Point = producer.make();
      return producer.consume(p);
    }
  )"),
            1);
  EXPECT_EQ(run(imports, R"(
    /** Replaces an existing owner with the embedded revision's value. */
    function main() i32 {
      var p = shapes.Point();
      p = producer.make();
      return p.x;
    }
  )"),
            1);
  EXPECT_EQ(run(imports, R"(
    /** Carries a point owned by the consumer. */
    enum Box { Value(shapes.Point) }
    /** Constructs an enum payload from an embedded point. */
    function main() i32 {
      return match Box.Value(producer.make()) { Box.Value(p) => p.x };
    }
  )"),
            1);
}

/** Propagates both direct and nested errors from an independent revision. */
TEST_F(ABI_LibraryAbiSnapshot, PropagatesOwnedErrorsAcrossRevisions) {
  /** Defines corresponding error declarations with different constructors. */
  const auto errors = [](int code) {
    return R"(
      manifest { id: "example/errors" }
      /** Exports an owned result API. */
      public module errors {
        /** Stores an error code. */
        public class Failure {
          public var code: i32;
          /** Initializes the revision's error code. */
          init() { this.code = )" +
           std::to_string(code) + R"(; }
        }
        /** Returns a value or an owned error. */
        public enum Result<T> { Ok(T), Error(Failure) }
      }
    )";
  };
  const auto first = build("first", errors(1));
  const auto second = build("second", errors(2));
  const auto producer = build("producer", R"(
    /** Constructs results in the embedded revision. */
    public module producer {
      /** Returns the original failure. */
      public function fail() errors.Result<i32> {
        return errors.Result.Error(errors.Failure());
      }
    }
  )",
                              {first});
  EXPECT_EQ(run({second, producer}, R"(
    /** Propagates an owned error to a result with a different success type. */
    function run() errors.Result<bool> {
      var value = try producer.fail();
      return errors.Result.Ok(value == 0);
    }
    /** Observes the original error code. */
    function main() i32 {
      return match run() { errors.Result.Ok(_) => 0, errors.Result.Error(e) => e.code };
    }
  )"),
            1);
  EXPECT_EQ(run({second, producer}, R"(
    /** Groups failure types. */
    enum Failures { Failure(errors.Failure) }
    /** Wraps the error sum. */
    enum Result { Ok(i32), Error(Failures) }
    /** Propagates into a nested error sum. */
    function run() Result { return Result.Ok(try producer.fail()); }
    /** Reads the nested error without replacing its value. */
    function main() i32 {
      return match run() {
        Result.Ok(v) => v,
        Result.Error(e) => match e { Failures.Failure(f) => f.code }
      };
    }
  )"),
            1);
  EXPECT_EQ(run({second, producer}, R"(
    /** Transfers an entire specialized enum. */
    function run() errors.Result<i32> { return producer.fail(); }
    /** Reads the transferred result. */
    function main() i32 {
      return match run() { errors.Result.Ok(v) => v, errors.Result.Error(e) => e.code };
    }
  )"),
            1);
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({second, producer}, R"(
    /** Rejects different success arguments even if only failure occurs. */
    function run() errors.Result<bool> { return producer.fail(); }
    /** Makes the return boundary reachable. */
    function main() i32 { var result = run(); return 0; }
  )"),
                                "Return value");
}

/** Same layout and spelling do not authorize unrelated or changed declarations.
 */
TEST_F(ABI_LibraryAbiSnapshot, RejectsIncompatibleOwnedValues) {
  const auto first = build("first", shapes(1));
  const auto producer = build("producer", R"(
    /** Exposes an embedded point. */
    public module producer {
      /** Returns the original point. */
      public function make() shapes.Point { return shapes.Point(); }
    }
  )",
                              {first});
  for (const auto& replacement :
       {shapes(2, "other/shapes"), shapes(2, "example/shapes", true)}) {
    const auto second = build("second", replacement);
    EXPECT_SUN_ERROR_WITH_MESSAGE(run({second, producer}, R"(
      /** Attempts an unproved owned transfer. */
      function main() i32 { var p: shapes.Point = producer.make(); return p.x; }
    )"),
                                  "type");
  }
}

/** Destructor implementation changes require an explicit conversion. */
TEST_F(ABI_LibraryAbiSnapshot, RejectsRevisionSpecificDestructors) {
  /** Defines a resource with revision-specific cleanup. */
  const auto resource = [](int count) {
    return R"(
      manifest { id: "example/resources" }
      /** Owns a revision-specific resource counter. */
      public module resources {
        var drops: i32 = 0;
        /** Tracks a resource with revision-specific cleanup. */
        public class Resource {
          /** Creates a resource. */
          init() {}
          /** Records cleanup in this revision. */
          deinit() { drops = drops + )" +
           std::to_string(count) + R"(; }
        }
      }
    )";
  };
  const auto first = build("first", resource(1));
  const auto second = build("second", resource(2));
  const auto producer = build("producer", R"(
    /** Returns resources with original destruction behavior. */
    public module producer {
      /** Creates an owned resource. */
      public function make() resources.Resource { return resources.Resource(); }
    }
  )",
                              {first});
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({second, producer}, R"(
    /** Attempts to change the destructor responsible for an owned resource. */
    function main() i32 { var r: resources.Resource = producer.make(); return 0; }
  )"),
                                "type");
}

/** An unchanged nested owner retains its allocator and destructor across a
 * move. */
TEST_F(ABI_LibraryAbiSnapshot, PreservesExactNestedResourceCleanup) {
  const auto common = build("common", R"(
    manifest { id: "example/common-resource" }
    /** Owns allocation and destruction accounting. */
    public module common {
      var live: i32 = 0;
      var destroyed: i32 = 0;
      /** Allocates storage using the process allocator. */
      extern function malloc(size: i64) raw_ptr<u8>;
      /** Releases storage using the matching process allocator. */
      extern function free(data: raw_ptr<u8>) void;
      /** Holds storage with one exact cleanup implementation. */
      public class Resource {
        var data: raw_ptr<u8>;
        /** Allocates one owned buffer. */
        init() { unsafe { this.data = malloc(8); }; live = live + 1; }
        /** Releases the buffer and records exactly one destruction. */
        deinit() {
          unsafe { free(this.data); };
          this.data = null;
          live = live - 1;
          destroyed = destroyed + 1;
        }
      }
      /** Reports outstanding buffers. */
      public function outstanding() i32 { return live; }
      /** Reports destruction count. */
      public function drops() i32 { return destroyed; }
    }
  )");
  /** Wraps the exact resource in a revision-specific aggregate. */
  const auto wrapper = [](int code) {
    return R"(
      manifest { id: "example/resource-wrapper" }
      /** Adds a code to an owned resource. */
      public module wrapper {
        /** Preserves the common resource's exact declaration and cleanup. */
        public class Value {
          public var resource: common.Resource;
          public var code: i32;
          /** Creates a resource using the common allocator. */
          init() { this.resource = common.Resource(); this.code = )" +
           std::to_string(code) + R"(; }
        }
      }
    )";
  };
  const auto first = build("first", wrapper(1), {common});
  const auto second = build("second", wrapper(2), {common});
  const auto producer = build("producer", R"(
    /** Creates resources with the original wrapper implementation. */
    public module producer {
      /** Returns an owned wrapper. */
      public function make() wrapper.Value { return wrapper.Value(); }
      /** Drops an owned wrapper after observing its source value. */
      public function consume(value: wrapper.Value) i32 { return value.code; }
    }
  )",
                              {first, common});
  EXPECT_EQ(run({second, producer, common}, R"(
    /** Carries an owned wrapper. */
    enum Box { Value(wrapper.Value) }
    /** Returns a wrapper without changing its nested resource. */
    function forward() wrapper.Value { return producer.make(); }
    /** Exercises variable replacement, enum construction, and owned calls. */
    function transfer() i32 {
      var original = producer.make();
      var value: wrapper.Value = original;
      var replacement = forward();
      value = replacement;
      var payload = producer.make();
      var boxed = Box.Value(payload);
      var result = match boxed { Box.Value(v) => producer.consume(v) };
      return value.code + result;
    }
    /** Requires every allocated resource to be destroyed exactly once. */
    function main() i32 {
      var result = transfer();
      if (common.outstanding() != 0) { return 100; }
      if (common.drops() != 3) { return 200 + common.drops(); }
      return result;
    }
  )"),
            2);
}

/** A dependency can propagate a supported standard error across stdlib
 * revisions. */
TEST_F(ABI_LibraryAbiSnapshot, PropagatesSupportedStdlibError) {
  const auto root = std::filesystem::current_path();
  std::ifstream input(root / "stdlib" / "stdlib.sun");
  ASSERT_TRUE(input.good());
  std::string manifest((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  const std::string prefix = "\"std/";
  const auto absolute = "\"" + (root / "stdlib" / "std").string() + "/";
  for (size_t offset = 0;
       (offset = manifest.find(prefix, offset)) != std::string::npos;
       offset += absolute.size()) {
    manifest.replace(offset, prefix.size(), absolute);
  }
  /** Changes an implementation while retaining the standard library ABI. */
  const auto revision = [&](int value) {
    return manifest + R"(
      /** Distinguishes independently compiled test revisions. */
      public module revision {
        /** Returns this revision's marker. */
        function marker() i32 { return )" +
           std::to_string(value) + R"(; }
      }
    )";
  };
  const auto first = build("stdlib_first", revision(1));
  const auto second = build("stdlib_second", revision(2));
  const auto producer = build("producer", R"(
    /** Produces an error from its embedded standard library. */
    public module producer {
      /** Reports an empty container. */
      public function fail() std.PopResult<void> {
        return std.PopResult.Empty(std.EmptyError());
      }
    }
  )",
                              {first});
  EXPECT_EQ(run({second, producer}, R"(
    /** Propagates the standard error across independent revisions. */
    function forward() std.PopResult<bool> {
      try producer.fail();
      return std.PopResult.Ok(true);
    }
    /** Observes the propagated error. */
    function main() i32 {
      return match forward() {
        std.PopResult.Ok(_) => 0,
        std.PopResult.Empty(error) => error.code()
      };
    }
  )"),
            1);
}

/** Rejects layout-only borrows when an implementation uses unsafe assumptions. */
TEST_F(ABI_LibraryAbiSnapshot, RejectsUnprovedBorrowInvariants) {
  auto source = shapes(1);
  source.insert(source.find("this.x"), "unsafe {}; ");
  const auto original = build("original", source);
  const auto reader = consumer(original);
  const auto changed = build("changed", shapes(2));
  EXPECT_THROW(run({changed, reader}, application()), sun::support::SunError);
}

/** Keeps const reads subject to the same representation invariants as mutation. */
TEST_F(ABI_LibraryAbiSnapshot, RejectsUnprovedConstBorrowInvariants) {
  auto source = shapes(1);
  source.insert(source.find("this.x"), "unsafe {}; ");
  const auto original = build("original", source);
  const auto reader = build("reader", R"(
    /** Reads a point through its original implementation contract. */
    public module reader {
      /** Observes the point without mutating it. */
      public function read(point: const ref shapes.Point) i32 { return point.x; }
    }
  )", {original});
  const auto changed = build("changed", shapes(2));
  EXPECT_THROW(run({changed, reader}, application()), sun::support::SunError);
}

/** Stores complete method evidence while ordinary imports retain bodyless methods. */
TEST_F(ABI_LibraryAbiSnapshot, RetainsSeparateImplementationProofs) {
  const auto bundle = build("point", shapes(1));
  auto reader = sun::moon_bundling::MoonReader::open(bundle);
  ASSERT_TRUE(reader);
  bool found = false;
  for (const auto& key : reader->listModules()) {
    const auto& metadata = *reader->getMetadata(key);
    for (const auto& node : metadata.correspondence_proof().definitions()) {
      if (!node.has_class_def() || node.class_def().name() != "Point") {
        continue;
      }
      found = true;
      EXPECT_EQ(metadata.correspondence_proof().version(), 1);
      EXPECT_FALSE(metadata.correspondence_proof().payload_digest().empty());
      ASSERT_GT(node.class_def().methods_size(), 0);
      EXPECT_GT(node.class_def().methods(0).function().body().body_size(), 0);
      ASSERT_GT(metadata.classes_size(), 0);
      EXPECT_EQ(metadata.classes(0).methods(0).function().body().body_size(), 0);
    }
  }
  EXPECT_TRUE(found);
}

/** Unknown proof versions cannot fall back to matching declarations or layouts. */
TEST_F(ABI_LibraryAbiSnapshot, RejectsUnsupportedImplementationProofVersion) {
  const auto first = build("first", shapes(1));
  const auto second = build("second", shapes(2));
  sun::abi::CorrespondenceEvidence evidence;
  std::vector<std::string> declarations;
  for (const auto& bundle : {first, second}) {
    auto reader = sun::moon_bundling::MoonReader::open(bundle);
    ASSERT_TRUE(reader);
    for (const auto& key : reader->listModules()) {
      auto metadata = *reader->getMetadata(key);
      for (const auto& cls : metadata.classes()) {
        if (cls.name() == "Point") {
          declarations.push_back(cls.declaration_identity().key());
        }
      }
      if (bundle == second && metadata.has_correspondence_proof()) {
        metadata.mutable_correspondence_proof()->set_version(999);
      }
      evidence.add(metadata, "example/shapes");
    }
  }
  ASSERT_EQ(declarations.size(), 2);
  auto result = evidence.compare(declarations[0], declarations[1], true);
  EXPECT_FALSE(result.proven);
  EXPECT_NE(result.reason.find("unsupported"), std::string::npos);
}

/** Comparing an implementation preserves constructor literals, unlike a plain move. */
TEST_F(ABI_LibraryAbiSnapshot, DistinguishesImplementationFromPlainStorageProof) {
  const auto first = build("first", shapes(1));
  const auto second = build("second", shapes(2));
  sun::abi::CorrespondenceEvidence evidence;
  std::vector<std::string> declarations;
  for (const auto& bundle : {first, second}) {
    auto reader = sun::moon_bundling::MoonReader::open(bundle);
    ASSERT_TRUE(reader);
    for (const auto& key : reader->listModules()) {
      const auto& metadata = *reader->getMetadata(key);
      for (const auto& cls : metadata.classes()) {
        if (cls.name() == "Point") {
          declarations.push_back(cls.declaration_identity().key());
        }
      }
      evidence.add(metadata, "example/shapes");
    }
  }
  ASSERT_EQ(declarations.size(), 2);
  EXPECT_TRUE(evidence.compare(declarations[0], declarations[1], true).proven);
  auto implementation = evidence.compare(declarations[0], declarations[1], false);
  EXPECT_FALSE(implementation.proven);
  EXPECT_EQ(implementation.reason, "resolved implementations differ");
}

/** A destructor with identical scalar operations needs no allocator contract. */
TEST_F(ABI_LibraryAbiSnapshot, TransfersProvenScalarCleanup) {
  /** Keeps cleanup fixed while changing an unrelated implementation. */
  const auto revision = [](int marker) {
    return R"(
      manifest { id: "example/scalar-cleanup" }
      /** Exports a value with scalar cleanup. */
      public module values {
        /** Stores a coordinate until destruction. */
        public class Value {
          public var x: i32;
          /** Initializes the coordinate. */ init() { this.x = 7; }
          /** Clears the scalar storage. */ deinit() { this.x = 0; }
        }
        /** Distinguishes independent builds. */
        public function marker() i32 { return )" + std::to_string(marker) + R"(; }
      }
    )";
  };
  const auto first = build("first", revision(1));
  const auto second = build("second", revision(2));
  const auto producer = build("producer", R"(
    /** Creates the embedded value. */ public module producer {
      /** Returns the original implementation's value. */
      public function make() values.Value { return values.Value(); }
    }
  )", {first});
  EXPECT_EQ(run({second, producer}, R"(
    /** Moves a value with proven scalar destruction. */
    function main() i32 { var value: values.Value = producer.make(); return value.x; }
  )"), 7);
}

/** Identical destructor syntax does not hide a changed helper implementation. */
TEST_F(ABI_LibraryAbiSnapshot, RejectsChangedCleanupDependency) {
  /** Changes a private helper while retaining the class body and layout. */
  const auto revision = [](int marker) {
    return R"(
      manifest { id: "example/cleanup-helper" }
      /** Exports a value with private cleanup dependencies. */
      public module values {
        /** Computes the cleanup value. */
        function cleanup_code() i32 { return )" + std::to_string(marker) + R"(; }
        /** Stores scalar state with a cleanup helper. */
        public class Value {
          public var x: i32;
          /** Initializes storage. */ init() { this.x = 7; }
          /** Clears storage using the revision's helper. */
          deinit() { this.x = cleanup_code(); }
        }
      }
    )";
  };
  const auto first = build("first", revision(1));
  const auto second = build("second", revision(2));
  const auto producer = build("producer", R"(
    /** Creates the original owner. */ public module producer {
      /** Returns an owned value. */
      public function make() values.Value { return values.Value(); }
    }
  )", {first});
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({second, producer}, R"(
    /** Attempts to move an owner with different cleanup dependencies. */
    function main() i32 { var value: values.Value = producer.make(); return value.x; }
  )"), "cleanup_code");
}

/** Equal initializers do not identify separately allocated mutable globals. */
TEST_F(ABI_LibraryAbiSnapshot, RejectsIndependentCleanupState) {
  /** Creates independent counters with otherwise identical cleanup code. */
  const auto revision = [](int marker) {
    return R"(
      manifest { id: "example/cleanup-state" }
      /** Owns a revision-local cleanup counter. */
      public module values {
        var drops: i32 = 0;
        /** Counts destruction in its defining revision. */
        public class Value {
          /** Creates the owner. */ init() {}
          /** Updates the original counter. */ deinit() { drops = drops + 1; }
        }
        /** Distinguishes the revisions without changing cleanup text. */
        public function marker() i32 { return )" + std::to_string(marker) + R"(; }
      }
    )";
  };
  const auto first = build("first", revision(1));
  const auto second = build("second", revision(2));
  const auto producer = build("producer", R"(
    /** Creates an owner tied to the embedded counter. */ public module producer {
      /** Returns the original owner. */
      public function make() values.Value { return values.Value(); }
    }
  )", {first});
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({second, producer}, R"(
    /** Attempts to transfer a revision-local cleanup obligation. */
    function main() i32 { var value: values.Value = producer.make(); return 0; }
  )"), "revision-local global state");
}
