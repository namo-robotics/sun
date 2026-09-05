#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "driver/execution_utils.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_builder.h"
#include "serialization/ast_deserializer.h"
#include "serialization/ast_serializer.h"
#include "serialization/metadata_references.h"
#include "support/error.h"

namespace {

/** Write independent source files for one import-scoping test. */
std::vector<std::string> writeImportSources(
    const std::vector<std::string>& sources) {
  const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
  auto dir = std::filesystem::current_path() / "tmp" / "file_import_tests" /
             test->name();
  std::filesystem::create_directories(dir);
  std::vector<std::string> paths;
  for (size_t i = 0; i < sources.size(); ++i) {
    auto path = dir / (std::to_string(i) + ".sun");
    std::ofstream(path) << sources[i];
    paths.push_back(path.string());
  }
  return paths;
}

/** Verify rejection is independent of source-file ordering. */
void rejectBothOrders(std::vector<std::string> paths) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileFiles(paths), "Unknown");
  std::reverse(paths.begin(), paths.end());
  EXPECT_SUN_ERROR_WITH_MESSAGE(compileFiles(paths), "Unknown");
}

}  // namespace

TEST(Modules_FileImports, top_level_import_does_not_reach_another_file) {
  for (const auto& import : {"using lib;", "using lib.answer;"}) {
    auto paths =
        writeImportSources({std::string(import) + R"(
          public module lib {
            public function answer() i32 { return 42; }
          }
          function local() i32 { return answer(); }
        )",
                            "function main() i32 { return answer(); }"});
    rejectBothOrders(paths);
  }
}

TEST(Modules_FileImports, merged_module_import_does_not_reach_another_file) {
  auto paths = writeImportSources({R"(
    public module lib {
      public class Value { var n: i32; }
    }
    public module app {
      using lib;
      function local(v: ref Value) i32 { return 1; }
    }
  )",
                                   R"(
    public module app {
      module inner {
        class Holder { var value: Value; }
      }
    }
    function main() i32 { return 0; }
  )"});
  rejectBothOrders(paths);
}

TEST(Modules_FileImports,
     imports_reach_same_file_fields_signatures_and_bodies) {
  auto paths = writeImportSources({R"(
    public module lib {
      public class Value { var n: i32; }
      public function answer() i32 { return 42; }
    }
  )",
                                   R"(
    public module app {
      module inner {
        class Holder { var value: Value; }
        function take(v: ref Value) i32 { return answer(); }
      }
    }
    using lib;
    function main() i32 { return answer(); }
  )"});
  EXPECT_NO_THROW(compileFiles(paths));
  std::reverse(paths.begin(), paths.end());
  EXPECT_NO_THROW(compileFiles(paths));
}

TEST(Modules_FileImports, distinct_imports_do_not_create_cross_file_ambiguity) {
  auto paths = writeImportSources({R"(
    public module left {
      public class Value { var n: i32; }
    }
    using left;
    public module app {
      function first(v: ref Value) i32 { return 1; }
    }
  )",
                                   R"(
    public module right {
      public class Value { var n: i64; }
    }
    using right;
    public module app {
      function second(v: ref Value) i32 { return 2; }
    }
    function main() i32 { return 0; }
  )"});
  EXPECT_NO_THROW(compileFiles(paths));
  std::reverse(paths.begin(), paths.end());
  EXPECT_NO_THROW(compileFiles(paths));
}

TEST(Modules_FileImports, qualified_names_and_shared_declarations_still_work) {
  auto paths = writeImportSources({R"(
    public module lib { public function answer() i32 { return 42; } }
    public module app { function first() i32 { return lib.answer(); } }
  )",
                                   R"(
    public module app { public function second() i32 { return first(); } }
    function main() i32 { return app.second(); }
  )"});
  EXPECT_NO_THROW(compileFiles(paths));
}

TEST(Modules_FileImports, generic_definitions_keep_their_file_imports) {
  auto paths = writeImportSources({R"(
    public module lib {
      public class Value { var n: i32; }
      public function answer() i32 { return 42; }
    }
    using lib;
    public module app {
      public class Box<T> {
        var value: Value;
        public method get() i32 { return answer(); }
        public method generic<U>() i32 { return answer(); }
      }
      public enum Choice<T> { Empty, Item(Value) }
      public interface View<T> { method get(v: ref Value) i32; }
      public function result<T>() i32 { return answer(); }
      public function consume<T>(v: ref Box<T>) i32 { return v.get(); }
    }
  )",
                                   R"(
    public module other { public function answer() i32 { return 7; } }
    using other;
    function inspect(v: ref app.Box<i32>, c: app.Choice<i32>,
                     view: ref app.View<i32>) i32 {
      return v.generic<i64>() + app.consume<i32>(v);
    }
    function main() i32 { return app.result<i32>(); }
  )"});
  EXPECT_NO_THROW(compileFiles(paths));
  std::reverse(paths.begin(), paths.end());
  EXPECT_NO_THROW(compileFiles(paths));
}

