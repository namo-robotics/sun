// abi/aapcs64.h — AArch64 AAPCS64 (ELF) argument classification
//
// What the AAPCS64 asks for, and what this module reproduces (all verified
// against clang's output for aarch64-linux-gnu):
//
//   struct { int, int }          -> one i64 argument
//   struct { int, int, int }     -> one [2 x i64] argument
//   struct { float, float }      -> one [2 x float] argument (HFA)
//   struct { double x 4 }        -> one [4 x double] argument (HFA, exempt
//                                   from the 16-byte limit)
//   struct of 20 bytes           -> a plain pointer to a caller-made copy
//                                   (no byval, unlike x86-64)
//   returning that 20-byte struct -> a prepended `sret` pointer (x8)
//
// Two asymmetries worth knowing: small non-HFA arguments round up to full
// i64 registers, but *returns* use the exact bit width (a 3-byte struct
// returns i24); and HFA returns stay the literal struct type (Direct) while
// HFA arguments coerce to [N x float/double].
//
// These are aarch64 ELF rules specifically (Darwin deviates) — reach them
// through abi::lowerCSignature() (abi/c_abi.h), which dispatches on the
// target triple, rather than including this header outside the ABI layer or
// its tests.

#pragma once

#include "abi/c_abi.h"

namespace sun::abi::aapcs64 {

/// Classify one type as it would be passed as a parameter.
ArgLowering lowerArgument(llvm::Type* type, const llvm::DataLayout& dl);

/// Classify one type as it would be returned.
ArgLowering lowerReturn(llvm::Type* type, const llvm::DataLayout& dl);

/// Classify a whole signature.
SignatureLowering lowerCSignature(llvm::Type* returnType,
                                  llvm::ArrayRef<llvm::Type*> paramTypes,
                                  const llvm::DataLayout& dl);

}  // namespace sun::abi::aapcs64
