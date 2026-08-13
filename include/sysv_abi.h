// sysv_abi.h — System V AMD64 argument classification for the C boundary
//
// Sun's own calling convention passes aggregates as raw LLVM structs, which
// is self-consistent because both sides are Sun code. C is not: the psABI
// requires the *frontend* to decide how each aggregate travels, and LLVM
// performs no classification of its own. Handing clang's `void f(struct S)`
// an `{i32, i32}` by value produces a call the callee cannot decode.
//
// What the ABI actually asks for, and what this header reproduces:
//
//   struct { int, int }        -> one i64 argument
//   struct { int, long }       -> two arguments: i32, i64
//   struct { double, double }  -> two arguments: double, double
//   struct { int, double }     -> two arguments: i32, double
//   struct of 20 bytes         -> one pointer argument marked `byval`
//   returning that 20-byte struct -> a prepended `sret` pointer, returns void
//
// Only aggregates need this; scalars and pointers already map one-to-one.
// This is x86-64 SysV specifically — AArch64 classifies differently, so keep
// callers going through lowerCSignature() rather than assuming these rules.

#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

#include <cstdint>
#include <vector>

namespace sun::sysv {

enum class ArgKind {
  // Passed unchanged: scalars, pointers, and anything already ABI-correct.
  Direct,
  // Aggregate small enough for registers, rewritten as one or two scalar
  // "eightbyte" pieces. As a parameter each piece becomes its own LLVM
  // argument; as a return value two pieces become an anonymous struct.
  Coerced,
  // Aggregate passed in memory: a pointer marked `byval` (parameter) or
  // `sret` (return value).
  Indirect,
};

struct ArgLowering {
  ArgKind kind = ArgKind::Direct;
  // Coerced: the eightbyte types (1 or 2 entries; empty for a zero-sized
  // aggregate, which C passes as nothing).
  llvm::SmallVector<llvm::Type*, 2> pieces;
  // Direct: the type itself. Indirect: the aggregate being pointed at.
  llvm::Type* type = nullptr;
  // Indirect: alignment for the byval/sret attribute.
  uint64_t align = 1;

  bool isDirect() const { return kind == ArgKind::Direct; }
  bool isCoerced() const { return kind == ArgKind::Coerced; }
  bool isIndirect() const { return kind == ArgKind::Indirect; }
};

struct SignatureLowering {
  ArgLowering ret;
  std::vector<ArgLowering> params;

  // True when the return value travels through a caller-allocated buffer
  // passed as a prepended pointer argument.
  bool usesSret() const { return ret.isIndirect(); }

  // True when nothing needed rewriting, so the naive signature is already
  // correct and callers can take their existing fast path.
  bool isTrivial() const {
    if (!ret.isDirect()) return false;
    for (const auto& p : params)
      if (!p.isDirect()) return false;
    return true;
  }
};

/// Classify one type as it would be passed as a parameter.
ArgLowering lowerArgument(llvm::Type* type, const llvm::DataLayout& dl);

/// Classify one type as it would be returned.
ArgLowering lowerReturn(llvm::Type* type, const llvm::DataLayout& dl);

/// Classify a whole signature.
SignatureLowering lowerCSignature(llvm::Type* returnType,
                                  llvm::ArrayRef<llvm::Type*> paramTypes,
                                  const llvm::DataLayout& dl);

/// The LLVM function type implied by a lowering: sret pointer prepended when
/// needed, coerced parameters expanded in place, indirect ones as pointers.
llvm::FunctionType* buildLoweredFunctionType(const SignatureLowering& lowering,
                                             llvm::LLVMContext& ctx,
                                             bool isVarArg);

}  // namespace sun::sysv
