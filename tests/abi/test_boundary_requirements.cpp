/** Exercises boundary conflicts, framing and dependency accumulation. */
#include <gtest/gtest.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "abi/abi_compatibility_checker.h"
#include "abi/boundary_requirements.h"
#include "abi/cpu_requirements.h"
#include "abi/lowering_descriptors.h"
#include "moon_bundling/moon.h"
#include "support/error.h"

/** Builds small generated contracts for independent boundary scenarios. */
namespace {
/** Describes a named function with a selected calling convention. */
sun::abi::BundleContract functionContract(llvm::CallingConv::ID convention) {
  llvm::LLVMContext context;
  llvm::Module module("signature", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  auto* function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context), false),
      llvm::Function::ExternalLinkage, "shared_function", module);
  function->setCallingConv(convention);
  return sun::abi::describeContract(module);
}

/** Describes a named layout used by a private global to keep it in the module.
 */
sun::abi::BundleContract layoutContract(bool packed) {
  llvm::LLVMContext context;
  llvm::Module module("layout", context);
  module.setTargetTriple("x86_64-linux-gnu");
  module.setDataLayout(
      sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
  auto* structure = llvm::StructType::create(
      context,
      {llvm::Type::getInt8Ty(context), llvm::Type::getInt64Ty(context)},
      "shared_layout", packed);
  new llvm::GlobalVariable(
      module, structure, false, llvm::GlobalValue::PrivateLinkage,
      llvm::Constant::getNullValue(structure), "private_value");
  return sun::abi::describeContract(module);
}
}  // namespace

/** A diamond dependency retains one copy of each requirement. */
TEST(ABI_Compatibility, BoundaryMergesDeduplicateDiamondDependencies) {
  auto outer = functionContract(llvm::CallingConv::C);
  auto left = outer;
  auto right = outer;
  const auto dependency = layoutContract(false);
  sun::abi::mergeContractRequirements(left, dependency);
  sun::abi::mergeContractRequirements(right, dependency);
  sun::abi::mergeContractRequirements(outer, left);
  sun::abi::mergeContractRequirements(outer, right);
  EXPECT_EQ(outer.boundary_records_size(), left.boundary_records_size());
  EXPECT_EQ(sun::abi::hashBundleContract(outer),
            sun::abi::hashBundleContract(left));
}

/** Conflicting signatures and layouts reject merges without changing the
 * destination. */
TEST(ABI_Compatibility, ConflictingNamedBoundariesAreRejectedAtomically) {
  auto signature = functionContract(llvm::CallingConv::C);
  const auto beforeSignature = signature.SerializeAsString();
  EXPECT_THROW(sun::abi::mergeContractRequirements(
                   signature, functionContract(llvm::CallingConv::Fast)),
               sun::support::SunError);
  EXPECT_EQ(beforeSignature, signature.SerializeAsString());
  auto layout = layoutContract(false);
  const auto beforeLayout = layout.SerializeAsString();
  EXPECT_THROW(
      sun::abi::mergeContractRequirements(layout, layoutContract(true)),
      sun::support::SunError);
  EXPECT_EQ(beforeLayout, layout.SerializeAsString());
}

/** A matching supplied hash cannot authorize contradictory named requirements.
 */
TEST(ABI_Compatibility, ConflictingBoundaryMetadataCannotAuthorizeImport) {
  auto description = functionContract(llvm::CallingConv::C);
  const auto conflicting = functionContract(llvm::CallingConv::Fast);
  for (const auto& record : conflicting.boundary_records()) {
    description.add_boundary_records(record);
  }
  description.set_compatibility_hash(sun::abi::hashBundleContract(description));
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  try {
    checker.validateContract(description, "conflicting.moon",
                             "x86_64-linux-gnu");
    FAIL() << "Accepted contradictory function signatures";
  } catch (const sun::support::SunError& error) {
    const std::string message(error.what());
    EXPECT_NE(message.find("shared_function"), std::string::npos);
    EXPECT_NE(message.find("conflicting.moon"), std::string::npos);
    EXPECT_NE(message.find(description.compatibility_hash()),
              std::string::npos);
  }
}

/** Malformed required fields fail bounded decoding even with a fresh contract
 * hash. */
