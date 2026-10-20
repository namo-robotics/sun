/** Exercises independently reconstructed declarations and retained generic
 * bodies. */
#include <google/protobuf/unknown_field_set.h>
#include <gtest/gtest.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <unistd.h>

#include <cstring>
#include <fstream>

#include "abi/abi_compatibility_checker.h"
#include "abi/semantic_requirements.h"
#include "moon_bundling/moon.h"
#include "support/error.h"

/** Diagnostic changes and declaration-map traversal do not change semantics. */
TEST(ABI_Compatibility,
     SemanticEncodingExcludesDiagnosticsAndSortsDeclarations) {
  sun::moon::ModuleMetadata metadata;
  metadata.set_module_name("example");
  metadata.add_declarations()->set_key("first");
  metadata.add_declarations()->set_key("second");
  auto* variable = metadata.add_globals();
  variable->set_name("value");
  variable->mutable_type_annotation()->set_base_name("i32");
  const auto original = sun::abi::describeSemantics(metadata);
  metadata.mutable_declarations()->SwapElements(0, 1);
  metadata.set_source_path("/another/source.sun");
  variable->set_source_file_id(99);
  variable->set_doc("New documentation");
  metadata.GetReflection()->MutableUnknownFields(&metadata)->AddVarint(1000, 7);
  EXPECT_EQ(original, sun::abi::describeSemantics(metadata));
  variable->mutable_type_annotation()->set_base_name("i64");
  EXPECT_NE(original, sun::abi::describeSemantics(metadata));
}

/** Retained statements remain ordered, and private generic body changes are
 * recorded. */
TEST(ABI_Compatibility, SemanticEncodingPreservesRetainedBodyOrder) {
  sun::moon::ModuleMetadata metadata;
  auto* function = metadata.add_functions();
  function->mutable_proto()->set_name("private_generic");
  auto* body = function->mutable_body();
  body->add_body()->mutable_bool_literal()->set_value(true);
  body->add_body()->mutable_bool_literal()->set_value(false);
  const auto original = sun::abi::describeSemantics(metadata);
  function->mutable_location()->set_file_path("ignored.sun");
  function->set_source_text("optional source text");
  EXPECT_EQ(original, sun::abi::describeSemantics(metadata));
  body->mutable_body()->SwapElements(0, 1);
  EXPECT_NE(original, sun::abi::describeSemantics(metadata));
  body->add_body();
  EXPECT_THROW(sun::abi::describeSemantics(metadata), sun::support::SunError);
}

/** Altered type metadata is rejected even when LLVM code and the supplied hash
 * match. */
TEST(ABI_Compatibility, AlteredDeclarationMetadataIsRejectedBeforeImport) {
  llvm::LLVMContext context;
  llvm::Module module("metadata", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("metadata");
  auto* variable = metadata.add_globals();
  variable->set_name("value");
  variable->mutable_type_annotation()->set_base_name("i32");
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  writer.addModule(module, metadata);
  std::filesystem::create_directories("tmp");
  const auto path = std::filesystem::path("tmp") /
                    ("abi_metadata_" + std::to_string(getpid()) + ".moon");
  ASSERT_TRUE(writer.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  EXPECT_NO_THROW(checker.validate(*reader));
  auto changed = *reader->getMetadata("metadata");
  const auto before = changed.SerializeAsString();
  changed.mutable_globals(0)->mutable_type_annotation()->set_base_name("i64");
  const auto after = changed.SerializeAsString();
  ASSERT_EQ(before.size(), after.size());
  auto bytes = *reader->getBytes();
  const auto offset = bytes.find(before);
  ASSERT_NE(offset, std::string::npos);
  bytes.replace(offset, before.size(), after);
  {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), bytes.size());
  }
  auto altered = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(altered, nullptr);
  try {
    checker.validate(*altered);
    FAIL() << "Accepted altered semantic metadata";
  } catch (const sun::support::SunError& error) {
    EXPECT_NE(std::string(error.what())
                  .find("declarations or retained bodies differ"),
              std::string::npos);
  }
  EXPECT_EQ(checker.importedContracts().size(), 1u);
  std::filesystem::remove(path);
}

/** Ownership is checked before declarations can populate the consumer's tables.
 */
TEST(ABI_Compatibility,
     DeclarationGraphRejectsDuplicatesMissingOwnersAndCycles) {
  sun::moon::ModuleMetadata metadata;
  auto* module = metadata.add_declarations();
  module->set_key("module");
  module->set_kind(sun::moon::MODULE);
  auto* function = metadata.add_declarations();
  function->set_key("function");
  function->set_kind(sun::moon::FUNCTION);
  function->set_owner("module");
  function->set_module("module");
  metadata.add_module_declarations("module");
  EXPECT_NO_THROW(sun::abi::validateDeclarationGraph({&metadata}));
  function->set_owner("missing");
  EXPECT_THROW(sun::abi::validateDeclarationGraph({&metadata}),
               sun::support::SunError);
  function->set_owner("function");
  EXPECT_THROW(sun::abi::validateDeclarationGraph({&metadata}),
               sun::support::SunError);
  function->set_owner("module");
  *metadata.add_declarations() = *function;
  EXPECT_THROW(sun::abi::validateDeclarationGraph({&metadata}),
               sun::support::SunError);
}

/** An unchanged old artifact cannot be authorized merely by its producer
 * version. */
