// function_registry.cpp — Finding functions by name (see function_registry.h)
//
// The three lookups where the answer is not simply "the function with this
// name": a renamed extern, a class method under its mangled name, and a
// method the module has not been given yet.

#include "codegen/functions/function_registry.h"

using namespace llvm;

Function* FunctionRegistry::lookupCallTarget(const std::string& name) {
  // A renamed extern is declared under its C symbol, not the Sun-side name
  // the call site resolved to.
  const std::string& symbol = externC_.symbolFor(name);
  if (symbol != name) {
    if (Function* f = state_.module->getFunction(symbol)) return f;
  }
  return state_.module->getFunction(name);
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