TEST(ABI_Compatibility, BoundaryFramesRejectTruncationAndUnknownKinds) {
  auto description = functionContract(llvm::CallingConv::C);
  ASSERT_EQ(description.boundary_records_size(), 1);
  const auto record = description.boundary_records(0);
  for (size_t size = 0; size < record.size(); ++size) {
    sun::abi::BoundaryRequirements boundaries;
    EXPECT_THROW(boundaries.add(record.substr(0, size)),
                 sun::support::SunError);
  }
  auto unknown = record;
  unknown[0] = 99;
  sun::abi::BoundaryRequirements boundaries;
  EXPECT_THROW(boundaries.add(unknown), sun::support::SunError);
  auto oversized = record;
  for (size_t i = 8; i < 16; ++i) {
    oversized[i] = static_cast<char>(0xff);
  }
  EXPECT_THROW(boundaries.add(oversized), sun::support::SunError);
  EXPECT_NO_THROW(boundaries.add(record));
  EXPECT_NO_THROW(boundaries.add(record));
  EXPECT_EQ(boundaries.records().size(), 1u);
  description.set_boundary_records(0, unknown);
  description.set_compatibility_hash(sun::abi::hashBundleContract(description));
  sun::abi::AbiCompatibilityChecker checker("x86_64-linux-gnu");
  EXPECT_THROW(
      checker.validateContract(description, "unknown.moon", "x86_64-linux-gnu"),
      sun::support::SunError);
}

/** LLVM aggregate naming changes do not create false machine-signature
 * conflicts. */
TEST(ABI_Compatibility, NamedAndLiteralAggregateSignaturesAreEquivalent) {
  /** Generates equivalent signatures with different LLVM aggregate spellings.
   */
  auto describe = [](bool named) {
    llvm::LLVMContext context;
    llvm::Module module("aggregate", context);
    module.setTargetTriple("x86_64-linux-gnu");
    module.setDataLayout(
        sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
    auto* pointer = llvm::PointerType::getUnqual(context);
    std::vector<llvm::Type*> members{pointer, pointer};
    auto* structure =
        named ? llvm::StructType::create(context, members, "closure")
              : llvm::StructType::get(context, members);
    auto* signature = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                              {structure, pointer}, false);
    auto* function =
        llvm::Function::Create(signature, llvm::Function::ExternalLinkage,
                               "aggregate_function", module);
    function->addParamAttr(
        1, llvm::Attribute::getWithByValType(context, structure));
    return sun::abi::describeContract(module);
  };
  auto named = describe(true);
  const auto literal = describe(false);
  const auto expected = sun::abi::hashBundleContract(named);
  EXPECT_NO_THROW(sun::abi::mergeContractRequirements(named, literal));
  EXPECT_EQ(expected, sun::abi::hashBundleContract(named));
}

/** LLVM's appending initialization lists retain requirements of different
 * lengths. */
