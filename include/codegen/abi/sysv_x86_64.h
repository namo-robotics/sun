// abi/sysv_x86_64.h — System V AMD64 argument classification
//
// What the psABI asks for, and what this module reproduces:
//
//   struct { int, int }        -> one i64 argument
//   struct { int, long }       -> two arguments: i32, i64
//   struct { double, double }  -> two arguments: double, double
//   struct { int, double }     -> two arguments: i32, double
//   struct of 20 bytes         -> one pointer argument marked `byval`
//   returning that 20-byte struct -> a prepended `sret` pointer, returns void
//
// Only aggregates need this; scalars and pointers already map one-to-one.
// These are x86-64 rules specifically — reach them through
// abi::lowerCSignature() (abi/c_abi.h), which dispatches on the target
// triple, rather than including this header outside the ABI layer or its
// tests.

#pragma once

#include "codegen/abi/c_abi.h"

namespace sun::abi::sysv {

/// Classify one type as it would be passed as a parameter.
ArgLowering lowerArgument(llvm::Type* type, const llvm::DataLayout& dl);

/// Classify one type as it would be returned.
ArgLowering lowerReturn(llvm::Type* type, const llvm::DataLayout& dl);

/// Classify a whole signature.
SignatureLowering lowerCSignature(llvm::Type* returnType,
                                  llvm::ArrayRef<llvm::Type*> paramTypes,
                                  const llvm::DataLayout& dl);

}  // namespace sun::abi::sysv