TEST(Modules_FileImports, caller_import_cannot_repair_a_generic_definition) {
  auto paths = writeImportSources({R"(
    public module lib { public function answer() i32 { return 42; } }
    public module app {
      public function result<T>() i32 { return answer(); }
    }
  )",
                                   R"(
    using lib;
    function main() i32 { return app.result<i32>(); }
  )"});
  rejectBothOrders(paths);
}

TEST(Modules_FileImports, imports_preserve_lexical_scope) {
  EXPECT_THROW(executeString(R"(
    public module lib { public function answer() i32 { return 42; } }
    module app { using lib; function local() i32 { return answer(); } }
    function main() i32 { return answer(); }
  )"),
               SunError);
  EXPECT_THROW(executeString(R"(
    public module lib { public function answer() i32 { return 42; } }
    function local() i32 { using lib; return answer(); }
    function main() i32 { return answer(); }
  )"),
               SunError);
}

TEST(Modules_FileImports,
     bundle_keeps_file_imports_and_generic_definition_context) {
  auto paths = writeImportSources({R"(
    manifest { source_files: ["1.sun", "2.sun"] }
  )",
                                   R"(
    public module left {
      public function answer() i32 { return 40; }
      public class Value { var n: i32; }
    }
    using left.answer;
    using left.Value;
    public module api {
      public class Holder { var value: Value; }
      public function first<T>() i32 { return answer(); }
    }
  )",
                                   R"(
    public module right { public function answer() i32 { return 2; } }
    using right;
    public module api {
      public function second<T>() i32 { return answer(); }
    }
  )"});
  initTestEnvironment();
  auto moon = std::filesystem::path(paths[0]).parent_path() / "library.moon";
  ASSERT_NO_THROW(sun::MoonBuilder::build(paths[0], moon));
  auto driver = Driver::createForJIT("file_import_bundle");
  driver->setMoonImports({sun::MoonImport(moon.string())});
  EXPECT_EQ(driver->executeString(R"(
    public module caller { public function answer() i32 { return 99; } }
    using caller;
    function main() i32 { return api.first<i32>() + api.second<i32>(); }
  )"),
            42);
}

TEST(Modules_FileImports, interpolation_keeps_enclosing_file_imports) {
  EXPECT_EQ(executeStringWithStdlib(R"(
    using std;
    public module lib { public function answer() i32 { return 42; } }
    using lib;
    function main() i32 {
      var text = `${answer()}`;
      return text.length();
    }
  )"),
            2);
}

TEST(Modules_FileImports, different_bundles_remap_overlapping_file_ids) {
  auto paths = writeImportSources({R"(
    public module left_impl { public function answer() i32 { return 40; } }
    using left_impl;
    public module left_api {
      public function result<T>() i32 { return answer(); }
    }
  )",
                                   R"(
    public module right_impl { public function answer() i32 { return 2; } }
    using right_impl;
    public module right_api {
      public function result<T>() i32 { return answer(); }
    }
  )"});
  initTestEnvironment();
  auto dir = std::filesystem::path(paths[0]).parent_path();
  auto left = dir / "left.moon";
  auto right = dir / "right.moon";
  ASSERT_NO_THROW(sun::MoonBuilder::build(paths[0], left));
  ASSERT_NO_THROW(sun::MoonBuilder::build(paths[1], right));
  auto driver = Driver::createForJIT("separate_bundle_file_ids");
  driver->setMoonImports(
      {sun::MoonImport(left.string()), sun::MoonImport(right.string())});
  EXPECT_EQ(driver->executeString(R"(
    function main() i32 {
      return left_api.result<i32>() + right_api.result<i32>();
    }
  )"),
            42);
}

TEST(Modules_FileImports, same_file_ambiguity_is_still_reported) {
  EXPECT_THROW(executeString(R"(
    public module left { public class Value { var n: i32; } }
    public module right { public class Value { var n: i64; } }
    using left;
    using right;
    function take(v: ref Value) i32 { return 0; }
    function main() i32 { return 0; }
  )"),
               SunError);
}

TEST(Modules_FileImports, targeted_import_does_not_import_other_symbols) {
  EXPECT_THROW(executeString(R"(
    public module lib {
      public function first() i32 { return 1; }
      public function second() i32 { return 2; }
    }
    using lib.first;
    function main() i32 { return second(); }
  )"),
               SunError);
}

