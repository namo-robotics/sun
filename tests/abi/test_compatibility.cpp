/** Exercises ABI contract encoding, support decisions, and Moon integration. */
#include <gtest/gtest.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/ADT/StringMap.h>
#include <google/protobuf/unknown_field_set.h>
#include <unistd.h>
#include <dlfcn.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "abi/compatibility.h"
#include "abi/boundary_requirements.h"
#include "abi/producer_identity.h"
#include "abi/abi_compatibility_checker.h"
#include "moon_bundling/moon.h"
#include "support/error.h"

/** Keeps test construction helpers private to this translation unit. */
namespace {
/** Creates a deterministic host contract without relying on source compilation. */
sun::abi::BundleContract contract() {
  llvm::LLVMContext context;
  llvm::Module module("contract", context);
  module.setTargetTriple("x86_64-unknown-linux-gnu");
  module.setDataLayout(sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  return sun::abi::describeContract(module);
}
/** Uses one temporary bundle path per test process. */
std::filesystem::path bundlePath() {
  std::filesystem::create_directories("tmp");
  return std::filesystem::path("tmp") / ("abi_contract_" + std::to_string(getpid()) + ".moon");
}
}  // namespace

/** Application libraries loaded before import are not compiler dependencies. */
TEST(ABI_Compatibility, ApplicationLibrariesDoNotChangeImplementation) {
  void* library = dlopen(SUN_FFI_TESTLIB_DIR "/libsun_ffi_testlib.so", RTLD_NOW);
  ASSERT_NE(library, nullptr) << dlerror();
  auto reader = sun::moon_bundling::MoonReader::open("build/stdlib.moon");
  ASSERT_NE(reader, nullptr);
  EXPECT_NO_THROW(sun::abi::AbiCompatibilityChecker::current().validate(*reader, "x86_64-linux-gnu"));
  dlclose(library);
}

/** Set traversal and diagnostic metadata must not change compiled requirements. */
TEST(ABI_Compatibility, CanonicalSetsExcludeProducerMetadata) {
  auto original = contract();
  original.add_boundary_records("second");
  original.add_boundary_records("first");
  auto reordered = original;
  reordered.mutable_boundary_records()->SwapElements(0, 1);
  reordered.mutable_requirements()->SwapElements(0, 1);
  reordered.mutable_producer()->set_build("future compiler");
  reordered.mutable_producer()->set_release_order(999);
  EXPECT_EQ(sun::abi::encodeBundleContract(original), sun::abi::encodeBundleContract(reordered));
  reordered.add_boundary_records("new boundary");
  EXPECT_NE(sun::abi::hashBundleContract(original), sun::abi::hashBundleContract(reordered));
}

/** Recomputing a hash cannot authorize an unknown semantic requirement. */
TEST(ABI_Compatibility, UnknownRequiredRuleIsRejected) {
  auto unsupported = contract();
  auto* rule = unsupported.add_requirements();
  rule->set_name("unknown.required.behavior");
  rule->set_implementation("1");
  unsupported.set_compatibility_hash(sun::abi::hashBundleContract(unsupported));
  EXPECT_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(unsupported, "unknown.moon", "x86_64-linux-gnu"), sun::support::SunError);
}

/** A supplied implementation fingerprint must be independently supported. */
TEST(ABI_Compatibility, ChangedFingerprintIsRejected) {
  auto unsupported = contract();
  unsupported.mutable_requirements(1)->set_implementation(std::string(64, '0'));
  unsupported.set_compatibility_hash(sun::abi::hashBundleContract(unsupported));
  EXPECT_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(unsupported, "changed.moon", "x86_64-linux-gnu"), sun::support::SunError);
}

/** Required rules cannot disappear merely because the supplied hash is consistent. */
TEST(ABI_Compatibility, MissingRuleIsRejected) {
  auto unsupported = contract();
  unsupported.mutable_requirements()->RemoveLast();
  unsupported.set_compatibility_hash(sun::abi::hashBundleContract(unsupported));
  EXPECT_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(unsupported, "missing.moon", "x86_64-linux-gnu"), sun::support::SunError);
}

