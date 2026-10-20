#include "abi/compatibility.h"

#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Instructions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <mutex>

#include "abi/boundary_requirements.h"
#include "abi/support_inventory.h"
#ifdef __linux__
#include <link.h>
#endif

#include <llvm/Support/SHA256.h>

#include "abi/producer_identity.h"
#include "generated/sun_compatibility.h"
#include "support/error.h"
#include "support/terminal.h"

/** Generates and checks complete Moon bundle requirements before semantic use.
 */
namespace sun::abi {
/** Private canonical encoding and implementation discovery helpers. */
namespace {
/** Hashes canonical contract bytes without depending on a container
 * implementation. */
std::string computeSha256Hex(llvm::StringRef bytes) {
  llvm::SHA256 hash;
  hash.update(bytes);
  std::string result;
  constexpr char hex[] = "0123456789abcdef";
  for (auto byte : hash.final()) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}
/** Encodes an unsigned integer without host layout or byte-order dependencies.
 */
std::string integer(uint64_t value) {
  std::string result;
  for (unsigned shift = 0; shift < 64; shift += 8)
    result.push_back(static_cast<char>(value >> shift));
  return result;
}
/** Frames a tagged field so arbitrary byte strings remain unambiguous. */
std::string field(uint64_t tag, const std::string& value) {
  return integer(tag) + integer(value.size()) + value;
}
/** Canonically encodes sets while preserving order within each entry. */
template <typename Range>
std::string sortedFields(uint64_t tag, const Range& values) {
  std::set<std::string> ordered(values.begin(), values.end());
  std::string result;
  for (const auto& value : ordered) result += field(tag, value);
  return result;
}
/** Encodes effective target properties rather than a triple's spelling. */
std::string encodeTarget(const TargetProfile& target) {
  return field(1, target.architecture()) + field(2, target.platform_abi()) +
         field(3, target.data_layout()) +
         field(4, integer(target.pointer_bits())) +
         field(5, integer(target.little_endian())) +
         sortedFields(6, target.cpu_features());
}
/** Records a required rule with its independently identifiable implementation.
 */
void require(BundleContract& contract, const std::string& name,
             const std::string& implementation) {
  for (const auto& rule : contract.requirements()) {
    if (rule.name() != name) continue;
    if (rule.implementation() != implementation)
      sun::support::logAndThrowError("Conflicting bundle requirement: " + name);
    return;
  }
  auto* rule = contract.add_requirements();
  rule->set_name(name);
  rule->set_implementation(implementation);
}
/** Describes machine types structurally while keeping opaque nominal
 * references. LLVM linking can replace a named aggregate with its identical
 * literal layout. Sun nominal identities are validated separately from these
 * machine shapes.
 */
std::string typeName(llvm::Type* type) {
  if (auto* structure = llvm::dyn_cast<llvm::StructType>(type);
      structure && structure->isSized()) {
    std::string result = structure->isPacked() ? "packed{" : "struct{";
    for (auto* member : structure->elements()) result += typeName(member) + ";";
    return result + "}";
  }
  if (auto* array = llvm::dyn_cast<llvm::ArrayType>(type))
    return "array[" + std::to_string(array->getNumElements()) + ":" +
           typeName(array->getElementType()) + "]";
  if (auto* function = llvm::dyn_cast<llvm::FunctionType>(type)) {
    std::string result = typeName(function->getReturnType()) + "(";
    for (auto* parameter : function->params())
      result += typeName(parameter) + ";";
    return result + (function->isVarArg() ? "...)" : ")");
  }
  std::string result;
  llvm::raw_string_ostream stream(result);
  type->print(stream, false, true);
  return result;
}
/** Retains ABI attributes, excluding optimizer facts and diagnostic attributes.
 */
std::string abiAttributes(llvm::AttributeList attributes) {
  std::string result;
  for (unsigned index : attributes.indexes()) {
    for (auto attribute : attributes.getAttributes(index)) {
      if (attribute.isStringAttribute()) continue;
      switch (attribute.getKindAsEnum()) {
        case llvm::Attribute::StructRet:
        case llvm::Attribute::ByVal:
        case llvm::Attribute::ByRef:
        case llvm::Attribute::InAlloca:
        case llvm::Attribute::Preallocated:
        case llvm::Attribute::InReg:
        case llvm::Attribute::SExt:
        case llvm::Attribute::ZExt:
        case llvm::Attribute::Alignment:
        case llvm::Attribute::Nest:
        case llvm::Attribute::SwiftSelf:
        case llvm::Attribute::SwiftError:
          result += field(
              index, attribute.isTypeAttribute()
                         ? llvm::Attribute::getNameFromAttrKind(
                               attribute.getKindAsEnum())
                                   .str() +
                               "(" + typeName(attribute.getValueAsType()) + ")"
                         : attribute.getAsString());
          break;
        default:
          break;
      }
    }
  }
  return result;
}
#ifdef __linux__
/** Collects the actual shared implementation dependencies, excluding the host
 * executable. */
int collectLibraries(dl_phdr_info* info, size_t, void* opaque) {
  auto& paths = *static_cast<std::set<std::string>*>(opaque);
  std::string path = info->dlpi_name;
  if (!path.empty() && path.find("linux-vdso") == std::string::npos)
    paths.insert(std::move(path));
  return 0;
}
/** Capture compiler dependencies before JIT code can load application
 * libraries. */
const std::set<std::string> implementationLibraries = [] {
  std::set<std::string> paths;
  dl_iterate_phdr(collectLibraries, &paths);
  return paths;
}();
#endif
}  // namespace

const std::string& compatibilityImplementation() {
  static const std::string fingerprint = [] {
    std::set<std::string> digests;
#ifdef __linux__
    for (const auto& path : implementationLibraries) {
      auto bytes = llvm::MemoryBuffer::getFile(path);
      if (!bytes)
        sun::support::logAndThrowError(
            "Cannot fingerprint implementation dependency: " + path);
      digests.insert(computeSha256Hex((*bytes)->getBuffer()));
    }
#else
    sun::support::logAndThrowError(
        "Experimental compatibility dependency discovery requires Linux; this "
        "host is unsupported");
#endif
    return computeSha256Hex(field(1, SUN_COMPATIBILITY_IMPLEMENTATION) +
                            sortedFields(2, digests));
  }();
  return fingerprint;
}

std::string encodeBundleContract(const BundleContract& contract) {
  std::vector<std::string> rules;
  for (const auto& rule : contract.requirements())
    rules.push_back(field(1, rule.name()) + field(2, rule.implementation()));
  return field(1, "sun.abi.bundle-contract") +
         field(2, integer(contract.encoding_schema())) +
         field(3, encodeTarget(contract.target())) + sortedFields(4, rules) +
         sortedFields(5, contract.boundary_records()) +
         field(6, integer(contract.experimental())) +
         sortedFields(7, contract.semantic_records());
}

std::string hashBundleContract(const BundleContract& contract) {
  return computeSha256Hex(encodeBundleContract(contract));
}

TargetProfile compatibilityTarget(const std::string& requested) {
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
  });
  llvm::Triple triple(llvm::Triple::normalize(
      requested.empty() ? llvm::sys::getDefaultTargetTriple() : requested));
  std::string error;
  const auto* target = llvm::TargetRegistry::lookupTarget(triple.str(), error);
  if (!target)
    sun::support::logAndThrowError("Unsupported compatibility target: " +
                                   error);
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
      triple.str(), "generic", "", options, llvm::Reloc::PIC_));
  if (!machine)
    sun::support::logAndThrowError("Cannot derive compatibility target");
  const auto layout = machine->createDataLayout();
  TargetProfile profile;
  profile.set_architecture(triple.getArchName().str());
  profile.set_platform_abi(triple.getOSName().str() + ":" +
                           triple.getEnvironmentName().str());
  profile.set_data_layout(layout.getStringRepresentation());
  profile.set_pointer_bits(layout.getPointerSizeInBits());
  profile.set_little_endian(layout.isLittleEndian());
  return profile;
}