namespace {
/** Inspect canonical module annotations throughout exported metadata. */
void visitModuleReferences(
    const google::protobuf::Message& message,
    const std::function<void(const sun::QualifiedName&)>& visit) {
  if (message.GetDescriptor() == sun::ast::ASTNode::descriptor()) {
    const auto& node = static_cast<const sun::ast::ASTNode&>(message);
    if (node.has_module_qualified_name())
      visit(sun::serialization::deserializeQualifiedName(
          node.module_qualified_name()));
  }
  auto* reflection = message.GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
      continue;
    if (field->is_repeated()) {
      for (int i = 0; i < reflection->FieldSize(message, field); ++i)
        visitModuleReferences(reflection->GetRepeatedMessage(message, field, i),
                              visit);
    } else
      visitModuleReferences(reflection->GetMessage(message, field), visit);
  }
}

/** Independent bundle versions used to test nominal metadata identities. */
class MoonExactTypes : public ::testing::Test {
 protected:
  std::filesystem::path dir;
  std::string b1, b2, a;

  /** Build a bundle from source and explicit dependencies. */
  std::string bundle(const std::string& name, const std::string& source,
                     std::vector<sun::MoonImport> imports = {}) {
    auto path = dir / (name + ".sun");
    std::ofstream(path) << source;
    auto output = dir / (name + ".moon");
    sun::MoonBuildOptions options;
    options.extraMoons = std::move(imports);
    sun::MoonBuilder::build(path.string(), output, options);
    return output.string();
  }

  void SetUp() override {
    initTestEnvironment();
    dir = std::filesystem::current_path() / "tmp" / "exact_types" /
          ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::create_directories(dir);
    b1 = bundle("b1", R"(
      public module b {
        public class Value { public var n: i32; }
        public function version() i32 { return 1; }
      }
    )");
    b2 = bundle("b2", R"(
      public module b {
        public class Value { public var n: i32; }
        public function version() i32 { return 2; }
      }
    )");
    a = bundle("a", R"(
      public module a {
        public function read(value: const ref b.Value) i32 { return value.n; }
      }
    )",
               {sun::MoonImport(b1)});
  }

  /** Compile or execute an application with its explicit import list. */
  sun::SunValue run(std::vector<sun::MoonImport> imports,
                    const std::string& source) {
    auto driver = Driver::createForJIT("exact_types");
    driver->setMoonImports(imports);
    return driver->executeString(source);
  }
};
}  // namespace

TEST_F(MoonExactTypes, missing_dependency_names_exact_declaration) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      run({sun::MoonImport(a)}, "function main() i32 { return 0; }"),
      "moon exact dependency");
}

TEST_F(MoonExactTypes, exact_version_works_in_either_import_order) {
  const std::string source = R"(
    function main() i32 {
      var value: b.Value = { n: 42 };
      return a.read(value);
    }
  )";
  EXPECT_EQ(run({sun::MoonImport(a), sun::MoonImport(b1)}, source), 42);
  EXPECT_EQ(run({sun::MoonImport(b1), sun::MoonImport(a)}, source), 42);
}

TEST_F(MoonExactTypes, wrong_version_cannot_rebind_signature) {
  EXPECT_SUN_ERROR_WITH_MESSAGE(run({sun::MoonImport(a), sun::MoonImport(b2)},
                                    "function main() i32 { return 0; }"),
                                "conflicting bundle supplied");
}

TEST_F(MoonExactTypes, aliased_equal_layout_versions_remain_distinct) {
  auto imports = std::vector<sun::MoonImport>{
      sun::MoonImport(a), sun::MoonImport(b1, "b", "old_b"),
      sun::MoonImport(b2, "b", "new_b")};
  EXPECT_EQ(run(imports, R"(
    function main() i32 {
      var value: old_b.Value = { n: 42 };
      return a.read(value) + old_b.version() + new_b.version();
    }
  )"),
            45);
  EXPECT_THROW(run(imports, R"(
    function main() i32 {
      var value: new_b.Value = { n: 42 };
      return a.read(value);
    }
  )"),
               SunError);
}

TEST_F(MoonExactTypes,
       implementation_only_dependency_needs_no_declaration_import) {
  auto implementation = bundle("implementation", R"(
    using b;
    public module implementation {
      public function result() i32 { return version(); }
    }
  )",
                               {sun::MoonImport(b1)});
  EXPECT_EQ(run({sun::MoonImport(implementation)}, R"(
    function main() i32 { return implementation.result(); }
  )"),
            1);
}

TEST_F(MoonExactTypes,
       build_time_aliases_survive_in_generic_definition_context) {
  auto generic = bundle("generic", R"(
    using original;
    public module generic {
      public function result<T>(value: const ref Value) i32 {
        return value.n + original.version();
      }
    }
  )",
                        {sun::MoonImport(b1, "b", "original")});
  EXPECT_EQ(
      run({sun::MoonImport(generic), sun::MoonImport(b1, "b", "application")},
          R"(
    function main() i32 {
      var value: application.Value = { n: 41 };
      return generic.result<i32>(value);
    }
  )"),
      42);
}

