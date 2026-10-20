#include "abi/lowering_descriptors.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "support/error.h"

/** Describes machine representations shared by code generation and contracts.
 */
namespace sun::abi {
/** Keeps canonical framing and ABI attribute selection private. */
namespace {
/** Encodes fixed-width integers independently of host byte order. */
std::string integer(uint64_t value) {
  std::string result;
  for (unsigned shift = 0; shift < 64; shift += 8)
    result.push_back(static_cast<char>(value >> shift));
  return result;
}
/** Frames an attribute's index and value without delimiter ambiguity. */
std::string field(uint64_t tag, const std::string& value) {
  return integer(tag) + integer(value.size()) + value;
}
/** Selects attributes that must agree between declarations and call sites. */
bool abiAttribute(llvm::Attribute attribute) {
  if (attribute.isStringAttribute()) return false;
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
    case llvm::Attribute::StackAlignment:
    case llvm::Attribute::Nest:
    case llvm::Attribute::SwiftSelf:
    case llvm::Attribute::SwiftError:
    case llvm::Attribute::SwiftAsync:
      return true;
    default:
      return false;
  }
}
}  // namespace

AggregateLayout AggregateLayout::inspect(llvm::StructType* type,
                                         const llvm::DataLayout& layout) {
  if (!type->isSized())
    sun::support::logAndThrowError("Cannot describe an unsized ABI layout");
  const auto* storage = layout.getStructLayout(type);
  AggregateLayout result{storage->getSizeInBytes().getFixedValue(),
                         storage->getAlignment().value(),
                         {}};
  for (unsigned i = 0; i < type->getNumElements(); ++i)
    result.offsets.push_back(storage->getElementOffset(i));
  return result;
}

llvm::StructType* AggregateLayout::build(llvm::LLVMContext& context,
                                         llvm::ArrayRef<llvm::Type*> fields,
                                         bool packed, uint64_t alignment) {
  if (alignment &&
      (packed || (alignment & (alignment - 1)) || alignment > 65536))
    sun::support::logAndThrowError(
        "Invalid explicit class alignment in declaration metadata");
  std::vector<llvm::Type*> storage(fields.begin(), fields.end());
  if (alignment) {
    auto* marker =
        llvm::FixedVectorType::get(llvm::Type::getInt8Ty(context), alignment);
    storage.push_back(llvm::ArrayType::get(marker, 0));
  }
  return llvm::StructType::get(context, storage, packed);
}

EnumLayout EnumLayout::build(
    llvm::LLVMContext& context, const llvm::DataLayout& layout,
    const std::vector<std::vector<llvm::Type*>>& payloads) {
  llvm::Align alignment(4);
  for (const auto& payload : payloads)
    for (auto* type : payload)
      alignment = std::max(alignment, layout.getABITypeAlign(type));
  const uint64_t unitBytes = alignment.value();
  const uint64_t tagArea = llvm::alignTo(4, unitBytes);
  uint64_t size = 8;
  EnumLayout result{};
  for (const auto& payload : payloads) {
    std::vector<llvm::Type*> fields{llvm::Type::getInt32Ty(context)};
    if (tagArea > 4)
      fields.push_back(
          llvm::ArrayType::get(llvm::Type::getInt8Ty(context), tagArea - 4));
    fields.insert(fields.end(), payload.begin(), payload.end());
    auto* variant = llvm::StructType::get(context, fields);
    result.variants.push_back(variant);
    size = std::max(size, layout.getTypeAllocSize(variant).getFixedValue());
  }
  auto* unit = unitBytes > 8
                   ? static_cast<llvm::Type*>(llvm::FixedVectorType::get(
                         llvm::Type::getInt8Ty(context), unitBytes))
                   : llvm::Type::getIntNTy(context, unitBytes * 8);
  const uint64_t payloadBytes = size > tagArea ? size - tagArea : 0;
  result.storage = llvm::StructType::get(
      context,
      {llvm::Type::getInt32Ty(context),
       llvm::ArrayType::get(unit, (payloadBytes + unitBytes - 1) / unitBytes)});
  return result;
}

CallDescriptor CallDescriptor::sun(llvm::Type* result,
                                   llvm::ArrayRef<llvm::Type*> parameters,
                                   bool closure) {
  std::vector<llvm::Type*> lowered;
  if (closure)
    lowered.push_back(llvm::PointerType::getUnqual(result->getContext()));
  lowered.insert(lowered.end(), parameters.begin(), parameters.end());
  return {llvm::FunctionType::get(result, lowered, false)};
}

