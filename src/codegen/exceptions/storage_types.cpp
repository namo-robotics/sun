/** Defines generated exception layouts shared by lowering and TLS emission. */
#include "codegen/exceptions/storage_types.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <string>

#include "abi/exception_layout.h"
#include "support/error.h"

/** Generates complete storage compatible with the precompiled runtime. */
namespace sun::codegen::exceptions {
/** Defines depth-specific structs whose offsets match the precompiled runtime.
 * Explicit padding preserves the native ABI even for odd or zero capacities.
 * Allocations of these types must use the exception ABI's alignment.
 */
StorageTypes storageTypes(llvm::Module& module, unsigned depth) {
  /** Names the layout contract shared with the native runtime. */
  namespace layout = sun::abi::exceptions;
  if (depth > layout::maximumTraceDepth) {
    sun::support::logAndThrowError("Invalid exception trace capacity");
  }
  const auto& data = module.getDataLayout();
  const auto pointerSize = data.getPointerSize();
  if (pointerSize != sizeof(std::uint64_t)) {
    sun::support::logAndThrowError("Native exceptions require 64-bit pointers");
  }
  auto& context = module.getContext();
  auto* byte = llvm::Type::getInt8Ty(context);
  auto* word = llvm::Type::getInt64Ty(context);
  auto* integer = llvm::Type::getInt32Ty(context);
  auto* pointer = llvm::PointerType::getUnqual(context);
  /** Reuses a named layout when several functions share the same capacity. */
  const auto define = [&](const char* name,
                          llvm::ArrayRef<llvm::Type*> fields) {
    const auto qualified =
        std::string("sun.") + name + "." + std::to_string(depth);
    auto* type = llvm::StructType::getTypeByName(context, qualified);
    if (!type) {
      type = llvm::StructType::create(context, fields, qualified);
    }
    return type;
  };
  auto* trace = define("StackTrace", {word, integer, integer,
                                      llvm::ArrayType::get(word, depth)});
  const auto tail = layout::valueSizeForDepth(depth) -
                    (layout::traceFramesOffset + depth * sizeof(std::uint64_t));
  auto* value = define(
      "ExceptionValue",
      {pointer, llvm::ArrayType::get(byte, layout::payloadOffset - pointerSize),
       llvm::ArrayType::get(byte, layout::payloadCapacity), trace,
       llvm::ArrayType::get(byte, tail)});
  auto* record =
      define("PropagationRecord",
             {llvm::ArrayType::get(byte, layout::unwindHeaderSize), value});
  auto* thread = define(
      "ThreadExceptionState",
      {pointer, llvm::ArrayType::get(byte, layout::alignment - pointerSize),
       record});
  if (data.getTypeAllocSize(value) != layout::valueSizeForDepth(depth) ||
      data.getTypeAllocSize(record) !=
          layout::unwindHeaderSize + layout::valueSizeForDepth(depth) ||
      data.getTypeAllocSize(thread) != layout::threadStateSizeForDepth(depth) ||
      data.getStructLayout(value)->getElementOffset(2) !=
          layout::payloadOffset ||
      data.getStructLayout(value)->getElementOffset(3) != layout::traceOffset ||
      data.getStructLayout(trace)->getElementOffset(1) !=
          layout::traceStatusOffset - layout::traceOffset ||
      data.getStructLayout(trace)->getElementOffset(3) !=
          layout::traceFramesOffset - layout::traceOffset ||
      data.getStructLayout(record)->getElementOffset(1) !=
          layout::unwindHeaderSize ||
      data.getStructLayout(thread)->getElementOffset(2) != layout::alignment) {
    sun::support::logAndThrowError(
        "Generated exception storage does not match the native ABI");
  }
  return {value, record, thread};
}
}  // namespace sun::codegen::exceptions
