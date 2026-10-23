/** Exercises ABI contract encoding, support decisions, and Moon integration. */
#include <dlfcn.h>
#include <google/protobuf/unknown_field_set.h>
#include <gtest/gtest.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Host.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "abi/abi_compatibility_checker.h"
#include "abi/abi_validation_scope.h"
#include "abi/boundary_requirements.h"
#include "abi/compatibility.h"
#include "abi/producer_identity.h"
#include "abi/support_inventory.h"
#include "abi/support_manifest.h"
#include "driver/build_record.h"
#include "driver/driver.h"
#include "driver/input_hash.h"
#include "moon_bundling/library_cache.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/moon_builder.h"
#include "support/error.h"

/** Keeps test construction helpers private to this translation unit. */
namespace {
/** Creates a deterministic host contract without relying on source compilation.
 */
sun::abi::BundleContract contract() {
  llvm::LLVMContext context;
  llvm::Module module("contract", context);
  module.setTargetTriple("x86_64-unknown-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  return sun::abi::describeContract(module);
}
/** Uses one temporary bundle path per test process. */
std::filesystem::path bundlePath() {
  std::filesystem::create_directories("tmp");
  return std::filesystem::path("tmp") /
         ("abi_contract_" + std::to_string(getpid()) + ".moon");
}
}  // namespace

/** Application libraries loaded before import are not compiler dependencies. */
TEST(ABI_Compatibility, ApplicationLibrariesDoNotChangeImplementation) {
  void* library =
      dlopen(SUN_FFI_TESTLIB_DIR "/libsun_ffi_testlib.so", RTLD_NOW);
  ASSERT_NE(library, nullptr) << dlerror();
  auto reader = sun::moon_bundling::MoonReader::open("build/stdlib.moon");
  ASSERT_NE(reader, nullptr);
  EXPECT_NO_THROW(sun::abi::AbiCompatibilityChecker::current().validate(
      *reader, "x86_64-linux-gnu"));
  dlclose(library);
}

/** Runtime provenance differences warn once after the contract is validated. */
TEST(ABI_Compatibility, RuntimeDifferencesWarnWithoutRejectingValidBundles) {
  llvm::LLVMContext context;
  llvm::Module module("runtime_warning", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("runtime_warning");
  writer.addModule(module, metadata);
  const auto path = bundlePath();
  ASSERT_TRUE(writer.write(path));
  std::ifstream input(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)), {});
  input.close();
  const auto runtime =
      sun::abi::currentProducerIdentity().runtime_fingerprint();
  ASSERT_FALSE(runtime.empty());
  const auto offset = bytes.find(runtime);
  ASSERT_NE(offset, std::string::npos);
  bytes.replace(offset, runtime.size(), runtime.size(), 'x');
  {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), bytes.size());
  }
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
  const auto warning = output.find("different runtime libraries");
  ASSERT_NE(warning, std::string::npos);
  EXPECT_EQ(output.find("different runtime libraries", warning + 1),
            std::string::npos);
  EXPECT_NE(output.find("bundle contract verified"), std::string::npos);
  std::filesystem::remove(path);
}

/** A runtime difference cannot excuse an unsupported implementation
 * requirement. */
TEST(ABI_Compatibility,
     RuntimeDifferencesDoNotAuthorizeUnknownImplementations) {
  auto unsupported = contract();
  unsupported.mutable_producer()->set_runtime_fingerprint("different runtime");
  unsupported.mutable_requirements(0)->set_implementation("unsupported");
  unsupported.set_compatibility_hash(sun::abi::hashBundleContract(unsupported));
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  EXPECT_THROW(
      checker.validateContract(unsupported, "runtime-test", "x86_64-linux-gnu"),
      sun::support::SunError);
}

/** Set traversal and diagnostic metadata must not change compiled requirements.
 */
TEST(ABI_Compatibility, CanonicalSetsExcludeProducerMetadata) {
  auto original = contract();
  original.add_boundary_records("second");
  original.add_boundary_records("first");
  auto reordered = original;
  reordered.mutable_boundary_records()->SwapElements(0, 1);
  reordered.mutable_requirements()->SwapElements(0, 1);
  reordered.mutable_producer()->set_build("future compiler");
  reordered.mutable_producer()->set_release_order(999);
  reordered.mutable_producer()->set_runtime_fingerprint("different runtime");
  EXPECT_EQ(sun::abi::encodeBundleContract(original),
            sun::abi::encodeBundleContract(reordered));
  reordered.add_boundary_records("new boundary");
  EXPECT_NE(sun::abi::hashBundleContract(original),
            sun::abi::hashBundleContract(reordered));
}