CallDescriptor CallDescriptor::inspect(const llvm::Function& function) {
  return {function.getFunctionType(), function.getCallingConv(),
          function.getAttributes()};
}

CallDescriptor CallDescriptor::inspect(const llvm::CallBase& call) {
  return {call.getFunctionType(), call.getCallingConv(), call.getAttributes()};
}

CallDescriptor CallDescriptor::forCall(llvm::FunctionType* signature,
                                       llvm::Value* callee) {
  if (auto* function =
          llvm::dyn_cast<llvm::Function>(callee->stripPointerCasts());
      function && function->getFunctionType() == signature)
    return inspect(*function);
  return {signature};
}

void CallDescriptor::apply(llvm::CallBase& call) const {
  if (call.getFunctionType() != signature)
    sun::support::logAndThrowError("Call does not match its ABI signature");
  call.setCallingConv(convention);
  for (unsigned index : attributes.indexes())
    for (auto attribute : attributes.getAttributes(index))
      if (abiAttribute(attribute)) call.addAttributeAtIndex(index, attribute);
}

std::string CallDescriptor::encodedAttributes() const {
  std::string result;
  for (unsigned index : attributes.indexes())
    for (auto attribute : attributes.getAttributes(index)) {
      if (!abiAttribute(attribute)) continue;
      result += field(
          index,
          attribute.isTypeAttribute()
              ? llvm::Attribute::getNameFromAttrKind(attribute.getKindAsEnum())
                        .str() +
                    "(" + machineTypeName(attribute.getValueAsType()) + ")"
              : attribute.getAsString());
    }
  return result;
}

/** Describes machine types structurally while keeping opaque nominal
 * references. LLVM linking can replace a named aggregate with its identical
 * literal layout. Sun nominal identities are validated separately from these
 * machine shapes.
 */
std::string machineTypeName(llvm::Type* type) {
  if (auto* structure = llvm::dyn_cast<llvm::StructType>(type);
      structure && structure->isSized()) {
    std::string result = structure->isPacked() ? "packed{" : "struct{";
    for (auto* member : structure->elements())
      result += machineTypeName(member) + ";";
    return result + "}";
  }
  if (auto* array = llvm::dyn_cast<llvm::ArrayType>(type))
    return "array[" + std::to_string(array->getNumElements()) + ":" +
           machineTypeName(array->getElementType()) + "]";
  if (auto* function = llvm::dyn_cast<llvm::FunctionType>(type)) {
    std::string result = machineTypeName(function->getReturnType()) + "(";
    for (auto* parameter : function->params())
      result += machineTypeName(parameter) + ";";
    return result + (function->isVarArg() ? "...)" : ")");
  }
  std::string result;
  llvm::raw_string_ostream stream(result);
  type->print(stream, false, true);
  return result;
}
void recordClassLayout(llvm::Module& module, const std::string& identity,
                       llvm::StructType* storage) {
  if (identity.empty() || !storage->isSized())
    sun::support::logAndThrowError(
        "Cannot record an incomplete class ABI layout");
  auto& context = module.getContext();
  module.getOrInsertNamedMetadata("sun.abi.classes")
      ->addOperand(llvm::MDNode::get(
          context,
          {llvm::MDString::get(context, identity),
           llvm::ConstantAsMetadata::get(llvm::UndefValue::get(storage))}));
}

std::map<std::string, llvm::StructType*> classLayouts(
    const llvm::Module& module) {
  std::map<std::string, llvm::StructType*> layouts;
  const auto* records = module.getNamedMetadata("sun.abi.classes");
  if (!records) return layouts;
  for (const auto* record : records->operands()) {
    if (record->getNumOperands() != 2)
      sun::support::logAndThrowError("Malformed compiled class ABI descriptor");
    const auto* key =
        llvm::dyn_cast_or_null<llvm::MDString>(record->getOperand(0));
    const auto* value =
        llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(record->getOperand(1));
    auto* type =
        value ? llvm::dyn_cast<llvm::StructType>(value->getType()) : nullptr;
    if (!key || key->getString().empty() || !type || !type->isSized())
      sun::support::logAndThrowError("Malformed compiled class ABI descriptor");
    const auto [found, inserted] =
        layouts.emplace(key->getString().str(), type);
    if (!inserted && machineTypeName(found->second) != machineTypeName(type))
      sun::support::logAndThrowError("Conflicting compiled class ABI layout: " +
                                     key->getString().str());
  }
  return layouts;
}

}  // namespace sun::abi
