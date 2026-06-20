// intrinsics/memory.h — Memory management intrinsic definitions
//
// Memory intrinsics for heap allocation and raw memory access.
// _malloc and _free wrap libc; _load_i64/_store_i64 are direct memory ops.

#pragma once

namespace sun {

// Memory intrinsic identifiers are defined in the main Intrinsic enum.
// This header exists for organizational purposes and future expansion.
//
// Memory intrinsics:
//   _malloc(size) -> raw_ptr<i8>      Allocate size bytes
//   _free(ptr) -> void                Free previously allocated memory
//   _load_i64(ptr, index) -> i64      Load i64 at ptr[index]
//   _store_i64(ptr, index, val)       Store i64 at ptr[index]

}  // namespace sun