TEST_F(MoonExactTypes,
       recursive_metadata_keeps_hash_paths_and_symbolic_parameters) {
  auto shapes = bundle("shapes", R"(
    using b;
    public module shapes {
      declare HiddenAlias = Value;
      public class Unused<T> {
        var direct: HiddenAlias;
        var pointer: raw_ptr<Value>;
        var static_pointer: static_ptr<Value>;
        var values: array<Value, 2, 3>;
        var callback: function (const ref Value) Value;
        var closure: (const ref Value) => Value;
        var parameter: T;
      }
      public enum Payload<T> { Some(Value, T), None }
      public interface Contract { public var value: Value; }
      public var inferred = b.Value();
    }
  )",
                       {sun::MoonImport(b1)});
  auto dependency = sun::MoonReader::open(b1);
  auto reader = sun::MoonReader::open(shapes);
  ASSERT_TRUE(dependency);
  ASSERT_TRUE(reader);
  auto requiredHash =
      dependency->getMetadata(dependency->listModules()[0])->content_hash();
  size_t references = 0;
  bool symbolic = false, inferred = false, array = false;
  for (const auto& key : reader->listModules()) {
    const auto* md = reader->getMetadata(key);
    sun::serialization::visitQualifiedNames(*md, [&](const auto& ref, auto) {
      if (ref.baseName != "Value") return;
      EXPECT_EQ(ref.bundleHash(), requiredHash);
      EXPECT_EQ(ref.scopePath,
                (std::vector<std::string>{"$" + requiredHash + "$", "b"}));
      ++references;
    });
    for (const auto& cls : md->classes()) {
      for (const auto& field : cls.fields()) {
        if (field.name() == "parameter") {
          symbolic = field.type().base_name() == "T" &&
                     !field.type().has_qualified_name();
        }
        if (field.name() == "values") {
          array = field.type().array_dimensions_size() == 2 &&
                  field.type().array_dimensions(0) == 2 &&
                  field.type().array_dimensions(1) == 3;
        }
      }
    }
    for (const auto& global : md->globals()) {
      inferred = global.has_type_annotation() &&
                 global.type_annotation().has_qualified_name() &&
                 !global.has_value();
    }
  }
  EXPECT_GE(references, 9);
  EXPECT_TRUE(symbolic);
  EXPECT_TRUE(inferred);
  EXPECT_TRUE(array);
  EXPECT_SUN_ERROR_WITH_MESSAGE(
      run({sun::MoonImport(shapes)}, "function main() i32 { return 0; }"),
      "moon exact dependency");
  EXPECT_EQ(run({sun::MoonImport(shapes), sun::MoonImport(b1, "b", "renamed")},
                "function main() i32 { return 0; }"),
            0);
}

TEST_F(MoonExactTypes, nested_module_alias_preserves_compiled_symbols) {
  auto nested = bundle("nested", R"(
    public module outer {
      public module inner {
        public class Value { public var n: i32; }
        public function read(value: const ref Value) i32 { return value.n; }
      }
    }
  )");
  EXPECT_EQ(run({sun::MoonImport(nested, "outer", "other")}, R"(
    function main() i32 {
      var value: other.inner.Value = { n: 42 };
      return other.inner.read(value);
    }
  )"),
            42);
}

TEST_F(MoonExactTypes,
       generic_templates_interfaces_and_constraints_keep_their_version) {
  const auto definitions = [](int version) {
    return std::string(R"(
      public module dep {
        public interface Marker { public method value() i32; }
        public class Item implements Marker {
          public method value() i32 { return )") +
           std::to_string(version) + R"(; }
        }
        public class Box<T> { public var item: T; }
        public interface View<T> { public var item: T; }
        public enum Maybe<T> { Some(T), None }
      }
    )";
  };
  auto first = bundle("dep1", definitions(1));
  auto second = bundle("dep2", definitions(2));
  auto api = bundle("api", R"(
    using dep;
    public module api {
      public class Adapter implements Marker {
        public method value() i32 { return 42; }
      }
      public function constrained<T: Marker>(value: const ref T) i32 { return 42; }
      public function take_box(value: const ref dep.Box<i32>) i32 { return value.item; }
      public function take_enum(value: dep.Maybe<i32>) i32 { return 42; }
      public class Unused<T> { var contract: dep.View<T>; }
    }
  )",
                    {sun::MoonImport(first)});
  auto imports = std::vector<sun::MoonImport>{
      sun::MoonImport(api), sun::MoonImport(first, "dep", "old_dep"),
      sun::MoonImport(second, "dep", "new_dep")};
  EXPECT_EQ(run(imports, R"(
    function main() i32 {
      var box: old_dep.Box<i32> = { item: 42 };
      var value = old_dep.Item();
      var optional: old_dep.Maybe<i32> = old_dep.Maybe.None;
      return api.take_box(box) + api.constrained<old_dep.Item>(value) + api.take_enum(optional);
    }
  )"),
            126);
  EXPECT_THROW(run(imports, R"(
    function main() i32 {
      var box: new_dep.Box<i32> = { item: 42 };
      return api.take_box(box);
    }
  )"),
               SunError);
  EXPECT_SUN_ERROR_WITH_MESSAGE(run(imports, R"(
    function main() i32 {
      var value = new_dep.Item();
      return api.constrained<new_dep.Item>(value);
    }
  )"),
                                "does not satisfy constraint");
  EXPECT_THROW(run(imports, R"(
    function main() i32 {
      var optional: new_dep.Maybe<i32> = new_dep.Maybe.None;
      return api.take_enum(optional);
    }
  )"),
               SunError);
}