/** Unknown optional protobuf fields have no effect on the canonical contract. */
TEST(ABI_Compatibility, OptionalExtensionsAreSkippable) {
  auto supported = contract();
  const auto hash = supported.compatibility_hash();
  supported.GetReflection()->MutableUnknownFields(&supported)->AddLengthDelimited(1000, "optional diagnostics");
  EXPECT_EQ(hash, sun::abi::hashBundleContract(supported));
  EXPECT_NO_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(supported, "optional.moon", "x86_64-linux-gnu"));
}

/** Transitive requirements are deduplicated and conflicting implementations rejected. */
TEST(ABI_Compatibility, InheritanceRetainsAndChecksRequirements) {
  auto outer = contract();
  auto inner = contract();
  llvm::LLVMContext context;
  llvm::Module module("inherited", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(inner.target().data_layout());
  llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(context), false),
                        llvm::Function::ExternalLinkage, "inherited_function", module);
  inner = sun::abi::describeContract(module);
  sun::abi::mergeContractRequirements(outer, inner);
  EXPECT_EQ(outer.requirements_size(), inner.requirements_size());
  EXPECT_EQ(outer.boundary_records(0), inner.boundary_records(0));
  inner.mutable_requirements(0)->set_implementation("unsupported replacement");
  EXPECT_THROW(sun::abi::mergeContractRequirements(outer, inner), sun::support::SunError);
}

/** Equivalent triples match, while different platform ABIs cannot authorize imports. */
TEST(ABI_Compatibility, TargetProfilesDistinguishPlatformABI) {
  const auto supported = contract();
  EXPECT_NO_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(supported, "target.moon", "x86_64-linux-gnu"));
  EXPECT_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(supported, "target.moon", "x86_64-linux-musl"), sun::support::SunError);
}