TEST(ABI_Compatibility,
     HistoricalExperimentalBundleRemainsUnchangedAndUnsupported) {
  const auto path = "tests/abi/fixtures/experimental-v1.moon";
  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input);
  const std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
  EXPECT_EQ(sun::moon_bundling::computeSha256Hex(bytes),
            "1df3009fa8a5e9d0052a7903391681b63ec2e08d21c9f332eed019b147c9b650");
  EXPECT_THROW(sun::moon_bundling::MoonReader::open(path),
               sun::support::SunError);
}

/** Retained bodies and nested types must resolve exact cross-module identities.
 */
TEST(ABI_Compatibility,
     RetainedReferencesResolveAcrossCompleteDeclarationGraph) {
  sun::moon::ModuleMetadata owner, user;
  auto* record = owner.add_declarations();
  record->set_key("private-class");
  record->set_kind(sun::moon::CLASS);
  auto* function = user.add_functions();
  auto* reference = function->mutable_proto()->mutable_return_type();
  reference->set_declaration_key("private-class");
  EXPECT_NO_THROW(sun::abi::validateDeclarationGraph({&owner, &user}));
  EXPECT_THROW(sun::abi::validateDeclarationGraph({&user}),
               sun::support::SunError);
  reference->mutable_element_type()->set_declaration_key("missing-nested-type");
  EXPECT_THROW(sun::abi::validateDeclarationGraph({&owner, &user}),
               sun::support::SunError);
  reference->clear_element_type();
  auto* node = function->mutable_body()->add_body();
  node->mutable_bool_literal()->set_value(true);
  node->set_module_declaration_key("private-class");
  EXPECT_THROW(sun::abi::validateDeclarationGraph({&owner, &user}),
               sun::support::SunError);
  record->set_kind(sun::moon::MODULE);
  EXPECT_NO_THROW(sun::abi::validateDeclarationGraph({&owner, &user}));
  node->mutable_declaration_identity()->add_parameters("private-class");
  EXPECT_THROW(sun::abi::validateDeclarationGraph({&owner, &user}),
               sun::support::SunError);
}

/** Local dangling keys fail while explicit external imports remain order
 * independent. */
TEST(ABI_Compatibility, LocalReferencesCannotHideAsExternalDependencies) {
  sun::moon::ModuleMetadata metadata;
  const std::string localScope = "$" + std::string(64, 'a') + "$_";
  auto* record = metadata.add_declarations();
  record->set_key(localScope + "1");
  record->set_kind(sun::moon::CLASS);
  auto* reference = metadata.add_globals()->mutable_type_annotation();
  reference->set_declaration_key(localScope + "2");
  EXPECT_THROW(sun::abi::validateDeclarationGraph({&metadata}),
               sun::support::SunError);
  reference->set_declaration_key("$" + std::string(64, 'b') + "$_1");
  EXPECT_NO_THROW(sun::abi::validateDeclarationGraph({&metadata}));
  reference->set_declaration_key("$" + std::string(64, 'z') + "$_1");
  EXPECT_THROW(sun::abi::validateDeclarationGraph({&metadata}),
               sun::support::SunError);
}

/** Rehashed specialization claims must still match their shared LLVM payload.
 */
TEST(ABI_Compatibility, RehashedSpecializationCannotInventMethodBodies) {
  llvm::LLVMContext context;
  llvm::Module module("specialization", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  llvm::IRBuilder<> builder(context);
  auto* function = llvm::Function::Create(
      llvm::FunctionType::get(builder.getVoidTy(), false),
      llvm::GlobalValue::ExternalLinkage, "present_method", module);
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
  builder.CreateRetVoid();
  sun::moon::ModuleMetadata first, second;
  first.set_source_hash("first");
  second.set_source_hash("second");
  auto* shape = second.add_classes()->add_compiled_specializations();
  shape->set_declaration_key("shape");
  shape->add_method_symbols("present_method");
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  writer.addModule(module, first);
  writer.addModule(module, second);
  std::filesystem::create_directories("tmp");
  const auto path =
      std::filesystem::path("tmp") /
      ("abi_specialization_" + std::to_string(getpid()) + ".moon");
  ASSERT_TRUE(writer.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  EXPECT_NO_THROW(checker.validate(*reader));
  auto library = reader->getLibrary();
  auto* changed = library.mutable_modules(1)->mutable_metadata();
  const auto oldRecord = sun::abi::describeSemantics(*changed);
  changed->mutable_classes(0)
      ->mutable_compiled_specializations(0)
      ->set_method_symbols(0, "missing_method");
  auto* contract = library.mutable_bundle_contract();
  for (auto& record : *contract->mutable_semantic_records())
    if (record == oldRecord) record = sun::abi::describeSemantics(*changed);
  contract->set_compatibility_hash(sun::abi::hashBundleContract(*contract));
  auto bytes = *reader->getBytes();
  sun::moon_bundling::MoonHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  const auto encoded = library.SerializeAsString();
  bytes.replace(header.bundleMetadataOffset, header.bundleMetadataSize,
                encoded);
  header.bundleMetadataSize = encoded.size();
  std::memcpy(bytes.data(), &header, sizeof(header));
  {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), bytes.size());
  }
  auto altered = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(altered, nullptr);
  try {
    checker.validate(*altered);
    FAIL() << "Accepted specialization without its promised method body";
  } catch (const sun::support::SunError& error) {
    EXPECT_NE(std::string(error.what()).find("missing_method"),
              std::string::npos);
    EXPECT_NE(std::string(error.what()).find(contract->compatibility_hash()),
              std::string::npos);
  }
  std::filesystem::remove(path);
}