TEST_F(MoonExactTypes,
       different_layout_version_cannot_satisfy_exported_signature) {
  auto wider = bundle("wider", R"(
    public module b { public class Value { public var n: i64; public var extra: i64; } }
  )");
  EXPECT_THROW(run({sun::MoonImport(a), sun::MoonImport(b1, "b", "old_b"),
                    sun::MoonImport(wider, "b", "wide_b")},
                   R"(
    function main() i32 {
      var value: wide_b.Value = { n: 42, extra: 7 };
      return a.read(value);
    }
  )"),
               SunError);
}

TEST_F(MoonExactTypes, incorrect_declaration_kind_is_rejected_before_codegen) {
  auto contracts = bundle("contracts", R"(
    public module contracts {
      public interface Marker { public method value() i32; }
      public class Adapter<T> implements Marker {
        public method value() i32 { return 42; }
      }
      public function constrained<T: Marker>(value: const ref T) i32 { return 42; }
    }
  )");
  auto dependency = sun::MoonReader::open(b1);
  ASSERT_TRUE(dependency);
  auto hash =
      dependency->getMetadata(dependency->listModules()[0])->content_hash();
  sun::QualifiedName wrongName({"$" + hash + "$", "b"}, "Value");
  for (bool constraint : {false, true}) {
    SCOPED_TRACE(constraint ? "constraint" : "implemented interface");
    auto reader = sun::MoonReader::open(contracts);
    ASSERT_TRUE(reader);
    llvm::LLVMContext context;
    auto module = reader->loadModule(reader->listModules()[0], context);
    ASSERT_TRUE(module);
    sun::MoonWriter writer(
        reader->getMetadata(reader->listModules()[0])->content_hash());
    bool changed = false;
    for (const auto& key : reader->listModules()) {
      auto md = *reader->getMetadata(key);
      if (constraint) {
        for (auto& function : *md.mutable_functions()) {
          for (auto& param : *function.mutable_proto()->mutable_type_params()) {
            *param.mutable_qualified_name() =
                sun::serialization::serializeQualifiedName(wrongName);
            changed = true;
          }
        }
      } else {
        for (auto& cls : *md.mutable_classes()) {
          for (auto& impl : *cls.mutable_implemented_interfaces()) {
            *impl.mutable_qualified_name() =
                sun::serialization::serializeQualifiedName(wrongName);
            changed = true;
          }
        }
      }
      writer.addModule(*module, md);
    }
    ASSERT_TRUE(changed);
    auto malformed = dir / "wrong_kind.moon";
    ASSERT_TRUE(writer.write(malformed));
    EXPECT_SUN_ERROR_WITH_MESSAGE(
        run({sun::MoonImport(malformed.string()), sun::MoonImport(b1)},
            "function main() i32 { return 0; }"),
        "wrong type kind");
  }
}

TEST_F(MoonExactTypes, aliases_leave_compiled_function_names_unchanged) {
  auto reader = sun::MoonReader::open(b1);
  ASSERT_TRUE(reader);
  llvm::LLVMContext context;
  auto original = reader->loadModule(reader->listModules()[0], context);
  ASSERT_TRUE(original);
  std::string originalName;
  for (const auto& function : original->functions()) {
    if (function.getName().contains("_b_version"))
      originalName = function.getName().str();
  }
  ASSERT_FALSE(originalName.empty());
  auto driver = Driver::createForJIT("canonical_symbols");
  driver->setMoonImports({sun::MoonImport(b1, "b", "visible_alias")});
  driver->compileString(
      "function main() i32 { return visible_alias.version(); }");
  EXPECT_NE(driver->getModule().getFunction(originalName), nullptr);
  for (const auto& function : driver->getModule().functions())
    EXPECT_FALSE(function.getName().contains("_visible_alias_"));
}