/** Bundle metadata owns the contract and shared code regions validate once. */
TEST(ABI_Compatibility, MultiModuleRoundTripHasOneContract) {
  sun::abi::AbiCompatibilityChecker::current().beginCompilation("x86_64-linux-gnu");
  llvm::LLVMContext context;
  llvm::Module module("roundtrip", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  sun::moon::ModuleMetadata first;
  first.set_source_hash("first");
  writer.addModule(module, first);
  first.set_source_hash("second");
  writer.addModule(module, first);
  const auto path = bundlePath();
  ASSERT_TRUE(writer.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(reader->getLibrary().has_bundle_contract());
  EXPECT_EQ(reader->getLibrary().content_hash(), std::string(64, 'a'));
  ASSERT_EQ(reader->getLibrary().modules_size(), 2);
  EXPECT_EQ(reader->getLibrary().modules(1).metadata().source_hash(), "second");
  EXPECT_EQ(sun::moon::ModuleMetadata::descriptor()->FindFieldByName("bundle_contract"), nullptr);
  EXPECT_NO_THROW(sun::abi::AbiCompatibilityChecker::current().validate(*reader));
  EXPECT_EQ(reader->getBitcodeId("first"), reader->getBitcodeId("second"));
  std::filesystem::remove(path);
}

/** Truncated headers are rejected before any index counts or sizes are used. */
TEST(ABI_Compatibility, TruncatedContainerIsRejected) {
  const auto path = bundlePath();
  { std::ofstream output(path, std::ios::binary); output << "SUN"; }
  EXPECT_THROW(sun::moon_bundling::MoonReader::open(path), sun::support::SunError);
  std::filesystem::remove(path);
}

/** Byte-keyed authorization survives path replacement without reusing old contracts. */
TEST(ABI_Compatibility, CheckerOwnsAuthorizationAndTracksImmutableSnapshots) {
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  llvm::LLVMContext context;
  llvm::Module module("snapshots", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  auto* signature = llvm::FunctionType::get(llvm::Type::getInt32Ty(context), false);
  auto* function = llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                                         "answer", module);
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("snapshot");
  const auto path = bundlePath();
  sun::moon_bundling::MoonWriter firstWriter(std::string(64, 'a'));
  firstWriter.addModule(module, metadata);
  ASSERT_TRUE(firstWriter.write(path));
  auto first = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(first, nullptr);
  const auto firstDigest = checker.bytesHash(*first);
  auto authorization = checker.validate(*first);
  const auto firstContract = authorization.contract().compatibility_hash();
  EXPECT_EQ(checker.validate(*first).contract().compatibility_hash(), firstContract);

  function->setCallingConv(llvm::CallingConv::Fast);
  sun::moon_bundling::MoonWriter replacement(std::string(64, 'a'));
  replacement.addModule(module, metadata);
  ASSERT_TRUE(replacement.write(path));
  auto second = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(checker.bytesHash(*second), firstDigest);
  EXPECT_THROW(checker.validate(*second), sun::support::SunError);
  EXPECT_EQ(checker.importedContracts().size(), 1u);
  checker.beginCompilation("x86_64-linux-gnu");
  EXPECT_NE(checker.validate(*second).contract().compatibility_hash(), firstContract);
  EXPECT_EQ(checker.bytesHash(*first), firstDigest);
  EXPECT_EQ(checker.importedContracts().size(), 1u);

  first.reset();
  checker.beginCompilation("x86_64-linux-musl");
  EXPECT_TRUE(checker.importedContracts().empty());
  EXPECT_EQ(authorization.contract().compatibility_hash(), firstContract);
  EXPECT_THROW(checker.validate(*second), sun::support::SunError);
  std::filesystem::remove(path);
}

/** Release ordering requires complete provenance and never compares version strings. */
TEST(ABI_Compatibility, ProducerOrderingRequiresComparableProvenance) {
  using sun::abi::ProducerOrdering;
  using sun::abi::compareProducerIdentity;
  sun::abi::ProducerIdentity consumer;
  consumer.set_build("consumer");
  consumer.set_release_series("official");
  consumer.set_release_order(20);
  auto producer = consumer;
  EXPECT_EQ(compareProducerIdentity(producer, consumer), ProducerOrdering::SameBuild);
  producer.set_build("producer");
  EXPECT_EQ(compareProducerIdentity(producer, consumer), ProducerOrdering::Unknown);
  producer.set_release_order(19);
  EXPECT_EQ(compareProducerIdentity(producer, consumer), ProducerOrdering::Older);
  producer.set_release_order(21);
  EXPECT_EQ(compareProducerIdentity(producer, consumer), ProducerOrdering::Newer);
  producer.set_release_series("another-series");
  EXPECT_EQ(compareProducerIdentity(producer, consumer), ProducerOrdering::Unknown);
  producer.set_release_series("official");
  producer.clear_release_order();
  EXPECT_EQ(compareProducerIdentity(producer, consumer), ProducerOrdering::Unknown);
  producer.set_release_order(19);
  consumer.clear_release_order();
  EXPECT_EQ(compareProducerIdentity(producer, consumer), ProducerOrdering::Unknown);
  consumer.set_release_order(20);
  producer.clear_build();
  EXPECT_EQ(compareProducerIdentity(producer, consumer), ProducerOrdering::Unknown);
  consumer.clear_build();
  EXPECT_EQ(compareProducerIdentity(producer, consumer), ProducerOrdering::Unknown);
}

/** Bundles record the generated producer without making it part of the contract. */
TEST(ABI_Compatibility, GeneratedProducerIsDiagnosticOnly) {
  const auto identity = sun::abi::currentProducerIdentity();
  auto description = contract();
  EXPECT_EQ(description.producer().SerializeAsString(), identity.SerializeAsString());
  const auto hash = description.compatibility_hash();
  description.mutable_producer()->set_build("different-build");
  description.mutable_producer()->set_release_order(1);
  description.mutable_producer()->set_release_series("another-series");
  EXPECT_EQ(hash, sun::abi::hashBundleContract(description));
  EXPECT_NO_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(
      description, "producer.moon", "x86_64-linux-gnu"));
}

/** Exact imported bytes produce one warning, only after successful validation. */
TEST(ABI_Compatibility, ProducerWarningFollowsValidationAndIsDeduplicated) {
  llvm::LLVMContext context;
  llvm::Module module("warning", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("warning");
  writer.addModule(module, metadata);
  const auto path = bundlePath();
  ASSERT_TRUE(writer.write(path));
  std::ifstream input(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)), {});
  input.close();
  const auto identity = sun::abi::currentProducerIdentity();
  const auto offset = bytes.find(identity.build());
  ASSERT_NE(offset, std::string::npos);
  bytes.replace(offset, identity.build().size(), identity.build().size(), 'x');
  { std::ofstream output(path, std::ios::binary); output.write(bytes.data(), bytes.size()); }
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-musl");
  testing::internal::CaptureStderr();
  EXPECT_THROW(checker.validate(*reader), sun::support::SunError);
  auto output = testing::internal::GetCapturedStderr();
  EXPECT_EQ(output.find("[sun][warning]"), std::string::npos);
  checker.beginCompilation("x86_64-linux-gnu");
  testing::internal::CaptureStderr();
  EXPECT_NO_THROW(checker.validate(*reader));
  EXPECT_NO_THROW(checker.validate(*reader));
  output = testing::internal::GetCapturedStderr();
  const auto warning = output.find("[sun][warning]");
  EXPECT_NE(warning, std::string::npos);
  EXPECT_EQ(output.find("[sun][warning]", warning + 1), std::string::npos);
  EXPECT_NE(output.find("unknown producer ordering"), std::string::npos);
  EXPECT_NE(output.find(identity.build()), std::string::npos);
  EXPECT_NE(output.find("bundle contract verified"), std::string::npos);
  std::filesystem::remove(path);
}

