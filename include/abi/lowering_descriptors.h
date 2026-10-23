#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>

#include <map>
#include <string>
#include <vector>

/** Provides LLVM function and instruction declarations. */
namespace llvm {
class Function;
class Module;
class CallBase;
class Value;
}  // namespace llvm

/** Describes machine representations shared by code generation and contracts.
 */
namespace sun::abi {
/** Records the target layout of an aggregate without conflating nominal types.
 */
struct AggregateLayout {
  uint64_t size;
  uint64_t alignment;
  std::vector<uint64_t> offsets;
  /** Derives allocation size, alignment, and field offsets from the target. */
  static AggregateLayout inspect(llvm::StructType* type,
                                 const llvm::DataLayout& layout);
  /** Builds Sun class storage, including packing and explicit alignment. */
  static llvm::StructType* build(llvm::LLVMContext& context,
                                 llvm::ArrayRef<llvm::Type*> fields,
                                 bool packed = false, uint64_t alignment = 0);
};

/** Describes tagged enum storage and the matching views of its payloads. */
struct EnumLayout {
  llvm::StructType* storage;
  std::vector<llvm::StructType*> variants;
  /** Builds a common tag area and storage large enough for every variant. */
  static EnumLayout build(
      llvm::LLVMContext& context, const llvm::DataLayout& layout,
      const std::vector<std::vector<llvm::Type*>>& payloads);
};

/** Records the lowered signature and attributes needed at both ends of a call.
 */
struct CallDescriptor {
  llvm::FunctionType* signature;
  llvm::CallingConv::ID convention = llvm::CallingConv::C;
  llvm::AttributeList attributes;
  /** Builds Sun's value signature, optionally prepending its closure pointer.
   */
  static CallDescriptor sun(llvm::Type* result,
                            llvm::ArrayRef<llvm::Type*> parameters,
                            bool closure = false);
  /** Reads a function's declared machine calling convention. */
  static CallDescriptor inspect(const llvm::Function& function);
  /** Reads the machine convention used by a call or invoke instruction. */
  static CallDescriptor inspect(const llvm::CallBase& call);
  /** Uses a direct callee's ABI, or Sun's default convention for indirect
   * calls. */
  static CallDescriptor forCall(llvm::FunctionType* signature,
                                llvm::Value* callee);
  /** Applies the same convention and ABI attributes to a call or invoke. */
  void apply(llvm::CallBase& call) const;
  /** Encodes only ABI attributes, excluding optimization facts and diagnostics.
   */
  std::string encodedAttributes() const;
};

/** Records the actual class storage under its portable source declaration
 * identity. */
void recordClassLayout(llvm::Module& module, const std::string& identity,
                       llvm::StructType* storage);
/** Reads recorded class layouts, rejecting malformed or conflicting identities.
 */
std::map<std::string, llvm::StructType*> classLayouts(
    const llvm::Module& module);

/** Describes LLVM storage structurally; source nominal identities stay
 * separate. */
std::string machineTypeName(llvm::Type* type);
}  // namespace sun::abi
