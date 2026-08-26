// struct_names.h — Well-known struct type names for LLVM codegen
//
// This header defines canonical names for compiler-internal struct types.
// Keep this header minimal to avoid circular dependencies - it can be
// included by both types.h and llvm_type_resolver.h.

#pragma once

#include <cstddef>

namespace sun {
namespace StructNames {

// Layout patterns for well-known struct types
enum class Layout {
  PtrPtr,     // { ptr, ptr }       - closure, interface_fat
  PtrI64,     // { ptr, i64 }       - static_ptr_struct
  PtrI32Ptr,  // { ptr, i32, ptr }  - array_struct
};

// Info about a well-known struct type
struct StructInfo {
  const char* name;
  Layout layout;
};

// All well-known struct types - iterate over this array
constexpr StructInfo All[] = {
    {"closure", Layout::PtrPtr},            // { ptr func, ptr env }
    {"static_ptr_struct", Layout::PtrI64},  // { ptr data, i64 length }
    {"interface_fat", Layout::PtrPtr},      // { ptr data, ptr vtable }
    {"array_struct", Layout::PtrI32Ptr},    // { ptr data, i32 ndims, ptr dims }
};

constexpr size_t Count = sizeof(All) / sizeof(All[0]);

// Named constants for direct access (for use in types.h etc.)
constexpr const char* Closure = "closure";
constexpr const char* StaticPtr = "static_ptr_struct";
constexpr const char* InterfaceFat = "interface_fat";
constexpr const char* ArrayStruct = "array_struct";

}  // namespace StructNames
}  // namespace sun