/** A supported feature must still be declared by the bundle that emits it. */
TEST(ABI_Compatibility, PayloadCannotOmitRequiredCPUFeatures) {
  const auto features = llvm::sys::getHostCPUFeatures();
  if (!features.lookup("sse2") || !features.lookup("sse3"))
    GTEST_SKIP() << "This fixture requires an x86 host supporting SSE2 and SSE3";
  llvm::LLVMContext context;
  llvm::Module module("features", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  auto* signature = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  auto* function = llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                                         "feature_user", module);
  function->addFnAttr("target-features", "+sse2");
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", function));
  builder.CreateRetVoid();
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("features");
  writer.addModule(module, metadata);
  const auto path = bundlePath();
  ASSERT_TRUE(writer.write(path));
  auto original = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(original, nullptr);
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  EXPECT_NO_THROW(checker.validate(*original));
  const auto* recorded = &original->getLibrary();
  ASSERT_NE(recorded, nullptr);
  auto changed = *recorded;
  auto* description = changed.mutable_bundle_contract();
  ASSERT_EQ(description->target().cpu_features_size(), 1);
  description->mutable_target()->set_cpu_features(0, "sse3");
  description->set_compatibility_hash(sun::abi::hashBundleContract(*description));
  // Both contracts are supported in isolation; only the payload reveals the omission.
  EXPECT_NO_THROW(checker.validateContract(*description, "features.moon", "x86_64-linux-gnu"));
  const auto before = recorded->SerializeAsString();
  const auto after = changed.SerializeAsString();
  ASSERT_EQ(before.size(), after.size());
  auto bytes = *original->getBytes();
  const auto offset = bytes.find(before);
  ASSERT_NE(offset, std::string::npos);
  bytes.replace(offset, before.size(), after);
  { std::ofstream output(path, std::ios::binary); output.write(bytes.data(), bytes.size()); }
  auto altered = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(altered, nullptr);
  try {
    checker.validate(*altered);
    FAIL() << "Accepted an undeclared payload requirement";
  } catch (const sun::support::SunError& error) {
    EXPECT_NE(std::string(error.what()).find("emitted CPU feature 'sse2' missing"),
              std::string::npos);
    EXPECT_NE(std::string(error.what()).find(description->compatibility_hash()),
              std::string::npos);
  }
  EXPECT_EQ(checker.importedContracts().size(), 1u);
  std::filesystem::remove(path);
}