/** Recomputing a hash cannot authorize an unknown semantic requirement. */
TEST(ABI_Compatibility, UnknownRequiredRuleIsRejected) {
  auto unsupported = contract();
  auto* rule = unsupported.add_requirements();
  rule->set_name("unknown.required.behavior");
  rule->set_implementation("1");
  unsupported.set_compatibility_hash(sun::abi::hashBundleContract(unsupported));
  EXPECT_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(
                   unsupported, "unknown.moon", "x86_64-linux-gnu"),
               sun::support::SunError);
}

/** A supplied implementation fingerprint must be independently supported. */
TEST(ABI_Compatibility, ChangedFingerprintIsRejected) {
  auto unsupported = contract();
  unsupported.mutable_requirements(1)->set_implementation(std::string(64, '0'));
  unsupported.set_compatibility_hash(sun::abi::hashBundleContract(unsupported));
  EXPECT_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(
                   unsupported, "changed.moon", "x86_64-linux-gnu"),
               sun::support::SunError);
}

/** Required rules cannot disappear merely because the supplied hash is
 * consistent. */
TEST(ABI_Compatibility, MissingRuleIsRejected) {
  auto unsupported = contract();
  unsupported.mutable_requirements()->RemoveLast();
  unsupported.set_compatibility_hash(sun::abi::hashBundleContract(unsupported));
  EXPECT_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(
                   unsupported, "missing.moon", "x86_64-linux-gnu"),
               sun::support::SunError);
}

/** Unknown optional protobuf fields have no effect on the canonical contract.
 */
TEST(ABI_Compatibility, OptionalExtensionsAreSkippable) {
  auto supported = contract();
  const auto hash = supported.compatibility_hash();
  supported.GetReflection()
      ->MutableUnknownFields(&supported)
      ->AddLengthDelimited(1000, "optional diagnostics");
  EXPECT_EQ(hash, sun::abi::hashBundleContract(supported));
  EXPECT_NO_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(
      supported, "optional.moon", "x86_64-linux-gnu"));
}

/** Transitive requirements are deduplicated and conflicting implementations
 * rejected. */
TEST(ABI_Compatibility, InheritanceRetainsAndChecksRequirements) {
  auto outer = contract();
  auto inner = contract();
  llvm::LLVMContext context;
  llvm::Module module("inherited", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(inner.target().data_layout());
  llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false),
      llvm::Function::ExternalLinkage, "inherited_function", module);
  inner = sun::abi::describeContract(module);
  sun::abi::mergeContractRequirements(outer, inner);
  EXPECT_EQ(outer.requirements_size(), inner.requirements_size());
  EXPECT_EQ(outer.boundary_records(0), inner.boundary_records(0));
  inner.mutable_requirements(0)->set_implementation("unsupported replacement");
  EXPECT_THROW(sun::abi::mergeContractRequirements(outer, inner),
               sun::support::SunError);
}

/** Equivalent triples match, while different platform ABIs cannot authorize
 * imports. */
TEST(ABI_Compatibility, TargetProfilesDistinguishPlatformABI) {
  const auto supported = contract();
  EXPECT_NO_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(
      supported, "target.moon", "x86_64-linux-gnu"));
  EXPECT_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(
                   supported, "target.moon", "x86_64-linux-musl"),
               sun::support::SunError);
}