TEST(ABI_Compatibility, AppendingInitializationListsCanCoexist) {
  /** Generates initialization-list storage of the requested length. */
  auto describe = [](unsigned count) {
    llvm::LLVMContext context;
    llvm::Module module("initializers", context);
    module.setTargetTriple("x86_64-linux-gnu");
    module.setDataLayout(
        sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
    auto* pointer = llvm::PointerType::getUnqual(context);
    auto* entry = llvm::StructType::get(
        context, {llvm::Type::getInt32Ty(context), pointer, pointer});
    auto* list = llvm::ArrayType::get(entry, count);
    new llvm::GlobalVariable(
        module, list, false, llvm::GlobalValue::AppendingLinkage,
        llvm::Constant::getNullValue(list), "llvm.global_ctors");
    return sun::abi::describeContract(module);
  };
  auto outer = describe(1);
  const auto dependency = describe(2);
  EXPECT_NO_THROW(sun::abi::mergeContractRequirements(outer, dependency));
  EXPECT_EQ(outer.boundary_records_size(), 2);
}

/** Code generation and contract inspection share packing and alignment rules.
 */
TEST(ABI_Compatibility, SharedAggregateDescriptorControlsStorage) {
  llvm::LLVMContext context;
  const llvm::DataLayout layout(
      sun::abi::compatibilityTarget("x86_64-linux-gnu").data_layout());
  const std::vector<llvm::Type*> fields{llvm::Type::getInt8Ty(context),
                                        llvm::Type::getInt64Ty(context)};
  auto natural = sun::abi::AggregateLayout::inspect(
      sun::abi::AggregateLayout::build(context, fields), layout);
  auto packed = sun::abi::AggregateLayout::inspect(
      sun::abi::AggregateLayout::build(context, fields, true), layout);
  auto aligned = sun::abi::AggregateLayout::inspect(
      sun::abi::AggregateLayout::build(context, fields, false, 32), layout);
  EXPECT_EQ(natural.offsets, (std::vector<uint64_t>{0, 8}));
  EXPECT_EQ(natural.size, 16);
  EXPECT_EQ(packed.offsets, (std::vector<uint64_t>{0, 1}));
  EXPECT_EQ(packed.size, 9);
  EXPECT_EQ(packed.alignment, 1);
  EXPECT_EQ(aligned.size, 32);
  EXPECT_EQ(aligned.alignment, 32);
  EXPECT_THROW(sun::abi::AggregateLayout::build(context, fields, true, 32),
               sun::support::SunError);
}

/** Calls and invokes inherit exactly the ABI attributes retained by contracts.
 */
TEST(ABI_Compatibility, SharedCallDescriptorPreservesConventionAndAttributes) {
  llvm::LLVMContext context;
  llvm::Module module("calls", context);
  auto* i32 = llvm::Type::getInt32Ty(context);
  auto direct = sun::abi::CallDescriptor::sun(i32, {i32});
  auto closure = sun::abi::CallDescriptor::sun(i32, {i32}, true);
  ASSERT_EQ(closure.signature->getNumParams(), 2);
  EXPECT_TRUE(closure.signature->getParamType(0)->isPointerTy());
  auto* function = llvm::Function::Create(
      direct.signature, llvm::GlobalValue::ExternalLinkage, "callee", module);
  function->setCallingConv(llvm::CallingConv::Fast);
  function->addParamAttr(0, llvm::Attribute::SExt);
  function->addFnAttr(llvm::Attribute::NoInline);
  auto* caller = llvm::Function::Create(
      direct.signature, llvm::GlobalValue::ExternalLinkage, "caller", module);
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", caller));
  auto* call = builder.CreateCall(function, {caller->getArg(0)});
  auto descriptor =
      sun::abi::CallDescriptor::forCall(direct.signature, function);
  descriptor.apply(*call);
  EXPECT_EQ(call->getCallingConv(), llvm::CallingConv::Fast);
  EXPECT_TRUE(call->paramHasAttr(0, llvm::Attribute::SExt));
  EXPECT_FALSE(call->getAttributes().hasFnAttr(llvm::Attribute::NoInline));
  EXPECT_EQ(descriptor.encodedAttributes(),
            sun::abi::CallDescriptor::inspect(*call).encodedAttributes());
  auto* next = llvm::BasicBlock::Create(context, "next", caller);
  auto* unwind = llvm::BasicBlock::Create(context, "unwind", caller);
  auto* invoke =
      builder.CreateInvoke(function, next, unwind, {caller->getArg(0)});
  descriptor.apply(*invoke);
  EXPECT_EQ(invoke->getCallingConv(), llvm::CallingConv::Fast);
  EXPECT_EQ(descriptor.encodedAttributes(),
            sun::abi::CallDescriptor::inspect(*invoke).encodedAttributes());
  closure.attributes = llvm::AttributeList().addParamAttribute(
      context, 1,
      llvm::Attribute::getWithStackAlignment(context, llvm::Align(16)));
  EXPECT_FALSE(closure.encodedAttributes().empty());
}

/** Every enum view uses the same payload boundary and fits in its storage. */
TEST(ABI_Compatibility, SharedEnumDescriptorPreservesAlignedPayloads) {
  llvm::LLVMContext context;
  const llvm::DataLayout layout(
      sun::abi::compatibilityTarget("x86_64-linux-gnu").data_layout());
  auto* wide = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(context), 32);
  const auto descriptor = sun::abi::EnumLayout::build(
      context, layout, {{}, {llvm::Type::getInt8Ty(context)}, {wide}});
  const auto storage =
      sun::abi::AggregateLayout::inspect(descriptor.storage, layout);
  ASSERT_EQ(descriptor.variants.size(), 3);
  EXPECT_EQ(storage.alignment, 32);
  EXPECT_EQ(storage.offsets[1], 32);
  for (size_t i = 1; i < descriptor.variants.size(); ++i) {
    const auto view =
        sun::abi::AggregateLayout::inspect(descriptor.variants[i], layout);
    EXPECT_EQ(view.offsets.back(), storage.offsets[1]);
    EXPECT_LE(view.size, storage.size);
  }
}