/** Parseable but invalid bitcode cannot authorize imports or enter the import cache. */
TEST(ABI_Compatibility, InvalidLLVMPayloadIsRejectedBeforeImport) {
  llvm::LLVMContext context;
  llvm::Module module("invalid", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  auto* signature = llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  auto* function = llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                                         "invalid_return", module);
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", function));
  builder.CreateRet(builder.getInt32(1));
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("invalid");
  writer.addModule(module, metadata);
  const auto path = bundlePath();
  ASSERT_TRUE(writer.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  try {
    checker.validate(*reader);
    FAIL() << "Accepted invalid LLVM code";
  } catch (const sun::support::SunError& error) {
    EXPECT_NE(std::string(error.what()).find("invalid LLVM payload"), std::string::npos);
  }
  EXPECT_TRUE(checker.importedContracts().empty());
  std::filesystem::remove(path);
}

/** Bundle metadata ranges cannot be missing or overlap indexed payloads. */
TEST(ABI_Compatibility, MoonLibraryRangeIsValidated) {
  llvm::LLVMContext context;
  llvm::Module module("ranges", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("ranges");
  writer.addModule(module, metadata);
  const auto path = bundlePath();
  ASSERT_TRUE(writer.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  const auto original = *reader->getBytes();
  sun::moon_bundling::MoonHeader header;
  std::memcpy(&header, original.data(), sizeof(header));
  for (int invalidCase : {0, 1, 2}) {
    auto changed = header;
    auto bytes = original;
    if (invalidCase == 0) changed.bundleMetadataSize = 0;
    else if (invalidCase == 1)
      changed.bundleMetadataSize = header.bundleMetadataSize + 1;
    else {
      // A valid unknown optional field cannot substitute for the mandatory contract.
      changed.bundleMetadataSize = 2;
      bytes[changed.bundleMetadataOffset] = 16;
      bytes[changed.bundleMetadataOffset + 1] = 1;
    }
    std::memcpy(bytes.data(), &changed, sizeof(changed));
    { std::ofstream output(path, std::ios::binary); output.write(bytes.data(), bytes.size()); }
    EXPECT_THROW(sun::moon_bundling::MoonReader::open(path), sun::support::SunError);
  }
  std::filesystem::remove(path);
}

/** Library records reject duplicate keys, inconsistent identities, and invalid payload ranges. */
TEST(ABI_Compatibility, LibraryRecordsAreValidatedBeforeImport) {
  llvm::LLVMContext context;
  llvm::Module module("records", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("records");
  writer.addModule(module, metadata);
  const auto path = bundlePath();
  ASSERT_TRUE(writer.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  sun::moon_bundling::MoonHeader header;
  std::memcpy(&header, reader->getBytes()->data(), sizeof(header));
  for (int invalidCase : {0, 1, 2, 3}) {
    auto library = reader->getLibrary();
    if (invalidCase == 0) *library.add_modules() = library.modules(0);
    else if (invalidCase == 1) library.set_content_hash(std::string(64, 'b'));
    else if (invalidCase == 2) library.mutable_modules(0)->set_bitcode_offset(header.bundleMetadataOffset);
    else library.clear_bundle_contract();
    const auto record = library.SerializeAsString();
    auto changed = header;
    changed.bundleMetadataSize = record.size();
    auto bytes = reader->getBytes()->substr(0, header.bundleMetadataOffset) + record;
    std::memcpy(bytes.data(), &changed, sizeof(changed));
    { std::ofstream output(path, std::ios::binary); output.write(bytes.data(), bytes.size()); }
    EXPECT_THROW(sun::moon_bundling::MoonReader::open(path), sun::support::SunError);
  }
  std::filesystem::remove(path);
}