/** Bundle metadata owns the contract and shared code regions validate once. */
TEST(ABI_Compatibility, MultiModuleRoundTripHasOneContract) {
  sun::abi::AbiCompatibilityChecker::current().beginCompilation(
      "x86_64-linux-gnu");
  llvm::LLVMContext context;
  llvm::Module module("roundtrip", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
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
  EXPECT_EQ(sun::moon::ModuleMetadata::descriptor()->FindFieldByName(
                "bundle_contract"),
            nullptr);
  EXPECT_NO_THROW(
      sun::abi::AbiCompatibilityChecker::current().validate(*reader));
  EXPECT_EQ(reader->getBitcodeId("first"), reader->getBitcodeId("second"));
  std::filesystem::remove(path);
}

/** Truncated headers are rejected before any index counts or sizes are used. */
TEST(ABI_Compatibility, TruncatedContainerIsRejected) {
  const auto path = bundlePath();
  {
    std::ofstream output(path, std::ios::binary);
    output << "SUN";
  }
  EXPECT_THROW(sun::moon_bundling::MoonReader::open(path),
               sun::support::SunError);
  std::filesystem::remove(path);
}

/** Byte-keyed authorization survives path replacement without reusing old
 * contracts. */
TEST(ABI_Compatibility, CheckerOwnsAuthorizationAndTracksImmutableSnapshots) {
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  llvm::LLVMContext context;
  llvm::Module module("snapshots", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  auto* signature =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context), false);
  auto* function = llvm::Function::Create(
      signature, llvm::Function::ExternalLinkage, "answer", module);
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
  EXPECT_EQ(checker.validate(*first).contract().compatibility_hash(),
            firstContract);

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
  EXPECT_NE(checker.validate(*second).contract().compatibility_hash(),
            firstContract);
  EXPECT_EQ(checker.bytesHash(*first), firstDigest);
  EXPECT_EQ(checker.importedContracts().size(), 1u);

  auto& cache = sun::moon_bundling::LibraryCache::instance();
  cache.clear();
  cache.setTargetTriple("x86_64-linux-gnu");
  const auto moduleKey = first->listModules().front();
  cache.addBundle(std::move(first));
  auto originalModule = cache.loadModule(moduleKey, context);
  ASSERT_NE(originalModule, nullptr);
  EXPECT_EQ(originalModule->getFunction("answer")->getCallingConv(),
            llvm::CallingConv::C);
  cache.addBundle(path);
  sun::abi::AbiCompatibilityChecker::current().beginCompilation(
      "x86_64-linux-gnu");
  auto replacedModule = cache.loadModule(moduleKey, context);
  ASSERT_NE(replacedModule, nullptr);
  EXPECT_EQ(replacedModule->getFunction("answer")->getCallingConv(),
            llvm::CallingConv::Fast);
  cache.clear();
  checker.beginCompilation("x86_64-linux-musl");
  EXPECT_TRUE(checker.importedContracts().empty());
  EXPECT_EQ(authorization.contract().compatibility_hash(), firstContract);
  EXPECT_THROW(checker.validate(*second), sun::support::SunError);
  std::filesystem::remove(path);
}

/** Release ordering requires complete provenance and never compares version
 * strings. */
TEST(ABI_Compatibility, ProducerOrderingRequiresComparableProvenance) {
  using sun::abi::compareProducerIdentity;
  using sun::abi::ProducerOrdering;
  sun::abi::ProducerIdentity consumer;
  consumer.set_build("consumer");
  consumer.set_release_series("official");
  consumer.set_release_order(20);
  auto producer = consumer;
  EXPECT_EQ(compareProducerIdentity(producer, consumer),
            ProducerOrdering::SameBuild);
  producer.set_build("producer");
  EXPECT_EQ(compareProducerIdentity(producer, consumer),
            ProducerOrdering::Unknown);
  producer.set_release_order(19);
  EXPECT_EQ(compareProducerIdentity(producer, consumer),
            ProducerOrdering::Older);
  producer.set_release_order(21);
  EXPECT_EQ(compareProducerIdentity(producer, consumer),
            ProducerOrdering::Newer);
  producer.set_release_series("another-series");
  EXPECT_EQ(compareProducerIdentity(producer, consumer),
            ProducerOrdering::Unknown);
  producer.set_release_series("official");
  producer.clear_release_order();
  EXPECT_EQ(compareProducerIdentity(producer, consumer),
            ProducerOrdering::Unknown);
  producer.set_release_order(19);
  consumer.clear_release_order();
  EXPECT_EQ(compareProducerIdentity(producer, consumer),
            ProducerOrdering::Unknown);
  consumer.set_release_order(20);
  producer.clear_build();
  EXPECT_EQ(compareProducerIdentity(producer, consumer),
            ProducerOrdering::Unknown);
  consumer.clear_build();
  EXPECT_EQ(compareProducerIdentity(producer, consumer),
            ProducerOrdering::Unknown);
}

/** Bundles record the generated producer without making it part of the
 * contract. */
