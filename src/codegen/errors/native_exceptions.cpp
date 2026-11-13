/** Lowers source exceptions to bounded storage and table-based native unwinding. */
#include "abi/exception_layout.h"
#include "abi/lowering_descriptors.h"
#include "codegen/codegen_visitor.h"
#include "codegen/errors/error_generator.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/TargetParser/Triple.h"
#include "semantic_analysis/portable_declaration_key.h"

/** Generates exception descriptors, transfer operations, and handler tables. */
namespace sun::codegen::errors {
using namespace llvm;
using sun::support::logAndThrowError;
/** Names the target-independent layout contract shared with the native runtime. */
namespace layout = sun::abi::exceptions;

void ErrorGenerator::emitExceptionTermination() {
  auto& builder = *ctx.builder;
  auto* call = builder.CreateCall(
      module->getOrInsertFunction("sun_eh_terminate", builder.getVoidTy()));
  call->setDoesNotReturn();
  call->setDoesNotThrow();
  builder.CreateUnreachable();
}

void ErrorGenerator::emitDestructorCall(Function* callee, Value* receiver) {
  auto& builder = *ctx.builder;
  auto* function = builder.GetInsertBlock()->getParent();
  ensurePersonality(function);
  auto* done = BasicBlock::Create(ctx.getContext(), "deinit.done", function);
  auto* failed = BasicBlock::Create(ctx.getContext(), "deinit.escaped", function);
  auto* call = builder.CreateInvoke(callee, done, failed, {receiver});
  sun::abi::CallDescriptor::forCall(callee->getFunctionType(), callee).apply(*call);
  builder.SetInsertPoint(failed);
  auto* pad = builder.CreateLandingPad(StructType::get(builder.getPtrTy(), builder.getInt32Ty()), 1);
  pad->addClause(ConstantPointerNull::get(builder.getPtrTy()));
  emitExceptionTermination();
  builder.SetInsertPoint(done);
}

AllocaInst* ErrorGenerator::exceptionStorage(std::size_t size, const Twine& name) {
  auto* function = ctx.builder->GetInsertBlock()->getParent();
  auto* storage = gen_.createEntryBlockAlloca(function, name.str(),
      ArrayType::get(ctx.builder->getInt8Ty(), size));
  storage->setAlignment(Align(layout::alignment));
  return storage;
}

Constant* ErrorGenerator::exceptionDescriptor(const sun::types::ClassType& type) {
  const Triple target(Triple::normalize(module->getTargetTriple()));
  if ((target.getArch() != Triple::x86_64 && target.getArch() != Triple::aarch64) ||
      (!target.isOSDarwin() && !target.isOSLinux()))
    logAndThrowError("Native exceptions require a supported 64-bit Linux or Darwin target");
  auto* payload = state_.typeResolver.resolve(type);
  const auto& data = module->getDataLayout();
  auto size = data.getTypeAllocSize(payload).getFixedValue();
  auto alignment = data.getABITypeAlign(payload).value();
  if (size > layout::payloadCapacity || alignment > layout::alignment)
    logAndThrowError("Exception '" + type.getDisplayName() + "' exceeds the bounded payload layout");
  auto identity = sun::semantic_analysis::PortableDeclarationKey::fromDeclaration(
      type.getDeclarationId(), state_.analysis->declarations).encoding();
  identity += ":sun-exception-v1:" + std::to_string(size) + ":" + std::to_string(alignment);
  for (const auto& field : type.getFields())
    identity += ":" + field.name + ":" + field.type->toString();
  auto name = "sun.exception." + identity;
  if (auto* existing = module->getNamedGlobal(name)) return existing;
  auto* ptr = ctx.builder->getPtrTy();
  auto* descriptorType = StructType::get(ptr, ptr, ctx.builder->getInt32Ty(), ctx.builder->getInt32Ty());
  Constant* parent = ConstantPointerNull::get(ptr);
  if (type.getExceptionParent()) parent = exceptionDescriptor(*type.getExceptionParent());
  auto* descriptor = new GlobalVariable(*module, descriptorType, true,
      GlobalValue::PrivateLinkage,
      ConstantStruct::get(descriptorType, {
          ctx.builder->CreateGlobalString(identity, "exception.identity"), parent,
          ctx.builder->getInt32(size), ctx.builder->getInt32(alignment)}), name);
  return descriptor;
}

void ErrorGenerator::emitNativeCleanup(std::size_t depth) {
  if (!scopes().hasLiveOwners(depth)) return;
  auto* nested = exceptionStorage(layout::recordSize, "cleanup.exception");
  auto push = module->getOrInsertFunction("sun_eh_push", ctx.builder->getPtrTy(), ctx.builder->getPtrTy());
  auto pop = module->getOrInsertFunction("sun_eh_pop", ctx.builder->getVoidTy(), ctx.builder->getPtrTy());
  auto* previous = ctx.builder->CreateCall(push, {nested});
  scopes().emitCleanupToDepth(depth, true);
  ctx.builder->CreateCall(pop, {previous});
}

Value* ErrorGenerator::emitNativeCall(FunctionType* type, Value* callee,
                                     ArrayRef<Value*> args, const Twine& name) {
  auto& builder = *ctx.builder;
  auto* function = builder.GetInsertBlock()->getParent();
  std::vector<NativeHandler> handlers;
  for (auto handler = nativeHandlers_.rbegin(); handler != nativeHandlers_.rend(); ++handler) {
    if (handler->function != function) continue;
    handlers.push_back(*handler);
    // An inner catch-all handles every exception before outer handlers can match.
    if (handler->type->isNullValue()) break;
  }
  const auto depth = scopes().functionBoundaryDepth();
  const bool canEscape = state_.frame.canError;
  const auto callName = type->getReturnType()->isVoidTy() ? Twine("") : name;
  const auto abi = sun::abi::CallDescriptor::forCall(type, callee);
  if (handlers.empty() && !scopes().hasLiveOwners(depth) && canEscape) {
    auto* call = builder.CreateCall(type, callee, args, callName);
    abi.apply(*call);
    return call;
  }
  ensurePersonality(function);
  auto* continuation = BasicBlock::Create(ctx.getContext(), "invoke.cont", function);
  auto* unwind = BasicBlock::Create(ctx.getContext(), "exception.pad", function);
  auto* invoke = builder.CreateInvoke(type, callee, continuation, unwind, args, callName);
  abi.apply(*invoke);
  builder.SetInsertPoint(unwind);
  auto* resultType = StructType::get(builder.getPtrTy(), builder.getInt32Ty());
  auto* pad = builder.CreateLandingPad(resultType, handlers.size() + 1);
  pad->setCleanup(true);
  for (const auto& handler : handlers) pad->addClause(handler.type);
  if (!canEscape) pad->addClause(ConstantPointerNull::get(builder.getPtrTy()));
  auto* exception = builder.CreateExtractValue(pad, 0);
  auto* selector = builder.CreateExtractValue(pad, 1);
  for (const auto& handler : handlers) {
    auto* selected = BasicBlock::Create(ctx.getContext(), "catch.cleanup", function);
    auto* next = BasicBlock::Create(ctx.getContext(), "catch.next", function);
    auto* id = builder.CreateCall(Intrinsic::getOrInsertDeclaration(
        module, Intrinsic::eh_typeid_for, {builder.getPtrTy()}), {handler.type});
    builder.CreateCondBr(builder.CreateICmpEQ(selector, id), selected, next);
    builder.SetInsertPoint(selected);
    emitNativeCleanup(handler.depth);
    handler.exception->addIncoming(exception, builder.GetInsertBlock());
    builder.CreateBr(handler.block);
    builder.SetInsertPoint(next);
  }
  if (canEscape) {
    emitNativeCleanup(depth);
    builder.CreateResume(pad);
  } else {
    emitExceptionTermination();
  }
  builder.SetInsertPoint(continuation);
  return invoke;
}

Value* ErrorGenerator::codegen(const sun::ast::ThrowAST& statement) {
  auto& builder = *ctx.builder;
  auto* function = builder.GetInsertBlock()->getParent();
  const auto* operand = statement.value.get();
  while (operand && operand->getType() == sun::ast::ASTNodeType::PAREN_EXPR)
    operand = static_cast<const sun::ast::ParenExprAST*>(operand)->getInner();
  Value* value = nullptr;
  for (auto active = nativeCatches_.rbegin(); active != nativeCatches_.rend(); ++active) {
    if (active->function != function) continue;
    if (!operand || (operand->getType() == sun::ast::ASTNodeType::VARIABLE_REFERENCE &&
        operand->getTargetDeclarationId() == active->binding)) {
      value = active->record;
      break;
    }
  }
  if (!value) {
    if (!statement.value) logAndThrowError("No active exception to rethrow", statement.getLocation());
    auto type = std::static_pointer_cast<sun::types::ClassType>(statement.value->getResolvedType());
    auto* descriptor = exceptionDescriptor(*type);
    auto* source = codegen(*statement.value);
    value = exceptionStorage(layout::valueSize, "thrown.exception");
    builder.CreateStore(descriptor, value);
    auto* destination = builder.CreateConstGEP1_64(builder.getInt8Ty(), value, layout::payloadOffset);
    const auto size = module->getDataLayout().getTypeAllocSize(state_.typeResolver.resolve(type));
    builder.CreateMemCpy(destination, Align(layout::alignment), source, MaybeAlign(), size);
  }
  // Storage is statically reserved. Initialization only selects it on first use.
  builder.CreateCall(module->getOrInsertFunction("sun_eh_initialize", builder.getVoidTy()));
  auto raise = module->getOrInsertFunction("sun_eh_raise", builder.getVoidTy(), builder.getPtrTy());
  emitPossiblyThrowingCall(raise, {value}, true, "");
  builder.CreateUnreachable();
  return nullptr;
}

Value* ErrorGenerator::codegen(const sun::ast::TryCatchAST& statement) {
  auto& builder = *ctx.builder;
  auto* function = builder.GetInsertBlock()->getParent();
  auto* continuation = BasicBlock::Create(ctx.getContext(), "try.end", function);
  std::vector<NativeHandler> handlers;
  for (const auto& source : statement.handlers) {
    Constant* type = ConstantPointerNull::get(builder.getPtrTy());
    if (source.binding)
      type = exceptionDescriptor(*std::static_pointer_cast<sun::types::ClassType>(source.binding->getResolvedType()));
    auto* block = BasicBlock::Create(ctx.getContext(), "catch.body", function);
    auto* exception = PHINode::Create(builder.getPtrTy(), 0, "caught.record", block);
    handlers.push_back({function, type, block, exception, scopes().size()});
  }
  const auto saved = nativeHandlers_.size();
  // The dispatch stack is visited in reverse, preserving source order here.
  for (auto handler = handlers.rbegin(); handler != handlers.rend(); ++handler)
    nativeHandlers_.push_back(*handler);
  scopes().push(statement.body->getLocation());
  codegen(*statement.body);
  scopes().pop();
  if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(continuation);
  nativeHandlers_.resize(saved);
  for (std::size_t i = 0; i < handlers.size(); ++i) {
    const auto& source = statement.handlers[i];
    const auto& handler = handlers[i];
    builder.SetInsertPoint(handler.block);
    auto* caught = exceptionStorage(layout::valueSize, "caught.exception");
    auto take = module->getOrInsertFunction("sun_eh_take", builder.getVoidTy(), builder.getPtrTy(), builder.getPtrTy());
    builder.CreateCall(take, {handler.exception, caught});
    scopes().push(source.body->getLocation());
    sun::semantic_analysis::DeclarationId binding;
    if (source.binding) {
      binding = source.binding->getDeclarationId();
      auto* slot = gen_.createEntryBlockAlloca(function, "catch.binding", builder.getPtrTy());
      builder.CreateStore(builder.CreateConstGEP1_64(builder.getInt8Ty(), caught, layout::payloadOffset), slot);
      scopes().back().variables[binding] = slot;
      scopes().back().indirectBindings.insert(binding);
    }
    nativeCatches_.push_back({function, binding, caught});
    codegen(*source.body);
    nativeCatches_.pop_back();
    scopes().pop();
    if (!builder.GetInsertBlock()->getTerminator()) builder.CreateBr(continuation);
  }
  builder.SetInsertPoint(continuation);
  if (pred_empty(continuation)) builder.CreateUnreachable();
  return builder.getInt32(0);
}
}  // namespace sun::codegen::errors
