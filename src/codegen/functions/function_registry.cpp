// Function declarations map directly to their LLVM functions.

#include "codegen/functions/function_registry.h"

using namespace llvm;

void FunctionRegistry::registerFunction(sun::DeclarationId declaration,
                                        Function* function) {
  const auto& record = state_.typeRegistry->declarations.get(declaration);
  if (record.kind != sun::DeclarationKind::Function &&
      record.kind != sun::DeclarationKind::Lambda)
    logAndThrowError("Function registration requires a callable declaration");
  if (!function) logAndThrowError("Cannot register a missing LLVM function");
  auto& existing = functionsById_[declaration];
  if (existing && existing != function)
    logAndThrowError("Declaration already has a different LLVM function");
  existing = function;
}

Function* FunctionRegistry::lookupFunctionById(sun::DeclarationId declaration) {
  auto found = functionsById_.find(declaration);
  if (found == functionsById_.end() || !found->second)
    logAndThrowError("Resolved function declaration has not been emitted");
  return llvm::cast<Function>(static_cast<llvm::Value*>(found->second));
}

Function* FunctionRegistry::findClassMethod(
    const std::shared_ptr<sun::ClassType>& classType,
    const std::string& typeName, const std::string& methodName) {
  if (classType) {
    if (auto* m = classType->getMethod(methodName)) {
      std::string mangled =
          classType->getMangledMethodName(methodName, m->paramTypes);
      if (auto* f = state_.module->getFunction(mangled)) return f;
    }
  }
  // Fallback: try without paramSuffix
  return state_.module->getFunction(typeName + "_" + methodName);
}

Function* FunctionRegistry::getOrDeclareMethodFunction(
    const std::string& mangledName, const std::vector<sun::TypePtr>& paramTypes,
    const sun::TypePtr& returnType, bool canThrow) {
  if (Function* existing = state_.module->getFunction(mangledName))
    return existing;

  std::vector<llvm::Type*> llvmParams;
  llvmParams.push_back(
      PointerType::getUnqual(state_.ctx.getContext()));  // closure
  for (const auto& pt : paramTypes) {
    llvmParams.push_back(state_.typeResolver.resolve(pt));
  }
  llvm::Type* retTy = returnType
                          ? state_.typeResolver.resolveForReturn(returnType)
                          : llvm::Type::getVoidTy(state_.ctx.getContext());
  FunctionType* funcType = FunctionType::get(retTy, llvmParams, false);
  Function* func = Function::Create(funcType, Function::ExternalLinkage,
                                    mangledName, state_.module);
  if (canThrow) {
    func->addFnAttr("sun.canthrow");
  }
  return func;
}