TEST(ABI_Compatibility, GeneratedProducerIsDiagnosticOnly) {
  const auto identity = sun::abi::currentProducerIdentity();
  auto description = contract();
  EXPECT_EQ(description.producer().SerializeAsString(),
            identity.SerializeAsString());
  const auto hash = description.compatibility_hash();
  description.mutable_producer()->set_build("different-build");
  description.mutable_producer()->set_release_order(1);
  description.mutable_producer()->set_release_series("another-series");
  EXPECT_EQ(hash, sun::abi::hashBundleContract(description));
  EXPECT_NO_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(
      description, "producer.moon", "x86_64-linux-gnu"));
}

/** Exact imported bytes produce one warning, only after successful validation.
 */
TEST(ABI_Compatibility, ProducerWarningFollowsValidationAndIsDeduplicated) {
  llvm::LLVMContext context;
  llvm::Module module("warning", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
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
  {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), bytes.size());
  }
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
  // Preflight and driver imports belong to one compilation, even when a
  // nested dependency build runs between them.
  testing::internal::CaptureStderr();
  {
    sun::abi::AbiValidationScope scope(
        {"x86_64-linux-gnu", sun::abi::NativeExecution::NativeLink});
    sun::driver::BuildInputs inputs;
    inputs.targetTriple = "x86_64-linux-gnu";
    inputs.moonImports.push_back({path.string(), {}});
    inputs.settings.emplace_back("sysroot", "/target");
    EXPECT_NO_THROW(sun::driver::computeInputHash(inputs));
    auto* parent = &sun::abi::AbiCompatibilityChecker::current();
    EXPECT_EQ(parent->context().linkOptions, inputs.settings);
    EXPECT_EQ(parent->importedContracts().size(), 1);
    inputs.debugInfo = true;
    EXPECT_NO_THROW(sun::driver::computeInputHash(inputs));
    inputs.debugInfo = false;
    {
      sun::abi::AbiValidationScope nested(
          {"aarch64-linux-gnu", sun::abi::NativeExecution::NativeLink});
      EXPECT_NE(&sun::abi::AbiCompatibilityChecker::current(), parent);
      EXPECT_TRUE(sun::abi::AbiCompatibilityChecker::current()
                      .importedContracts()
                      .empty());
    }
    EXPECT_EQ(&sun::abi::AbiCompatibilityChecker::current(), parent);
    auto driver =
        sun::driver::Driver::createForAOT("scope", inputs.targetTriple);
    EXPECT_EQ(parent->importedContracts().size(), 1);
    EXPECT_NO_THROW(parent->validate(*reader));
    EXPECT_NO_THROW(sun::driver::readMoonInputHash(path.string()));
  }
  output = testing::internal::GetCapturedStderr();
  const auto scopedWarning = output.find("[sun][warning]");
  EXPECT_NE(scopedWarning, std::string::npos);
  EXPECT_EQ(output.find("[sun][warning]", scopedWarning + 1),
            std::string::npos);
  std::filesystem::remove(path);
}

/** A supported feature must still be declared by the bundle that emits it. */
TEST(ABI_Compatibility, PayloadCannotOmitRequiredCPUFeatures) {
  const auto features = llvm::sys::getHostCPUFeatures();
  if (!features.lookup("sse2") || !features.lookup("sse3")) {
    GTEST_SKIP()
        << "This fixture requires an x86 host supporting SSE2 and SSE3";
  }
  llvm::LLVMContext context;
  llvm::Module module("features", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  auto* signature =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  auto* function = llvm::Function::Create(
      signature, llvm::Function::ExternalLinkage, "feature_user", module);
  function->addFnAttr("target-features", "+sse2");
  llvm::IRBuilder<> builder(
      llvm::BasicBlock::Create(context, "entry", function));
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
  description->set_compatibility_hash(
      sun::abi::hashBundleContract(*description));
  // Both contracts are supported in isolation; only the payload reveals the
  // omission.
  EXPECT_NO_THROW(checker.validateContract(*description, "features.moon",
                                           "x86_64-linux-gnu"));
  const auto before = recorded->SerializeAsString();
  const auto after = changed.SerializeAsString();
  ASSERT_EQ(before.size(), after.size());
  auto bytes = *original->getBytes();
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
    FAIL() << "Accepted an undeclared payload requirement";
  } catch (const sun::support::SunError& error) {
    EXPECT_NE(
        std::string(error.what()).find("emitted CPU feature 'sse2' missing"),
        std::string::npos);
    EXPECT_NE(std::string(error.what()).find(description->compatibility_hash()),
              std::string::npos);
  }
  EXPECT_EQ(checker.importedContracts().size(), 1u);
  std::filesystem::remove(path);
}