/** Constructs a real compiler probe and a directly guarded local kernel. */
namespace {
/** Owns a small dispatch module for independent guard and requirement checks.
 */
struct CPUDispatchFixture {
  llvm::LLVMContext context;
  llvm::Module module{"dispatch", context};
  llvm::Function* kernel;
  llvm::Function* caller;
  llvm::BranchInst* guard;
  llvm::BasicBlock* ready;
  llvm::BasicBlock* fallback;

  /** Creates a baseline caller whose successful probe edge gates AVX2/FMA. */
  CPUDispatchFixture() {
    module.setTargetTriple("x86_64-linux-gnu");
    module.setDataLayout(
        sun::abi::compatibilityTarget(module.getTargetTriple()).data_layout());
    llvm::IRBuilder<> builder(context);
    auto* signature = llvm::FunctionType::get(builder.getVoidTy(), false);
    kernel = llvm::Function::Create(
        signature, llvm::GlobalValue::InternalLinkage, "kernel", module);
    kernel->addFnAttr(llvm::Attribute::NoInline);
    kernel->addFnAttr("target-cpu", "x86-64");
    kernel->addFnAttr("target-features", "+avx,+avx2,+fma,-avx512f");
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", kernel));
    builder.CreateRetVoid();
    caller = llvm::Function::Create(
        signature, llvm::GlobalValue::ExternalLinkage, "caller", module);
    auto* entry = llvm::BasicBlock::Create(context, "entry", caller);
    ready = llvm::BasicBlock::Create(context, "ready", caller);
    fallback = llvm::BasicBlock::Create(context, "fallback", caller);
    builder.SetInsertPoint(entry);
    auto* supported = builder.CreateCall(sun::abi::createAvx2FmaProbe(module));
    guard = builder.CreateCondBr(supported, ready, fallback);
    builder.SetInsertPoint(ready);
    builder.CreateCall(kernel);
    builder.CreateRetVoid();
    builder.SetInsertPoint(fallback);
    builder.CreateRetVoid();
  }
};
}  // namespace

/** Optional kernels do not raise the destination baseline when every call is
 * guarded. */
TEST(ABI_Compatibility, GuardedLLVMKernelAcceptsGenericDestination) {
  CPUDispatchFixture fixture;
  ASSERT_TRUE(sun::abi::hasGuardedCPURequirements(*fixture.kernel));
  auto contract = sun::abi::describeContract(fixture.module);
  EXPECT_EQ(contract.target().cpu_features_size(), 0);
  sun::abi::AbiCompatibilityChecker checker(
      "x86_64-linux-gnu", sun::abi::NativeExecution::NativeLink);
  EXPECT_NO_THROW(
      checker.validateContract(contract, "guarded.moon", "x86_64-linux-gnu"));
  // Symbol isolation can rename the probe without changing its implementation.
  sun::abi::createAvx2FmaProbe(fixture.module)->setName("isolated.probe");
  EXPECT_TRUE(sun::abi::hasGuardedCPURequirements(*fixture.kernel));
}

/** An inverted guard or a forged always-true probe cannot suppress
 * requirements. */
TEST(ABI_Compatibility, LLVMDispatchRequiresActualProbeAndSuccessfulEdge) {
  CPUDispatchFixture fixture;
  fixture.guard->swapSuccessors();
  EXPECT_FALSE(sun::abi::hasGuardedCPURequirements(*fixture.kernel));
  fixture.guard->swapSuccessors();
  auto* probe = sun::abi::createAvx2FmaProbe(fixture.module);
  probe->deleteBody();
  llvm::IRBuilder<> builder(
      llvm::BasicBlock::Create(fixture.context, "entry", probe));
  builder.CreateRet(builder.getTrue());
  EXPECT_FALSE(sun::abi::hasGuardedCPURequirements(*fixture.kernel));
  auto contract = sun::abi::describeContract(fixture.module);
  EXPECT_GT(contract.target().cpu_features_size(), 0);
  sun::abi::AbiCompatibilityChecker checker(
      "x86_64-linux-gnu", sun::abi::NativeExecution::NativeLink);
  EXPECT_THROW(
      checker.validateContract(contract, "forged.moon", "x86_64-linux-gnu"),
      sun::support::SunError);
}

/** Every entry into a targeted kernel must be guarded, including escaped
 * addresses. */
