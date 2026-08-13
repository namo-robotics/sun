// abi/c_abi.h — Target-neutral C ABI lowering model and dispatch
//
// Sun's own calling convention passes aggregates as raw LLVM structs, which
// is self-consistent because both sides are Sun code. C is not: each psABI
// requires the *frontend* to decide how an aggregate travels, and LLVM
// performs no classification of its own. Handing clang's `void f(struct S)`
// an `{i32, i32}` by value produces a call the callee cannot decode.
//
// This header owns the target-independent pieces: the data model describing
// how one value crosses the boundary, the dispatch that picks a target's
// rules from the module's triple, and the translation of a lowering into an
// LLVM function type. The rules themselves are per-target modules
// (abi/sysv_x86_64.h, abi/aapcs64.h) that nothing outside the ABI layer
// should include directly — go through lowerCSignature().

#pragma once

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdint>
#include <vector>

namespace sun::abi {

enum class ArgKind {
  // Passed unchanged: scalars, pointers, and anything already ABI-correct.
  Direct,
  // Aggregate small enough for registers, rewritten as one or more scalar
  // pieces, each becoming its own LLVM argument. SysV uses up to two
  // "eightbytes"; AAPCS64 always one piece, possibly an array like [2 x i64].
  Coerced,
  // Aggregate passed in memory through a pointer. Whether that pointer
  // carries `byval` is the target's call (indirectByval below).
  Indirect,
};

struct ArgLowering {
  ArgKind kind = ArgKind::Direct;
  // Coerced: the register piece types (empty for a zero-sized aggregate,
  // which C passes as nothing).
  llvm::SmallVector<llvm::Type*, 2> pieces;
  // Coerced: byte offset of each piece within the aggregate, parallel to
  // `pieces`. SysV eightbytes sit at {0, 8}; AAPCS64's single piece at {0}.
  llvm::SmallVector<uint64_t, 2> pieceOffsets;
  // Direct: the type itself. Indirect: the aggregate being pointed at.
  llvm::Type* type = nullptr;
  // Indirect: alignment for the byval/sret attribute.
  uint64_t align = 1;
  // Indirect parameter: whether the pointer takes the `byval` attribute.
  // SysV passes the copy in the caller's argument area (byval); AAPCS64
  // passes a plain pointer to a caller-made copy.
  bool indirectByval = true;
  // Coerced: nonzero -> each piece takes alignstack(N), which AAPCS64
  // requires on HFA arguments so stack spills stay 8-byte aligned.
  uint64_t stackAlign = 0;

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

/// Classify a whole signature under the C ABI of `triple`. Throws a
/// compilation error for targets without implemented rules.
SignatureLowering lowerCSignature(const llvm::Triple& triple,
                                  llvm::Type* returnType,
                                  llvm::ArrayRef<llvm::Type*> paramTypes,
                                  const llvm::DataLayout& dl);

/// The LLVM function type implied by a lowering: sret pointer prepended when
/// needed, coerced parameters expanded in place, indirect ones as pointers.
llvm::FunctionType* buildLoweredFunctionType(const SignatureLowering& lowering,
                                             llvm::LLVMContext& ctx,
                                             bool isVarArg);

}  // namespace sun::abi
