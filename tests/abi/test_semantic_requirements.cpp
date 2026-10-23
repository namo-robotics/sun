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
#include "abi/lowering_descriptors.h"
#include "abi/semantic_requirements.h"
#include "moon_bundling/moon.h"
#include "semantic_analysis/portable_declaration_key.h"
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
  EXPECT_THROW(sun::abi::validateDeclarationGraph({&metadata}, true),
               sun::support::SunError);
  sun::moon::ModuleMetadata dependency;
  auto* external = dependency.add_declarations();
  external->set_key(reference->declaration_key());
  external->set_kind(sun::moon::CLASS);
  EXPECT_NO_THROW(
      sun::abi::validateDeclarationGraph({&metadata, &dependency}, true));
  EXPECT_NO_THROW(sun::abi::validateDeclarationGraph(
      {&dependency, &metadata, &dependency}, true));
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
  for (auto& record : *contract->mutable_semantic_records()) {
    if (record == oldRecord) {
      record = sun::abi::describeSemantics(*changed);
    }
  }
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

/** Unused imports need no declarations, but retained code keeps its lookup
 * scope. */
TEST(ABI_Compatibility, NavigationImportsAreRequiredForRetainedCode) {
  sun::moon::ModuleMetadata metadata;
  auto* navigation = metadata.add_using_declarations();
  navigation->mutable_using_stmt()->set_is_module_import(true);
  navigation->set_module_declaration_key("$" + std::string(64, 'e') +
                                         "$_module");
  EXPECT_NO_THROW(sun::abi::validateDeclarationGraph({&metadata}, true));
  metadata.add_functions()
      ->mutable_body()
      ->add_body()
      ->mutable_bool_literal()
      ->set_value(true);
  EXPECT_THROW(sun::abi::validateDeclarationGraph({&metadata}, true),
               sun::support::SunError);
  sun::moon::ModuleMetadata dependency;
  auto* module = dependency.add_declarations();
  module->set_key(navigation->module_declaration_key());
  module->set_kind(sun::moon::MODULE);
  EXPECT_NO_THROW(
      sun::abi::validateDeclarationGraph({&metadata, &dependency}, true));
}

/** Recomputing metadata and contract hashes cannot bless a changed class ABI.
 */
