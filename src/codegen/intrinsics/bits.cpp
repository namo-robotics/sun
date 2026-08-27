// src/codegen/intrinsics/bits.cpp - Bit-level intrinsics
//
// - _mul_hi_u64(a, b): high 64 bits of the 128-bit product
// - _ctlz_u64(x) / _cttz_u64(x): leading / trailing zero counts (64 for 0)
//
// The building blocks of table-driven float conversion (Ryu, Eisel-Lemire)
// and multi-limb integer arithmetic.

#include <llvm/IR/Intrinsics.h>

#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics_generator.h"
#include "support/error.h"

using namespace llvm;

Value* IntrinsicsGenerator::codegenMulHiU64Intrinsic(const CallExprAST& expr) {
  const auto& args = expr.getArgs();
  if (args.size() != 2) {
    logAndThrowError("_mul_hi_u64 expects 2 arguments: (a, b)");
    return nullptr;
  }
  Value* a = codegen(*args[0]);
  Value* b = codegen(*args[1]);
  if (!a || !b) return nullptr;

  auto* i64Ty = Type::getInt64Ty(ctx.getContext());
  auto* i128Ty = Type::getInt128Ty(ctx.getContext());
  a = ctx.builder->CreateZExtOrTrunc(a, i64Ty, "mulhi.a");
  b = ctx.builder->CreateZExtOrTrunc(b, i64Ty, "mulhi.b");
  Value* wideA = ctx.builder->CreateZExt(a, i128Ty, "mulhi.a.wide");
  Value* wideB = ctx.builder->CreateZExt(b, i128Ty, "mulhi.b.wide");
  Value* product = ctx.builder->CreateMul(wideA, wideB, "mulhi.prod");
  Value* shifted = ctx.builder->CreateLShr(
      product, ConstantInt::get(i128Ty, 64), "mulhi.shift");
  return ctx.builder->CreateTrunc(shifted, i64Ty, "mulhi");
}

Value* IntrinsicsGenerator::codegenCountZerosIntrinsic(const CallExprAST& expr,
                                                  bool leading) {
  const char* name = leading ? "_ctlz_u64" : "_cttz_u64";
  const auto& args = expr.getArgs();
  if (args.size() != 1) {
    logAndThrowError(std::string(name) + " expects 1 argument: (x)");
    return nullptr;
  }
  Value* x = codegen(*args[0]);
  if (!x) return nullptr;

  auto* i64Ty = Type::getInt64Ty(ctx.getContext());
  x = ctx.builder->CreateZExtOrTrunc(x, i64Ty, "ctz.x");
  Function* fn = Intrinsic::getOrInsertDeclaration(
      module, leading ? Intrinsic::ctlz : Intrinsic::cttz, {i64Ty});
  // is_zero_poison = false: 0 yields 64
  Value* isZeroPoison = ConstantInt::getFalse(ctx.getContext());
  return ctx.builder->CreateCall(fn, {x, isZeroPoison},
                                 leading ? "ctlz" : "cttz");
}