/** Parseable but invalid bitcode cannot authorize imports or enter the import
 * cache. */
TEST(ABI_Compatibility, InvalidLLVMPayloadIsRejectedBeforeImport) {
  llvm::LLVMContext context;
  llvm::Module module("invalid", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  auto* signature =
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);
  auto* function = llvm::Function::Create(
      signature, llvm::Function::ExternalLinkage, "invalid_return", module);
  llvm::IRBuilder<> builder(
      llvm::BasicBlock::Create(context, "entry", function));
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
    EXPECT_NE(std::string(error.what()).find("invalid LLVM payload"),
              std::string::npos);
  }
  EXPECT_TRUE(checker.importedContracts().empty());
  std::filesystem::remove(path);
}

/** Bundle metadata ranges cannot be missing or overlap indexed payloads. */
TEST(ABI_Compatibility, MoonLibraryRangeIsValidated) {
  llvm::LLVMContext context;
  llvm::Module module("ranges", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
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
    if (invalidCase == 0) {
      changed.bundleMetadataSize = 0;
    } else if (invalidCase == 1) {
      changed.bundleMetadataSize = header.bundleMetadataSize + 1;
    } else {
      // A valid unknown optional field cannot substitute for the mandatory
      // contract.
      changed.bundleMetadataSize = 2;
      bytes[changed.bundleMetadataOffset] = 16;
      bytes[changed.bundleMetadataOffset + 1] = 1;
    }
    std::memcpy(bytes.data(), &changed, sizeof(changed));
    {
      std::ofstream output(path, std::ios::binary);
      output.write(bytes.data(), bytes.size());
    }
    EXPECT_THROW(sun::moon_bundling::MoonReader::open(path),
                 sun::support::SunError);
  }
  std::filesystem::remove(path);
}

/** Library records reject duplicate keys, inconsistent identities, and invalid
 * payload ranges. */
TEST(ABI_Compatibility, LibraryRecordsAreValidatedBeforeImport) {
  llvm::LLVMContext context;
  llvm::Module module("records", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
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
    if (invalidCase == 0) {
      *library.add_modules() = library.modules(0);
    } else if (invalidCase == 1) {
      library.set_content_hash(std::string(64, 'b'));
    } else if (invalidCase == 2) {
      library.mutable_modules(0)->set_bitcode_offset(
          header.bundleMetadataOffset);
    } else {
      library.clear_bundle_contract();
    }
    const auto record = library.SerializeAsString();
    auto changed = header;
    changed.bundleMetadataSize = record.size();
    auto bytes =
        reader->getBytes()->substr(0, header.bundleMetadataOffset) + record;
    std::memcpy(bytes.data(), &changed, sizeof(changed));
    {
      std::ofstream output(path, std::ios::binary);
      output.write(bytes.data(), bytes.size());
    }
    EXPECT_THROW(sun::moon_bundling::MoonReader::open(path),
                 sun::support::SunError);
  }
  std::filesystem::remove(path);
}

/** Embedded support resolves each advertised implementation and emission
 * default. */
TEST(ABI_Compatibility, EmbeddedCompilerSupportResolvesImplementations) {
  const auto& support = sun::abi::compilerAbiSupport();
  EXPECT_NO_THROW(sun::abi::validateCompilerSupport(support));
  for (const auto& rule : support.rules()) {
    EXPECT_EQ(sun::abi::supportedRules().at(rule.name()),
              sun::abi::emissionImplementation(rule.name()));
  }
  const auto emitted = contract();
  for (const auto& rule : emitted.requirements()) {
    EXPECT_EQ(rule.implementation(),
              sun::abi::emissionImplementation(rule.name()));
    EXPECT_NE(rule.name(), "native.elf.x86_64.declared");
    EXPECT_NE(rule.name(), "native.runtime.atexit");
  }
  EXPECT_THROW(sun::abi::emissionImplementation("unknown"), std::exception);
}