TEST(ABI_Compatibility, RehashedClassMetadataMustMatchCompiledLayout) {
  llvm::LLVMContext context;
  llvm::Module module("layout", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  const auto functionKey =
      sun::semantic_analysis::PortableDeclarationKey::original(
          std::string(64, 'a'), 1);
  const auto classKey =
      sun::semantic_analysis::PortableDeclarationKey::original(
          std::string(64, 'a'), 2);
  auto* storage = sun::abi::AggregateLayout::build(
      context,
      {llvm::Type::getInt8Ty(context), llvm::Type::getInt64Ty(context)});
  sun::abi::recordClassLayout(module, classKey.encoding(), storage);
  auto* function = llvm::Function::Create(
      sun::abi::CallDescriptor::sun(llvm::Type::getInt32Ty(context),
                                    {llvm::PointerType::getUnqual(context)})
          .signature,
      llvm::GlobalValue::ExternalLinkage, functionKey.symbol("function"),
      module);
  llvm::IRBuilder<> builder(
      llvm::BasicBlock::Create(context, "entry", function));
  builder.CreateRet(builder.getInt32(42));
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("layout");
  auto* record = metadata.add_declarations();
  record->set_key(functionKey.encoding());
  record->set_kind(sun::moon::FUNCTION);
  record = metadata.add_declarations();
  record->set_key(classKey.encoding());
  record->set_kind(sun::moon::CLASS);
  auto* cls = metadata.add_classes();
  cls->set_name("Pair");
  cls->mutable_declaration_identity()->set_key(classKey.encoding());
  cls->add_fields()->mutable_type()->set_base_name("i8");
  cls->add_fields()->mutable_type()->set_base_name("i64");
  auto* proto = metadata.add_functions()->mutable_proto();
  proto->set_name("make");
  proto->mutable_declaration_identity()->set_key(functionKey.encoding());
  proto->mutable_return_type()->set_base_name("i32");
  auto* parameter = proto->add_args()->mutable_type();
  parameter->set_base_name("ref");
  parameter->mutable_element_type()->set_base_name("Pair");
  parameter->mutable_element_type()->set_declaration_key(classKey.encoding());
  const auto path =
      std::filesystem::path("tmp") /
      ("abi_payload_layout_" + std::to_string(getpid()) + ".moon");
  std::filesystem::create_directories("tmp");
  /** Writes a newly hashed contract while preserving the original LLVM body. */
  auto write = [&](const sun::moon::ModuleMetadata& description) {
    sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
    writer.addModule(module, description);
    EXPECT_TRUE(writer.write(path));
    return sun::moon_bundling::MoonReader::open(path);
  };
  auto reader = write(metadata);
  ASSERT_NE(reader, nullptr);
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  EXPECT_NO_THROW(checker.validate(*reader));
  for (int change = 0; change < 5; ++change) {
    auto changed = metadata;
    if (change == 0) {
      changed.mutable_classes(0)
          ->mutable_fields(1)
          ->mutable_type()
          ->set_base_name("i32");
    }
    if (change == 1) {
      changed.mutable_classes(0)->set_is_packed(true);
    }
    if (change == 2) {
      changed.mutable_classes(0)->set_alignment(32);
    }
    if (change == 3) {
      changed.mutable_functions(0)
          ->mutable_proto()
          ->mutable_return_type()
          ->set_base_name("i64");
    }
    if (change == 4) {
      module.eraseNamedMetadata(module.getNamedMetadata("sun.abi.classes"));
    }
    auto altered = write(changed);
    ASSERT_NE(altered, nullptr);
    checker.beginCompilation("x86_64-linux-gnu");
    try {
      checker.validate(*altered);
      FAIL() << "Accepted changed source layout against unchanged code";
    } catch (const sun::support::SunError& error) {
      EXPECT_NE(std::string(error.what()).find("ABI"), std::string::npos);
    }
  }
  std::filesystem::remove(path);
}

/** Enum payload metadata is checked against independently lowered call types.
 */
TEST(ABI_Compatibility, EnumMetadataMustMatchCompiledSignature) {
  llvm::LLVMContext context;
  llvm::Module module("enum", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  const auto functionKey =
      sun::semantic_analysis::PortableDeclarationKey::original(
          std::string(64, 'a'), 1);
  const auto enumKey = sun::semantic_analysis::PortableDeclarationKey::original(
      std::string(64, 'a'), 2);
  auto layout = sun::abi::EnumLayout::build(
      context, module.getDataLayout(), {{}, {llvm::Type::getInt64Ty(context)}});
  auto* function = llvm::Function::Create(
      sun::abi::CallDescriptor::sun(llvm::Type::getVoidTy(context),
                                    {layout.storage})
          .signature,
      llvm::GlobalValue::ExternalLinkage, functionKey.symbol("function"),
      module);
  llvm::IRBuilder<> builder(
      llvm::BasicBlock::Create(context, "entry", function));
  builder.CreateRetVoid();
  sun::moon::ModuleMetadata metadata;
  auto* enumeration = metadata.add_enums();
  enumeration->set_name("Option");
  enumeration->mutable_declaration_identity()->set_key(enumKey.encoding());
  enumeration->add_variants()->set_name("None");
  auto* variant = enumeration->add_variants();
  variant->set_name("Some");
  variant->add_payload_types()->set_base_name("i64");
  auto* proto = metadata.add_functions()->mutable_proto();
  proto->set_name("consume");
  proto->mutable_declaration_identity()->set_key(functionKey.encoding());
  proto->mutable_return_type()->set_base_name("void");
  auto* argument = proto->add_args()->mutable_type();
  argument->set_base_name("Option");
  argument->set_declaration_key(enumKey.encoding());
  EXPECT_NO_THROW(sun::abi::validatePayloadDeclarations({&metadata}, module));
  variant->mutable_payload_types(0)->set_base_name("i32");
  EXPECT_THROW(sun::abi::validatePayloadDeclarations({&metadata}, module),
               sun::support::SunError);
  variant->mutable_payload_types(0)->set_base_name("i64");
  function->deleteBody();
  EXPECT_THROW(sun::abi::validatePayloadDeclarations({&metadata}, module),
               sun::support::SunError);
}

/** Generic arguments behind pointers do not create by-value storage cycles. */
TEST(ABI_Compatibility, RecursiveGenericStorageFollowsPointerBoundaries) {
  llvm::LLVMContext context;
  llvm::Module module("recursive", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  const auto nodeKey = sun::semantic_analysis::PortableDeclarationKey::original(
      std::string(64, 'a'), 1);
  const auto boxKey = sun::semantic_analysis::PortableDeclarationKey::original(
      std::string(64, 'a'), 2);
  auto* boxStorage = sun::abi::AggregateLayout::build(
      context, {llvm::PointerType::getUnqual(context)});
  sun::abi::recordClassLayout(
      module, nodeKey.encoding(),
      sun::abi::AggregateLayout::build(context, {boxStorage}));
  sun::moon::ModuleMetadata metadata;
  auto* node = metadata.add_classes();
  node->set_name("Node");
  node->mutable_declaration_identity()->set_key(nodeKey.encoding());
  auto* boxField = node->add_fields()->mutable_type();
  boxField->set_base_name("Box");
  boxField->set_declaration_key(boxKey.encoding());
  auto* argument = boxField->add_type_arguments();
  argument->set_base_name("Node");
  argument->set_declaration_key(nodeKey.encoding());
  auto* box = metadata.add_classes();
  box->set_name("Box");
  box->mutable_declaration_identity()->set_key(boxKey.encoding());
  box->add_type_params()->set_name("T");
  auto* pointer = box->add_fields()->mutable_type();
  pointer->set_base_name("raw_ptr");
  pointer->mutable_element_type()->set_base_name("T");
  EXPECT_NO_THROW(sun::abi::validatePayloadDeclarations({&metadata}, module));
  pointer->Clear();
  pointer->set_base_name("T");
  EXPECT_THROW(sun::abi::validatePayloadDeclarations({&metadata}, module),
               sun::support::SunError);
}