std::vector<std::string> describeBoundaries(const llvm::Module& module) {
  std::set<std::string> records;
  const auto& layout = module.getDataLayout();
  for (auto* type : module.getIdentifiedStructTypes()) {
    if (type->isOpaque() || !type->isSized()) continue;
    const auto* structure = layout.getStructLayout(type);
    std::string record = field(1, type->getName().str()) +
                         field(2, integer(structure->getSizeInBytes())) +
                         field(3, integer(structure->getAlignment().value()));
    for (unsigned i = 0; i < type->getNumElements(); ++i)
      record += field(4, integer(structure->getElementOffset(i)) +
                             field(1, typeName(type->getElementType(i))));
    records.insert(field(1, record));
  }
  for (const auto& function : module) {
    if (!function.hasLocalLinkage())
      records.insert(
          field(2, field(1, function.getName().str()) +
                       field(2, typeName(function.getFunctionType())) +
                       field(3, integer(function.getCallingConv())) +
                       field(4, abiAttributes(function.getAttributes()))));
    for (const auto& block : function)
      for (const auto& instruction : block)
        if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
          records.insert(
              field(3, field(1, typeName(call->getFunctionType())) +
                           field(2, integer(call->getCallingConv())) +
                           field(3, abiAttributes(call->getAttributes()))));
  }
  for (const auto& global : module.globals()) {
    if (global.hasLocalLinkage()) continue;
    records.insert(
        field(4, field(1, global.getName().str()) +
                     field(2, typeName(global.getValueType())) +
                     field(3, integer(global.getAlign().valueOrOne().value())) +
                     field(4, integer(global.getThreadLocalMode()))));
  }
  return {records.begin(), records.end()};
}

