/** Lowers fixed-width float operations while keeping vectors internal to LLVM.
 */
#include "codegen/codegen_visitor.h"
#include "codegen/intrinsics/intrinsics_generator.h"
#include "support/error.h"

/** Implements compiler-provided operations. */
namespace sun::codegen::intrinsics {
llvm::Value* IntrinsicsGenerator::codegenSimdF32x4(
    const CallExprAST& expr, const std::string& operation) {
  const bool binary =
      operation == "add" || operation == "sub" || operation == "mul";
  if (expr.getArgs().size() != (binary ? 3 : 2))
    sun::support::logAndThrowError("Invalid argument count for SIMD intrinsic");
  auto* output = codegen(*expr.getArgs()[0]);
  auto* input = codegen(*expr.getArgs()[1]);
  auto* right = binary ? codegen(*expr.getArgs()[2]) : nullptr;
  llvm::IRBuilderBase::FastMathFlagGuard flags(*ctx.builder);
  ctx.builder->clearFastMathFlags();
  auto* vectorType =
      llvm::FixedVectorType::get(llvm::Type::getFloatTy(ctx.getContext()), 4);
  llvm::Value* result;
  if (operation == "splat") {
    result = ctx.builder->CreateVectorSplat(4, input, "simd.splat");
  } else {
    auto* a = ctx.builder->CreateAlignedLoad(vectorType, input, llvm::Align(4),
                                             "simd.a");
    result = a;
    if (binary) {
      auto* b = ctx.builder->CreateAlignedLoad(vectorType, right,
                                               llvm::Align(4), "simd.b");
      if (operation == "add")
        result = ctx.builder->CreateFAdd(a, b, "simd.add");
      if (operation == "sub")
        result = ctx.builder->CreateFSub(a, b, "simd.sub");
      if (operation == "mul")
        result = ctx.builder->CreateFMul(a, b, "simd.mul");
    }
  }
  return ctx.builder->CreateAlignedStore(result, output, llvm::Align(4));
}
}  // namespace sun::codegen::intrinsics