TEST_F(MoonExactTypes, failed_build_does_not_publish_metadata) {
  auto path = dir / "broken.sun";
  std::ofstream(path) << "public module broken { public function bad() i32 { "
                         "return unknown; } }";
  auto output = dir / "broken.moon";
  EXPECT_THROW(sun::MoonBuilder::build(path.string(), output), SunError);
  EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(MoonExactTypes, old_format_is_rejected) {
  auto output = dir / "old.moon";
  std::filesystem::copy_file(a, output,
                             std::filesystem::copy_options::overwrite_existing);
  std::fstream file(output, std::ios::in | std::ios::out | std::ios::binary);
  sun::MoonHeader header;
  file.read(reinterpret_cast<char*>(&header), sizeof(header));
  --header.version;
  file.seekp(0);
  file.write(reinterpret_cast<const char*>(&header), sizeof(header));
  file.close();
  EXPECT_FALSE(sun::MoonReader::open(output));
}

TEST(MoonMetadata,
     canonical_type_round_trip_preserves_spelling_and_qualifiers) {
  TypeAnnotation nominal("build_alias.Value");
  nominal.qualifiedName =
      sun::QualifiedName{{"$1234abcd$", "original", "nested"},
                         "Value",
                         {"$1234abcd$", "original", "nested"}};
  nominal.typeArguments.push_back(std::make_unique<TypeAnnotation>("T"));
  nominal.lifetimeArguments = {"a"};
  TypeAnnotation reference("ref");
  reference.constRef = true;
  reference.lifetimeName = "a";
  reference.elementType = std::make_unique<TypeAnnotation>(nominal);
  TypeAnnotation callable("lambda");
  callable.canError = true;
  callable.refEnv = true;
  callable.lifetimeName = "a";
  callable.paramTypes.push_back(std::make_unique<TypeAnnotation>(reference));
  callable.returnType = std::make_unique<TypeAnnotation>(nominal);
  sun::serialization::ASTSerializer serializer;
  sun::serialization::ASTDeserializer deserializer;
  VariableCreationAST variable("callback", nullptr, callable);
  auto serialized = serializer.serialize(variable);
  auto restored = deserializer.deserialize(serialized);
  TypeAnnotation copied(
      *static_cast<const VariableCreationAST&>(*restored).getTypeAnnotation());
  TypeAnnotation assigned;
  assigned = copied;
  VariableCreationAST roundTrip("callback", nullptr, assigned);
  EXPECT_EQ(
      serializer.serialize(roundTrip)
          .variable_creation()
          .type_annotation()
          .SerializeAsString(),
      serialized.variable_creation().type_annotation().SerializeAsString());
  ASSERT_TRUE(assigned.returnType->qualifiedName);
  EXPECT_EQ(assigned.returnType->qualifiedName->modulePath,
            nominal.qualifiedName->modulePath);
  EXPECT_EQ(assigned.returnType->baseName, "build_alias.Value");
  EXPECT_EQ(assigned.returnType->qualifiedName->lookupName(),
            "$1234abcd$.original.nested.Value");
}

TEST(MoonMetadata, qualified_name_round_trip_preserves_owner_and_overload) {
  sun::QualifiedName name({"$1234abcd$", "original", "Container"}, "read",
                          {"$1234abcd$", "original"});
  name.paramSuffix = "$i32";
  auto restored = sun::serialization::deserializeQualifiedName(
      sun::serialization::serializeQualifiedName(name));
  EXPECT_EQ(restored, name);
  EXPECT_EQ(restored.modulePath, name.modulePath);
}

TEST(MoonMetadata, interface_requirement_does_not_apply_to_type_arguments) {
  sun::QualifiedName view({"$1234abcd$", "dep"}, "View");
  sun::QualifiedName value({"$1234abcd$", "dep"}, "Value");
  sun::ast::ImplementedInterface impl;
  *impl.mutable_qualified_name() =
      sun::serialization::serializeQualifiedName(view);
  *impl.add_type_arguments()->mutable_qualified_name() =
      sun::serialization::serializeQualifiedName(value);
  std::vector<std::pair<sun::QualifiedName, std::optional<sun::Type::Kind>>>
      uses;
  sun::serialization::visitQualifiedNames(
      impl,
      [&](const auto& name, auto kind) { uses.emplace_back(name, kind); });
  ASSERT_EQ(uses.size(), 2);
  for (const auto& [name, kind] : uses) {
    if (name == view)
      EXPECT_EQ(kind, sun::Type::Kind::Interface);
    else {
      EXPECT_EQ(name, value);
      EXPECT_FALSE(kind);
    }
  }
}

TEST_F(MoonExactTypes, generic_module_references_keep_the_defining_bundle) {
  auto first = bundle("first", R"(
    public module first {
      public function version<T>() i32 { return shared.version(); }
    }
  )",
                      {sun::MoonImport(b1, "b", "shared")});
  auto second = bundle("second", R"(
    public module second {
      public function version<T>() i32 { return shared.version(); }
    }
  )",
                       {sun::MoonImport(b2, "b", "shared")});
  std::vector<sun::MoonImport> imports{
      sun::MoonImport(first), sun::MoonImport(second),
      sun::MoonImport(b1, "b", "old_b"), sun::MoonImport(b2, "b", "new_b")};
  for (int order = 0; order < 2; ++order) {
    EXPECT_EQ(run(imports, R"(
      function main() i32 {
        return first.version<i32>() * 10 + second.version<i32>();
      }
    )"),
              12);
    EXPECT_THROW(run(imports, R"(
      function main() i32 { return shared.version(); }
    )"),
                 SunError);
    std::reverse(imports.begin(), imports.end());
  }
}