BundleContract describeContract(const llvm::Module& module) {
  BundleContract contract;
  contract.set_encoding_schema(1);
  contract.set_experimental(true);
  *contract.mutable_target() = compatibilityTarget(module.getTargetTriple());
  if (!module.getDataLayoutStr().empty()) {
    contract.mutable_target()->set_data_layout(module.getDataLayoutStr());
    contract.mutable_target()->set_pointer_bits(
        module.getDataLayout().getPointerSizeInBits());
    contract.mutable_target()->set_little_endian(
        module.getDataLayout().isLittleEndian());
  }
  require(contract, "moon.interface", emissionImplementation("moon.interface"));
  require(contract, "llvm.bitcode", emissionImplementation("llvm.bitcode"));
  require(contract, "sun.experimental.semantics",
          emissionImplementation("sun.experimental.semantics"));
  for (const auto& function : module) {
    const auto features = function.getFnAttribute("target-features");
    if (features.isValid()) {
      llvm::SubtargetFeatures parsed(features.getValueAsString());
      for (const auto& feature : parsed.getFeatures())
        if (!feature.empty() && feature[0] == '+')
          contract.mutable_target()->add_cpu_features(feature.substr(1));
    }
    const auto cpu = function.getFnAttribute("target-cpu");
    if (cpu.isValid() && cpu.getValueAsString() != "generic" &&
        cpu.getValueAsString() != "x86-64")
      require(contract, "llvm.cpu." + cpu.getValueAsString().str(),
              compatibilityImplementation());
  }
  for (const auto& record : describeBoundaries(module))
    contract.add_boundary_records(record);
  *contract.mutable_producer() = currentProducerIdentity();
  contract.set_compatibility_hash(hashBundleContract(contract));
  return contract;
}

void mergeContractRequirements(BundleContract& destination,
                               const BundleContract& inherited) {
  auto left = destination.target();
  auto right = inherited.target();
  left.clear_cpu_features();
  right.clear_cpu_features();
  if (encodeTarget(left) != encodeTarget(right))
    sun::support::logAndThrowError(
        "Conflicting embedded bundle target requirements");
  BoundaryRequirements boundaries;
  for (const auto& record : destination.boundary_records())
    boundaries.add(record);
  for (const auto& record : inherited.boundary_records())
    boundaries.add(record);
  // Merge into a copy so rejected dependencies cannot leave partial
  // requirements.
  auto merged = destination;
  for (const auto& rule : inherited.requirements())
    require(merged, rule.name(), rule.implementation());
  std::set<std::string> features(destination.target().cpu_features().begin(),
                                 destination.target().cpu_features().end());
  features.insert(inherited.target().cpu_features().begin(),
                  inherited.target().cpu_features().end());
  merged.mutable_target()->clear_cpu_features();
  for (const auto& feature : features)
    merged.mutable_target()->add_cpu_features(feature);
  // Retain dependencies even when linking has internalized their code.
  merged.clear_boundary_records();
  for (const auto& record : boundaries.records())
    merged.add_boundary_records(record);
  std::set<std::string> semantics(destination.semantic_records().begin(),
                                  destination.semantic_records().end());
  semantics.insert(inherited.semantic_records().begin(),
                   inherited.semantic_records().end());
  merged.clear_semantic_records();
  for (const auto& record : semantics) merged.add_semantic_records(record);
  std::set<std::string> symbols(destination.native_link_symbols().begin(),
                                destination.native_link_symbols().end());
  symbols.insert(inherited.native_link_symbols().begin(),
                 inherited.native_link_symbols().end());
  merged.clear_native_link_symbols();
  for (const auto& symbol : symbols) merged.add_native_link_symbols(symbol);
  destination = std::move(merged);
}

}  // namespace sun::abi