/** Configuration cannot advertise absent code or conflicting rule versions. */
TEST(ABI_Compatibility, CompilerSupportRejectsUnavailableBindings) {
  auto support = sun::abi::compilerAbiSupport();
  support.mutable_rules(0)->set_binding("unimplemented");
  EXPECT_THROW(sun::abi::validateCompilerSupport(support), std::exception);
  support = sun::abi::compilerAbiSupport();
  support.mutable_rules(0)->set_version("future");
  EXPECT_THROW(sun::abi::validateCompilerSupport(support), std::exception);
  support = sun::abi::compilerAbiSupport();
  *support.add_rules() = support.rules(0);
  EXPECT_THROW(sun::abi::validateCompilerSupport(support), std::exception);
  support = sun::abi::compilerAbiSupport();
  support.mutable_rules()->RemoveLast();
  EXPECT_THROW(sun::abi::validateCompilerSupport(support), std::exception);
}

/** Missing, duplicate, or unsupported defaults cannot alter emitted
 * requirements. */
TEST(ABI_Compatibility, CompilerSupportRejectsInvalidDefaults) {
  auto support = sun::abi::compilerAbiSupport();
  support.mutable_emission_defaults(0)->set_version("future");
  EXPECT_THROW(sun::abi::validateCompilerSupport(support), std::exception);
  support = sun::abi::compilerAbiSupport();
  *support.add_emission_defaults() = support.emission_defaults(0);
  EXPECT_THROW(sun::abi::validateCompilerSupport(support), std::exception);
  support = sun::abi::compilerAbiSupport();
  support.mutable_emission_defaults()->RemoveLast();
  EXPECT_THROW(sun::abi::validateCompilerSupport(support), std::exception);
  support = sun::abi::compilerAbiSupport();
  support.set_schema(0);
  EXPECT_THROW(sun::abi::validateCompilerSupport(support), std::exception);
}

/** Native declarations remain subject to the consumer's CPU support checks. */
TEST(ABI_Compatibility, DeclaredNativeCPURequirementsMustBeSupported) {
  auto declared = contract();
  auto* native = declared.add_requirements();
  native->set_name("native.elf.x86_64.declared");
  native->set_implementation(sun::abi::emissionImplementation(native->name()));
  declared.mutable_target()->add_cpu_features(
      "sun-test-unavailable-cpu-feature");
  declared.set_compatibility_hash(sun::abi::hashBundleContract(declared));
  EXPECT_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(
                   declared, "native.moon", "x86_64-linux-gnu"),
               sun::support::SunError);
}

/** Declared native trust does not silently reinterpret the old audited rule. */
TEST(ABI_Compatibility, AuditedNativeRuleIsNotDeclaredNativeTrust) {
  auto audited = contract();
  auto* native = audited.add_requirements();
  native->set_name("native.elf.x86_64.baseline");
  native->set_implementation(sun::abi::compatibilityImplementation());
  audited.set_compatibility_hash(sun::abi::hashBundleContract(audited));
  EXPECT_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(
                   audited, "native.moon", "x86_64-linux-gnu"),
               sun::support::SunError);
}

/** Native symbol diagnostics survive merging without affecting compatibility.
 */
TEST(ABI_Compatibility, NativeSymbolDiagnosticsAreInherited) {
  auto first = contract();
  first.add_native_link_symbols("malloc");
  first.add_native_link_symbols("free");
  auto reordered = first;
  reordered.mutable_native_link_symbols()->SwapElements(0, 1);
  reordered.add_native_link_symbols("malloc");
  EXPECT_EQ(sun::abi::hashBundleContract(first),
            sun::abi::hashBundleContract(reordered));
  auto inherited = contract();
  inherited.add_native_link_symbols("__errno_location");
  sun::abi::mergeContractRequirements(first, inherited);
  EXPECT_EQ(first.native_link_symbols_size(), 3);
  EXPECT_EQ(sun::abi::hashBundleContract(first),
            sun::abi::hashBundleContract(reordered));
}

/** Diagnostic symbol names do not impose additional compatibility requirements.
 */
TEST(ABI_Compatibility, NativeSymbolDiagnosticsDoNotRequireLinkerRule) {
  auto native = contract();
  const auto hash = native.compatibility_hash();
  native.add_native_link_symbols("__errno_location");
  EXPECT_EQ(sun::abi::hashBundleContract(native), hash);
  EXPECT_NO_THROW(sun::abi::AbiCompatibilityChecker::current().validateContract(
      native, "native.moon", "x86_64-linux-gnu"));
}

