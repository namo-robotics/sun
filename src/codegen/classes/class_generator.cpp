// class_generator.cpp — How class codegen reaches the rest of codegen
//
// Forwarders that cannot be inline in the header, because they need the full
// definition of CodegenVisitor and the header is included by it.

#include "codegen/classes/class_generator.h"

#include "codegen/codegen_visitor.h"

llvm::Value* ClassGenerator::codegen(const ExprAST& expr) {
  return gen_.codegen(expr);
}

llvm::Value* ClassGenerator::codegen(const BlockExprAST& block) {
  return gen_.codegen(block);
}

ScopeManager& ClassGenerator::scopes() { return gen_.scopeManager(); }

FunctionRegistry& ClassGenerator::functions() {
  return gen_.functionRegistry();
}

IntrinsicsGenerator& ClassGenerator::intrinsics() {
  return gen_.intrinsicsGenerator();
}

llvm::AllocaInst* ClassGenerator::createEntryBlockAlloca(llvm::Function* func,
                                                         llvm::StringRef name,
                                                         llvm::Type* type) {
  return gen_.createEntryBlockAlloca(func, name, type);
}

void ClassGenerator::debugDeclareParam(llvm::AllocaInst* alloca,
                                       const std::string& name,
                                       const PrototypeAST& proto,
                                       unsigned userArgIdx,
                                       unsigned argNoBase) {
  gen_.debugDeclareParam(alloca, name, proto, userArgIdx, argNoBase);
}

llvm::Value* ClassGenerator::codegenEnumVariantAccess(
    sun::EnumType& enumType, const sun::EnumVariant& variant) {
  return gen_.codegenEnumVariantAccess(enumType, variant);
}

llvm::Value* ClassGenerator::codegenArrayShape(const MemberAccessAST& expr) {
  return gen_.codegenArrayShape(expr);
}

std::pair<llvm::Value*, sun::ClassType*> ClassGenerator::codegenObjectPtr(
    const ExprAST& object) {
  return gen_.codegenObjectPtr(object);
}

llvm::Value* ClassGenerator::materializeMethodClosure(llvm::Value* fnPtr,
                                                      llvm::Value* receiverPtr,
                                                      llvm::StringRef name) {
  return gen_.materializeMethodClosure(fnPtr, receiverPtr, name);
}

llvm::Value* ClassGenerator::materializeMethodClosureValue(
    llvm::Value* fnPtr, llvm::Value* receiverPtr) {
  return gen_.materializeMethodClosureValue(fnPtr, receiverPtr);
}

bool ClassGenerator::emitCallArguments(
    const std::vector<std::unique_ptr<ExprAST>>& args,
    const std::vector<sun::ArgConversion>& conversions,
    const std::vector<sun::TypePtr>& paramTypes, llvm::FunctionType* calleeTy,
    std::vector<llvm::Value*>& argValues, const std::string& calleeName,
    size_t firstArg) {
  return gen_.emitCallArguments(args, conversions, paramTypes, calleeTy,
                                argValues, calleeName, firstArg);
}

void ClassGenerator::assignToVariableSlot(llvm::Value* slot, llvm::Value* value,
                                          const sun::TypePtr& varType,
                                          const std::string& name) {
  gen_.assignToVariableSlot(slot, value, varType, name);
}

bool ClassGenerator::isPrecompiledFunction(const std::string& name) {
  return gen_.isPrecompiledFunction(name);
}