TEST_F(MoonExactTypes, module_references_are_canonical_in_all_source_files) {
  auto dep = bundle("nested", R"(
    public module outer {
      public module inner {
        public module deep {
          public function answer() i32 { return 21; }
        }
      }
    }
  )");
  std::ofstream(dir / "first.sun") << R"(
    public module api {
      public function first<T>() i32 { return build_root.inner.deep.answer(); }
    }
  )";
  std::ofstream(dir / "second.sun") << R"(
    public module api {
      public function second<T>() i32 { return build_root.inner.deep.answer(); }
    }
  )";
  auto api = bundle("multi", R"(
    manifest { source_files: ["first.sun", "second.sun"] }
  )",
                    {sun::MoonImport(dep, "outer", "build_root")});
  auto reader = sun::MoonReader::open(api);
  ASSERT_TRUE(reader);
  ASSERT_GE(reader->listModules().size(), 2);
  size_t references = 0;
  for (const auto& key : reader->listModules()) {
    visitModuleReferences(*reader->getMetadata(key), [&](const auto& name) {
      EXPECT_TRUE(name.lookupName().ends_with(".outer.inner.deep"));
      EXPECT_FALSE(name.bundleHash().empty());
      ++references;
    });
  }
  EXPECT_EQ(references, 2);
  EXPECT_EQ(sun::moon::ModuleMetadata::descriptor()->FindFieldByName(
                "module_bindings"),
            nullptr);
  EXPECT_EQ(
      run({sun::MoonImport(api), sun::MoonImport(dep, "outer", "client_root")},
          R"(
    function main() i32 { return api.first<i32>() + api.second<i32>(); }
  )"),
      42);
}

TEST_F(MoonExactTypes, explicit_nested_alias_preserves_ordinary_children) {
  auto dep = bundle("nested", R"(
    public module outer {
      public module inner {
        public module deep {
          public function answer() i32 { return 42; }
        }
      }
    }
  )");
  for (const std::string alias : {"mapped", "outer.mapped"}) {
    SCOPED_TRACE(alias);
    auto api =
        bundle("nested_api",
               "public module api { public function result<T>() i32 { return " +
                   alias + ".deep.answer(); } }",
               {sun::MoonImport(dep, "outer.inner", alias)});
    auto reader = sun::MoonReader::open(api);
    ASSERT_TRUE(reader);
    size_t references = 0;
    for (const auto& key : reader->listModules()) {
      visitModuleReferences(*reader->getMetadata(key), [&](const auto& name) {
        EXPECT_TRUE(name.lookupName().ends_with(".outer.inner.deep"));
        ++references;
      });
    }
    EXPECT_EQ(references, 1);
    EXPECT_EQ(run({sun::MoonImport(api),
                   sun::MoonImport(dep, "outer", "client_root")},
                  R"(
      function main() i32 { return api.result<i32>(); }
    )"),
              42);
  }
}

TEST(MoonMetadata, module_reference_round_trip_preserves_source_spelling) {
  sun::QualifiedName module({"$1234abcd$", "original"}, "inner",
                            {"$1234abcd$", "original"});
  VariableReferenceAST reference("build_alias.inner");
  reference.setModuleQualifiedName(module);
  auto clone = reference.clone();
  ASSERT_TRUE(clone->getModuleQualifiedName());
  EXPECT_EQ(*clone->getModuleQualifiedName(), module);
  EXPECT_EQ(clone->toString(), "build_alias.inner");
  UsingAST use({"build_alias"}, "renamed_inner");
  use.setModuleQualifiedName(module);
  use.setModuleImport(true);
  auto imported = use.clone();
  ASSERT_TRUE(imported->getModuleQualifiedName());
  EXPECT_EQ(*imported->getModuleQualifiedName(), module);
  EXPECT_EQ(imported->toString(), "using build_alias.renamed_inner");
  EXPECT_TRUE(static_cast<const UsingAST&>(*imported).isModuleImport());
}