/** Cross compilation accepts baseline features of the selected target. */
TEST(ABI_Compatibility, NativeLinkChecksTargetCPUInsteadOfBuildHost) {
  for (const auto& [triple, feature] :
       {std::pair{"aarch64-linux-gnu", "neon"},
        std::pair{"x86_64-linux-gnu", "sse2"}}) {
    llvm::LLVMContext context;
    llvm::Module module("cross", context);
    module.setTargetTriple(triple);
    module.setDataLayout(sun::abi::compatibilityTarget(triple).data_layout());
    auto declared = sun::abi::describeContract(module);
    declared.mutable_target()->add_cpu_features(feature);
    declared.set_compatibility_hash(sun::abi::hashBundleContract(declared));
    sun::abi::AbiCompatibilityChecker checker(
        triple, sun::abi::NativeExecution::NativeLink);
    EXPECT_NO_THROW(checker.validateContract(declared, "cross.moon", triple));
    declared.mutable_target()->add_cpu_features("sun-unknown-feature");
    declared.set_compatibility_hash(sun::abi::hashBundleContract(declared));
    EXPECT_THROW(checker.validateContract(declared, "cross.moon", triple),
                 sun::support::SunError);
  }
}

/** Options and runtime changes discard previous authorizations. */
TEST(ABI_Compatibility, CompilationSettingsInvalidateAuthorizations) {
  llvm::LLVMContext llvmContext;
  llvm::Module module("scope", llvmContext);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  sun::moon_bundling::MoonWriter writer(std::string(64, 'b'));
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("scope");
  writer.addModule(module, metadata);
  const auto path = bundlePath();
  ASSERT_TRUE(writer.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  sun::abi::CompilationContext context{"x86_64-linux-gnu",
                                       sun::abi::NativeExecution::NativeLink};
  sun::abi::AbiCompatibilityChecker checker(context);
  auto accepted = checker.validate(*reader);
  context.linkOptions.emplace_back("sysroot", "/different");
  checker.configure(context);
  EXPECT_TRUE(checker.importedContracts().empty());
  EXPECT_NO_THROW(checker.validate(*reader));
  checker.configure(context);
  EXPECT_EQ(checker.importedContracts().size(), 1);
  context.optimize = false;
  checker.configure(context);
  EXPECT_TRUE(checker.importedContracts().empty());
  EXPECT_FALSE(accepted.contract().compatibility_hash().empty());
  std::filesystem::remove(path);
}

/** Rehashed external references must resolve before preflight or AST import. */
TEST(ABI_Compatibility, CompleteImportSetRejectsMissingExternalReferences) {
  llvm::LLVMContext context;
  llvm::Module module("external", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  const auto consumerPath = bundlePath();
  const std::filesystem::path dependencyPath =
      consumerPath.string() + ".dependency";
  const std::string key = "$" + std::string(64, 'd') + "$_type";
  sun::moon::ModuleMetadata consumer;
  consumer.set_source_hash("consumer");
  consumer.add_globals()->mutable_type_annotation()->set_declaration_key(key);
  sun::moon_bundling::MoonWriter consumerWriter(std::string(64, 'c'));
  consumerWriter.addModule(module, consumer);
  ASSERT_TRUE(consumerWriter.write(consumerPath));
  sun::moon::ModuleMetadata dependency;
  dependency.set_source_hash("dependency");
  auto* declaration = dependency.add_declarations();
  declaration->set_key(key);
  declaration->set_kind(sun::moon::CLASS);
  sun::moon_bundling::MoonWriter dependencyWriter(std::string(64, 'd'));
  dependencyWriter.addModule(module, dependency);
  ASSERT_TRUE(dependencyWriter.write(dependencyPath));
  auto reader = sun::moon_bundling::MoonReader::open(consumerPath);
  auto owner = sun::moon_bundling::MoonReader::open(dependencyPath);
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(owner, nullptr);
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  EXPECT_THROW(checker.validateImports({reader.get()}), sun::support::SunError);
  EXPECT_TRUE(checker.importedContracts().empty());
  EXPECT_NO_THROW(checker.validateImports({reader.get(), owner.get()}));
  checker.beginCompilation("x86_64-linux-gnu");
  EXPECT_NO_THROW(
      checker.validateImports({owner.get(), reader.get(), owner.get()}));
  sun::driver::BuildInputs inputs;
  inputs.targetTriple = "x86_64-linux-gnu";
  inputs.moonImports.emplace_back(consumerPath.string());
  EXPECT_THROW(sun::driver::computeInputHash(inputs), sun::support::SunError);
  inputs.moonImports.emplace_back(dependencyPath.string());
  EXPECT_NO_THROW(sun::driver::computeInputHash(inputs));
  std::filesystem::remove(consumerPath);
  std::filesystem::remove(dependencyPath);
}

/** Library identity is authenticated contract data, not an inherited rule. */
TEST(ABI_Compatibility, LibraryIdentityIsHashedValidatedAndNotInherited) {
  auto original = contract();
  auto identified = original;
  identified.set_library_id("org.example/shapes");
  identified.set_compatibility_hash(sun::abi::hashBundleContract(identified));
  EXPECT_NE(original.compatibility_hash(), identified.compatibility_hash());
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  EXPECT_NO_THROW(
      checker.validateContract(identified, "shapes", "x86_64-linux-gnu"));
  auto tampered = identified;
  tampered.set_library_id("other/shapes");
  EXPECT_THROW(checker.validateContract(tampered, "shapes", "x86_64-linux-gnu"),
               sun::support::SunError);
  for (const auto& invalid :
       {std::string(), std::string(513, 'a'), std::string("\xff")}) {
    auto malformed = identified;
    malformed.set_library_id(invalid);
    malformed.set_compatibility_hash(sun::abi::hashBundleContract(malformed));
    EXPECT_THROW(
        checker.validateContract(malformed, "shapes", "x86_64-linux-gnu"),
        sun::support::SunError);
  }
  sun::abi::mergeContractRequirements(original, identified);
  EXPECT_FALSE(original.has_library_id());
  auto consumer = original;
  consumer.set_library_id("org.example/consumer");
  sun::abi::mergeContractRequirements(consumer, identified);
  EXPECT_EQ(consumer.library_id(), "org.example/consumer");
}

/** Moon serialization preserves arbitrary identity bytes through validation. */
TEST(ABI_Compatibility, LibraryIdentitySurvivesBundleRoundTrip) {
  llvm::LLVMContext context;
  llvm::Module module("identity", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("identity");
  sun::moon_bundling::MoonWriter writer(std::string(64, 'a'));
  writer.addModule(module, metadata);
  const std::string identity("library\0名前", 14);
  writer.setLibraryId(identity);
  const auto path = bundlePath();
  ASSERT_TRUE(writer.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  EXPECT_EQ(checker.validate(*reader).contract().library_id(), identity);
  std::filesystem::remove(path);
}

/** Packaging uses the entrypoint ID and rebuilds when that identity changes. */
TEST(ABI_Compatibility, PackagingPreservesEntrypointLibraryIdentity) {
  const auto directory = std::filesystem::path("tmp") /
                         ("abi_library_identity_" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto source = directory / "library.sun";
  const auto output = directory / "library.moon";
  /** Writes an explicit identity with otherwise identical library code. */
  const auto writeSource = [&](const std::string& identity) {
    std::ofstream file(source);
    file << "manifest { library_id: \"" << identity << "\" }\n"
         << "/** Exports a value for identity packaging tests. */\n"
         << "public module identity_fixture {\n"
         << "/** Returns a constant. */\n"
         << "public function value() i32 { return 42; } }\n";
  };
  writeSource("org.example/shapes");
  ASSERT_NO_THROW(
      sun::moon_bundling::MoonBuilder::build(source.string(), output));
  auto reader = sun::moon_bundling::MoonReader::open(output);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->getLibrary().bundle_contract().library_id(),
            "org.example/shapes");
  const auto firstHash = reader->getLibrary().content_hash();
  EXPECT_TRUE(
      sun::moon_bundling::MoonBuilder::build(source.string(), output).upToDate);
  writeSource("org.example/other");
  EXPECT_FALSE(
      sun::moon_bundling::MoonBuilder::build(source.string(), output).upToDate);
  reader = sun::moon_bundling::MoonReader::open(output);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->getLibrary().bundle_contract().library_id(),
            "org.example/other");
  EXPECT_NE(reader->getLibrary().content_hash(), firstHash);
  std::filesystem::remove_all(directory);
}