TEST(ABI_Compatibility, LLVMDispatchRejectsUnguardedCallsAndEscapingKernels) {
  CPUDispatchFixture fixture;
  llvm::IRBuilder<> builder(fixture.fallback->getTerminator());
  auto* unguarded = builder.CreateCall(fixture.kernel);
  EXPECT_FALSE(sun::abi::hasGuardedCPURequirements(*fixture.kernel));
  unguarded->eraseFromParent();
  EXPECT_TRUE(sun::abi::hasGuardedCPURequirements(*fixture.kernel));
  new llvm::GlobalVariable(fixture.module, fixture.kernel->getType(), false,
                           llvm::GlobalValue::ExternalLinkage, fixture.kernel,
                           "escaped");
  EXPECT_FALSE(sun::abi::hasGuardedCPURequirements(*fixture.kernel));
}

/** A probe covers only its known instruction set, not arbitrary targeted code.
 */
TEST(ABI_Compatibility, LLVMDispatchDoesNotAuthorizeAdditionalFeatures) {
  CPUDispatchFixture fixture;
  fixture.kernel->addFnAttr("target-features", "+avx,+avx2,+fma,+avx512f");
  EXPECT_FALSE(sun::abi::hasGuardedCPURequirements(*fixture.kernel));
}

/** Native compilation checks the generic destination even on an AVX2 build
 * host. */
TEST(ABI_Compatibility, NativeDestinationDoesNotBorrowHostFeatures) {
  CPUDispatchFixture fixture;
  auto contract = sun::abi::describeContract(fixture.module);
  contract.mutable_target()->add_cpu_features("avx2");
  contract.set_compatibility_hash(sun::abi::hashBundleContract(contract));
  sun::abi::AbiCompatibilityChecker checker(
      "x86_64-linux-gnu", sun::abi::NativeExecution::NativeLink);
  EXPECT_THROW(checker.validateContract(contract, "required-avx2.moon",
                                        "x86_64-linux-gnu"),
               sun::support::SunError);
}

/** Rehashing metadata cannot conceal an unsafe dispatch edge in actual bitcode.
 */
TEST(ABI_Compatibility, RehashedDispatchContractCannotHideUnguardedPayload) {
  CPUDispatchFixture fixture;
  std::filesystem::create_directories("tmp");
  const auto path = std::filesystem::path("tmp") /
                    ("abi_dispatch_" + std::to_string(getpid()) + ".moon");
  sun::moon::ModuleMetadata metadata;
  metadata.set_source_hash("dispatch");
  sun::moon_bundling::MoonWriter original(std::string(64, 'd'));
  original.addModule(fixture.module, metadata);
  ASSERT_TRUE(original.write(path));
  auto reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  sun::abi::AbiCompatibilityChecker checker(
      "x86_64-linux-gnu", sun::abi::NativeExecution::NativeLink);
  ASSERT_NO_THROW(checker.validate(*reader));

  fixture.guard->swapSuccessors();
  sun::moon_bundling::MoonWriter changed(std::string(64, 'e'));
  changed.addModule(fixture.module, metadata);
  ASSERT_TRUE(changed.write(path));
  reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  auto library = reader->getLibrary();
  auto* contract = library.mutable_bundle_contract();
  ASSERT_GT(contract->target().cpu_features_size(), 0);
  contract->mutable_target()->clear_cpu_features();
  contract->set_compatibility_hash(sun::abi::hashBundleContract(*contract));
  ASSERT_NO_THROW(
      checker.validateContract(*contract, path.string(), "x86_64-linux-gnu"));
  auto bytes = *reader->getBytes();
  sun::moon_bundling::MoonHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  auto encoded = library.SerializeAsString();
  bytes.resize(header.bundleMetadataOffset);
  bytes += encoded;
  header.bundleMetadataSize = encoded.size();
  std::memcpy(bytes.data(), &header, sizeof(header));
  {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), bytes.size());
  }
  reader = sun::moon_bundling::MoonReader::open(path);
  ASSERT_NE(reader, nullptr);
  EXPECT_THROW(checker.validate(*reader), sun::support::SunError);
  std::filesystem::remove(path);
}

/** A direct call does not authorize passing the kernel address as call data. */
TEST(ABI_Compatibility, LLVMDispatchRejectsKernelAddressInOperandBundle) {
  CPUDispatchFixture fixture;
  llvm::IRBuilder<> builder(fixture.ready->getTerminator());
  llvm::OperandBundleDef escaped("deopt",
                                 std::vector<llvm::Value*>{fixture.kernel});
  builder.CreateCall(fixture.kernel, {}, {escaped});
  EXPECT_FALSE(sun::abi::hasGuardedCPURequirements(*fixture.kernel));
}