TEST_F(MoonExactTypes, module_alias_shadowing_survives_generic_export) {
  auto api = bundle("shadowing", R"(
    public module api {
      public enum Result { Value(i32) }
      public function lambda<T>() i32 {
        var f = (dep: i32) => i32 { return dep; };
        return f(42);
      }
      public function pattern<T>() i32 {
        return match Result.Value(42) { Result.Value(dep) => dep };
      }
      public function loop<T>() i32 {
        var result: i32 = 0;
        for (var dep: i32 = 0; dep < 3; dep += 1) { result += dep; }
        return result + dep.version();
      }
      public function parameter<T>(dep: i32) i32 { return dep; }
      public function local<T>() i32 {
        var dep = dep.version();
        return dep;
      }
      public function block<T>() i32 {
        var result = dep.version();
        if (true) { var dep: i32 = 40; result += dep; }
        return result + 1;
      }
      public function nested<T>() i32 {
        var dep = () => i32 { return 42; };
        return dep();
      }
    }
  )",
                    {sun::MoonImport(b1, "b", "dep")});
  auto imports = std::vector<sun::MoonImport>{sun::MoonImport(api),
                                              sun::MoonImport(b1, "b", "old_b"),
                                              sun::MoonImport(b2, "b", "dep")};
  EXPECT_EQ(run(imports, R"(
    function main() i32 {
      return api.parameter<i32>(42) + api.local<i32>() + api.block<i32>() + api.nested<i32>() + api.lambda<i32>() + api.pattern<i32>() + api.loop<i32>();
    }
  )"),
            215);
}

TEST_F(MoonExactTypes,
       canonical_module_reference_defers_generic_overload_selection) {
  auto dep = bundle("overloads", R"(
    public module dep {
      public function pick(value: i32) i32 { return 1; }
      public function pick(value: bool) i32 { return 2; }
    }
  )");
  auto api = bundle("overload_api", R"(
    public module api {
      public function pick<T>(value: T) i32 { return build_dep.pick(value); }
    }
  )",
                    {sun::MoonImport(dep, "dep", "build_dep")});
  EXPECT_EQ(
      run({sun::MoonImport(api), sun::MoonImport(dep, "dep", "client_dep")}, R"(
    function main() i32 { return api.pick<i32>(42) * 10 + api.pick<bool>(true); }
  )"),
      12);
  EXPECT_THROW(
      run({sun::MoonImport(api), sun::MoonImport(dep, "dep", "client_dep")}, R"(
    function main() i32 { return api.pick<f64>(1.5); }
  )"),
      SunError);
}
TEST_F(MoonExactTypes, nested_using_target_is_canonical_in_generic_body) {
  auto dep = bundle("nested_using", R"(
    public module outer {
      public module inner { public function answer() i32 { return 42; } }
    }
  )");
  auto api = bundle("using_api", R"(
    public module api {
      public function result<T>() i32 {
        using outer.renamed;
        return answer();
      }
    }
  )",
                    {sun::MoonImport(dep, "outer.inner", "outer.renamed")});
  EXPECT_EQ(
      run({sun::MoonImport(api), sun::MoonImport(dep, "outer", "client_root")},
          R"(
    function main() i32 { return api.result<i32>(); }
  )"),
      42);
}

TEST(Modules_FileImports, field_initializers_survive_source_and_moon_imports) {
  auto paths = writeImportSources({R"(
    public module defaults {
      var calls: i32 = 0;
      function next() i32 { calls = calls + 1; return calls; }
      public class Value { public var n: i32 = next(); }
      public class Box<T> { public var value: Value = Value(); }
      public function count() i32 { return calls; }
    }
  )",
                                   R"(
    function main() i32 {
      var a = defaults.Value();
      var b = defaults.Box<i32>();
      var literal: defaults.Value = { n: 100 };
      return a.n + b.value.n + defaults.count() + literal.n - 100;
    }
  )"});
  EXPECT_EQ(
      Driver::createForJIT("field_initializer_sources")->executeFiles(paths),
      5);
  initTestEnvironment();
  auto moon = std::filesystem::path(paths[0]).parent_path() / "defaults.moon";
  ASSERT_NO_THROW(sun::MoonBuilder::build(paths[0], moon));
  auto driver = Driver::createForJIT("field_initializer_bundle");
  driver->setMoonImports({sun::MoonImport(moon.string())});
  EXPECT_EQ(driver->executeString(R"(
    function main() i32 {
      var a = defaults.Value();
      var b = defaults.Box<i32>();
      var literal: defaults.Value = { n: 100 };
      return a.n + b.value.n + defaults.count() + literal.n - 100;
    }
  )"),
            5);
}
