/** Lowers source exceptions to bounded storage and table-based native
 * unwinding. */
#include <unordered_set>

#include "abi/exception_layout.h"
#include "abi/lowering_descriptors.h"
#include "codegen/codegen_visitor.h"
#include "codegen/exceptions/exception_generator.h"
#include "codegen/exceptions/storage_types.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/TargetParser/Triple.h"
#include "semantic_analysis/portable_declaration_key.h"

/** Generates exception descriptors, transfer operations, and handler tables. */
namespace sun::codegen::exceptions {
using namespace llvm;
using sun::support::logAndThrowError;
/** Names the target-independent layout contract shared with the native runtime.
 */
namespace layout = sun::abi::exceptions;

/** Records the source spelling of a throw for the compiler's address index. */
static void recordThrowSource(Module& module,
                              const sun::support::Position& location,
                              const std::string& typeName) {
  auto& context = module.getContext();
  auto* line = ConstantInt::get(Type::getInt32Ty(context), location.line);
  auto* site = MDNode::get(
      context, {MDString::get(context, location.filePath.value_or("")),
                ConstantAsMetadata::get(line),
                MDString::get(context, "throw " + typeName + "()")});
  module.getOrInsertNamedMetadata("sun.trace.throws")->addOperand(site);
}

void ExceptionGenerator::emitExceptionTermination(Value* exception) {
  auto& builder = *ctx.builder;
  auto* call = builder.CreateCall(
      module->getOrInsertFunction("sun_eh_terminate_exception",
                                  builder.getVoidTy(), builder.getPtrTy()),
      {exception});
  call->setDoesNotReturn();
  call->setDoesNotThrow();
  builder.CreateUnreachable();
}

void ExceptionGenerator::emitDestructorCall(Function* callee, Value* receiver) {
  auto& builder = *ctx.builder;
  auto* function = builder.GetInsertBlock()->getParent();
  ensurePersonality(function);
  auto* done = BasicBlock::Create(ctx.getContext(), "deinit.done", function);
  auto* failed =
      BasicBlock::Create(ctx.getContext(), "deinit.escaped", function);
  auto* call = builder.CreateInvoke(callee, done, failed, {receiver});
  sun::abi::CallDescriptor::forCall(callee->getFunctionType(), callee)
      .apply(*call);
  builder.SetInsertPoint(failed);
  auto* pad = builder.CreateLandingPad(
      StructType::get(builder.getPtrTy(), builder.getInt32Ty()), 1);
  pad->addClause(ConstantPointerNull::get(builder.getPtrTy()));
  emitExceptionTermination(builder.CreateExtractValue(pad, 0));
  builder.SetInsertPoint(done);
}

AllocaInst* ExceptionGenerator::exceptionStorage(Type* type,
                                                 const Twine& name) {
  auto* function = ctx.builder->GetInsertBlock()->getParent();
  auto* storage = gen_.createEntryBlockAlloca(function, name.str(), type);
  storage->setAlignment(Align(layout::alignment));
  return storage;
}

Constant* ExceptionGenerator::exceptionDescriptor(
    const sun::types::ClassType& type) {
  const Triple target(Triple::normalize(module->getTargetTriple()));
  if ((target.getArch() != Triple::x86_64 &&
       target.getArch() != Triple::aarch64) ||
      (!target.isOSDarwin() && !target.isOSLinux())) {
    logAndThrowError(
        "Native exceptions require a supported 64-bit Linux or Darwin target");
  }
  auto* payload = state_.typeResolver.resolve(type);
  const auto& data = module->getDataLayout();
  auto size = data.getTypeAllocSize(payload).getFixedValue();
  auto alignment = data.getABITypeAlign(payload).value();
  if (size > layout::payloadCapacity || alignment > layout::alignment) {
    logAndThrowError("Exception '" + type.getDisplayName() +
                     "' exceeds the bounded payload layout");
  }
  auto identity =
      sun::semantic_analysis::PortableDeclarationKey::fromDeclaration(
          type.getDeclarationId(), state_.analysis->declarations)
          .encoding();
  identity += ":sun-exception-v2:" + std::to_string(size) + ":" +
              std::to_string(alignment);
  for (const auto& field : type.getFields()) {
    identity += ":" + field.name + ":" + field.type->toString();
  }
  auto name = "sun.exception." + identity;
  if (auto* existing = module->getNamedGlobal(name)) {
    return existing;
  }
  auto* ptr = ctx.builder->getPtrTy();
  auto* descriptorType = StructType::get(ptr, ptr, ctx.builder->getInt32Ty(),
                                         ctx.builder->getInt32Ty(), ptr);
  Constant* parent = ConstantPointerNull::get(ptr);
  if (type.getExceptionParent()) {
    parent = exceptionDescriptor(*type.getExceptionParent());
  }
  auto* descriptor = new GlobalVariable(
      *module, descriptorType, true, GlobalValue::PrivateLinkage,
      ConstantStruct::get(
          descriptorType,
          {ctx.builder->CreateGlobalString(identity, "exception.identity"),
           parent, ctx.builder->getInt32(size),
           ctx.builder->getInt32(alignment),
           ctx.builder->CreateGlobalString(type.getDisplayName(),
                                           "exception.name")}),
      name);
  return descriptor;
}

void ExceptionGenerator::emitNativeCleanup(std::size_t depth) {
  scopes().emitCleanupToDepth(depth, true);
}

void ExceptionGenerator::protectDestructor(Function* function) {
  function->addFnAttr("sun.destructor");
  IRBuilderBase::InsertPointGuard guard(*ctx.builder);
  auto& builder = *ctx.builder;
  builder.SetInsertPoint(&function->getEntryBlock(),
                         function->getEntryBlock().begin());
  auto* nested = builder.CreateAlloca(
      storageTypes(*module, ctx.exceptionTraceDepth).record, nullptr,
      "destructor.exception");
  nested->setAlignment(Align(layout::alignment));
  auto push = module->getOrInsertFunction("sun_eh_push", builder.getPtrTy(),
                                          builder.getPtrTy());
  auto pop = module->getOrInsertFunction("sun_eh_pop", builder.getVoidTy(),
                                         builder.getPtrTy());
  auto* previous = builder.CreateCall(push, {nested});
  // Return instructions follow generated cleanup, including cleanup for early
  // returns.
  for (auto& block : *function) {
    if (auto* result = dyn_cast_or_null<ReturnInst>(block.getTerminator())) {
      builder.SetInsertPoint(result);
      builder.SetCurrentDebugLocation(result->getDebugLoc());
      builder.CreateCall(pop, {previous});
    }
  }
}

void ExceptionGenerator::omitInfallibleDestructorStorage() {
  std::unordered_set<const Function*> initiating;
  bool changed;
  do {
    changed = false;
    for (const auto& function : *module) {
      if (initiating.contains(&function)) {
        continue;
      }
      bool mayInitiate = function.isDeclaration();
      for (const auto& block : function) {
        for (const auto& instruction : block) {
          const auto* call = dyn_cast<CallBase>(&instruction);
          if (!call) {
            continue;
          }
          const auto* callee = call->getCalledFunction();
          if (callee && (callee->isIntrinsic() ||
                         (callee->isDeclaration() &&
                          (callee->getName() == "sun_eh_push" ||
                           callee->getName() == "sun_eh_pop" ||
                           callee->getName() == "sun_eh_take" ||
                           callee->getName() == "sun_eh_report" ||
                           callee->getName() == "sun_eh_initialize" ||
                           callee->getName() == "sun_eh_print_trace" ||
                           callee->getName() == "sun_eh_terminate_exception" ||
                           callee->getName() == "sun_eh_terminate")))) {
            continue;
          }
          if (!callee || callee->isDeclaration() ||
              initiating.contains(callee)) {
            mayInitiate = true;
          }
        }
      }
      if (mayInitiate) {
        changed |= initiating.insert(&function).second;
      }
    }
  } while (changed);
  for (auto& function : *module) {
    if (!function.hasFnAttribute("sun.destructor") ||
        initiating.contains(&function)) {
      continue;
    }
    SmallVector<CallInst*> pushes;
    for (auto& block : function) {
      for (auto& instruction : block) {
        auto* call = dyn_cast<CallInst>(&instruction);
        if (!call || !call->getCalledFunction()) {
          continue;
        }
        if (call->getCalledFunction()->getName() == "sun_eh_push") {
          pushes.push_back(call);
        }
      }
    }
    // Imported optimized bodies can contain several inlined destructor records.
    // Remove each selection only with the restorations that use its saved
    // target.
    for (auto* push : pushes) {
      SmallVector<CallInst*> pops;
      bool paired = true;
      for (auto* user : push->users()) {
        auto* pop = dyn_cast<CallInst>(user);
        if (!pop || !pop->getCalledFunction() ||
            pop->getCalledFunction()->getName() != "sun_eh_pop" ||
            pop->arg_size() != 1 || pop->getArgOperand(0) != push) {
          paired = false;
          break;
        }
        pops.push_back(pop);
      }
      if (!paired) {
        continue;
      }
      auto* storage = dyn_cast<AllocaInst>(push->getArgOperand(0));
      for (auto* pop : pops) {
        pop->eraseFromParent();
      }
      push->eraseFromParent();
      if (storage && storage->use_empty()) {
        storage->eraseFromParent();
      }
    }
  }
}

std::optional<Value*> ExceptionGenerator::emitTraceOperation(
    const sun::ast::ExprAST& call,
    const std::vector<std::unique_ptr<sun::ast::ExprAST>>& arguments) {
  const auto* ordinary = dynamic_cast<const sun::ast::CallExprAST*>(&call);
  std::string name;
  if (ordinary) {
    const auto* callee = dynamic_cast<const sun::ast::VariableReferenceAST*>(
        ordinary->getCallee());
    if (!callee || callee->getTargetDeclarationId()) {
      return std::nullopt;
    }
    name = callee->getName();
  } else if (const auto* generic =
                 dynamic_cast<const sun::ast::GenericCallAST*>(&call)) {
    if (generic->getTargetDeclarationId()) {
      return std::nullopt;
    }
    name = generic->getFunctionName();
  }
  if (name == "_report_stack_trace") {
    auto& builder = *ctx.builder;
    state_.debugInfo.attachExpressionLocation(builder, call.getLocation());
    auto* report = builder.CreateCall(module->getOrInsertFunction(
        "sun_report_stack_trace", builder.getVoidTy()));
    report->setDoesNotThrow();
    report->setTailCallKind(CallInst::TCK_NoTail);
    return report;
  }
  if (name == "_report_exception" || name == "_report_unhandled_exception") {
    return reportCurrentException(call.getLocation(),
                                  name == "_report_unhandled_exception");
  }
  const bool framesOnly = name == "_exception_trace_frames";
  const bool statusOnly = name == "_exception_trace_status";
  if (!framesOnly && !statusOnly) {
    return std::nullopt;
  }
  assert(arguments.size() == 1 && "Intrinsic signature was not validated");
  auto& builder = *ctx.builder;
  auto* function = builder.GetInsertBlock()->getParent();
  const auto* operand = arguments.front().get();
  while (operand->getType() == sun::ast::ASTNodeType::PAREN_EXPR) {
    operand = static_cast<const sun::ast::ParenExprAST*>(operand)->getInner();
  }
  Value* retained = nullptr;
  if (operand->getType() == sun::ast::ASTNodeType::VARIABLE_REFERENCE) {
    for (auto active = nativeCatches_.rbegin(); active != nativeCatches_.rend();
         ++active) {
      if (active->function == function && active->binding &&
          active->binding == operand->getTargetDeclarationId()) {
        retained = active->record;
        break;
      }
    }
  }
  if (!retained) {
    logAndThrowError(
        "Exception trace extraction requires a direct active catch binding",
        call.getLocation());
  }
  if (statusOnly) {
    auto* address = builder.CreateConstGEP1_64(builder.getInt8Ty(), retained,
                                               layout::traceStatusOffset);
    return builder.CreateLoad(builder.getInt32Ty(), address);
  }
  auto* frames = builder.CreateConstGEP1_64(builder.getInt8Ty(), retained,
                                            layout::traceFramesOffset);
  auto* dimensions = builder.CreateConstGEP1_64(builder.getInt8Ty(), retained,
                                                layout::traceOffset);
  Value* array = UndefValue::get(
      sun::types::ArrayType::getArrayStructType(ctx.getContext()));
  array = builder.CreateInsertValue(array, frames, 0);
  array = builder.CreateInsertValue(array, builder.getInt32(1), 1);
  return builder.CreateInsertValue(array, dimensions, 2);
}

Value* ExceptionGenerator::reportCurrentException(
    const sun::support::Position& location, bool unhandled) {
  auto& builder = *ctx.builder;
  auto* function = builder.GetInsertBlock()->getParent();
  for (auto active = nativeCatches_.rbegin(); active != nativeCatches_.rend();
       ++active) {
    if (active->function == function) {
      return builder.CreateCall(
          module->getOrInsertFunction("sun_eh_report", builder.getVoidTy(),
                                      builder.getPtrTy(), builder.getInt32Ty()),
          {active->record, builder.getInt32(unhandled)});
    }
  }
  logAndThrowError("_report_exception requires an active catch handler",
                   location);
}

Value* ExceptionGenerator::emitNativeCall(FunctionType* type, Value* callee,
                                          ArrayRef<Value*> args,
                                          const Twine& name) {
  auto& builder = *ctx.builder;
  auto* function = builder.GetInsertBlock()->getParent();
  std::vector<NativeHandler> handlers;
  for (auto handler = nativeHandlers_.rbegin();
       handler != nativeHandlers_.rend(); ++handler) {
    if (handler->function != function) {
      continue;
    }
    handlers.push_back(*handler);
    // An inner catch-all handles every exception before outer handlers can
    // match.
    if (handler->type->isNullValue()) {
      break;
    }
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
  auto* continuation =
      BasicBlock::Create(ctx.getContext(), "invoke.cont", function);
  auto* unwind =
      BasicBlock::Create(ctx.getContext(), "exception.pad", function);
  auto* invoke =
      builder.CreateInvoke(type, callee, continuation, unwind, args, callName);
  abi.apply(*invoke);
  builder.SetInsertPoint(unwind);
  auto* resultType = StructType::get(builder.getPtrTy(), builder.getInt32Ty());
  auto* pad = builder.CreateLandingPad(resultType, handlers.size() + 1);
  pad->setCleanup(true);
  for (const auto& handler : handlers) {
    pad->addClause(handler.type);
  }
  if (!canEscape) {
    pad->addClause(ConstantPointerNull::get(builder.getPtrTy()));
  }
  auto* exception = builder.CreateExtractValue(pad, 0);
  auto* selector = builder.CreateExtractValue(pad, 1);
  for (const auto& handler : handlers) {
    auto* selected =
        BasicBlock::Create(ctx.getContext(), "catch.cleanup", function);
    auto* next = BasicBlock::Create(ctx.getContext(), "catch.next", function);
    auto* id = builder.CreateCall(
        Intrinsic::getOrInsertDeclaration(module, Intrinsic::eh_typeid_for,
                                          {builder.getPtrTy()}),
        {handler.type});
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
    emitExceptionTermination(exception);
  }
  builder.SetInsertPoint(continuation);
  return invoke;
}

Value* ExceptionGenerator::codegen(const sun::ast::ThrowAST& statement) {
  auto& builder = *ctx.builder;
  auto* function = builder.GetInsertBlock()->getParent();
  const auto* operand = statement.value.get();
  while (operand && operand->getType() == sun::ast::ASTNodeType::PAREN_EXPR) {
    operand = static_cast<const sun::ast::ParenExprAST*>(operand)->getInner();
  }
  Value* value = nullptr;
  for (auto active = nativeCatches_.rbegin(); active != nativeCatches_.rend();
       ++active) {
    if (active->function != function) {
      continue;
    }
    if (!operand ||
        (operand->getType() == sun::ast::ASTNodeType::VARIABLE_REFERENCE &&
         operand->getTargetDeclarationId() == active->binding)) {
      value = active->record;
      break;
    }
  }
  if (!value) {
    if (!statement.value) {
      logAndThrowError("No active exception to rethrow",
                       statement.getLocation());
    }
    auto type = std::static_pointer_cast<sun::types::ClassType>(
        statement.value->getResolvedType());
    auto* descriptor = exceptionDescriptor(*type);
    // Evaluate the expression before claiming runtime storage: evaluation can
    // itself throw.
    auto* source = codegen(*statement.value);
    state_.debugInfo.attachExpressionLocation(builder, statement.getLocation());
    recordThrowSource(*module, statement.getLocation(), type->getDisplayName());
    auto raise =
        module->getOrInsertFunction("sun_eh_throw", builder.getVoidTy(),
                                    builder.getPtrTy(), builder.getPtrTy());
    auto* call =
        emitPossiblyThrowingCall(raise, {descriptor, source}, true, "");
    if (auto* direct = dyn_cast<CallInst>(call)) {
      direct->setTailCallKind(CallInst::TCK_NoTail);
    }
  } else {
    auto raise = module->getOrInsertFunction(
        "sun_eh_raise", builder.getVoidTy(), builder.getPtrTy());
    emitPossiblyThrowingCall(raise, {value}, true, "");
  }
  builder.CreateUnreachable();
  return nullptr;
}

Value* ExceptionGenerator::codegen(const sun::ast::TryCatchAST& statement) {
  auto& builder = *ctx.builder;
  auto* function = builder.GetInsertBlock()->getParent();
  auto* continuation =
      BasicBlock::Create(ctx.getContext(), "try.end", function);
  std::vector<NativeHandler> handlers;
  for (const auto& source : statement.handlers) {
    Constant* type = ConstantPointerNull::get(builder.getPtrTy());
    if (source.binding) {
      type =
          exceptionDescriptor(*std::static_pointer_cast<sun::types::ClassType>(
              source.binding->getResolvedType()));
    }
    auto* block = BasicBlock::Create(ctx.getContext(), "catch.body", function);
    auto* exception =
        PHINode::Create(builder.getPtrTy(), 0, "caught.record", block);
    handlers.push_back({function, type, block, exception, scopes().size()});
  }
  const auto saved = nativeHandlers_.size();
  // The dispatch stack is visited in reverse, preserving source order here.
  for (auto handler = handlers.rbegin(); handler != handlers.rend();
       ++handler) {
    nativeHandlers_.push_back(*handler);
  }
  scopes().push(statement.body->getLocation());
  codegen(*statement.body);
  scopes().pop();
  if (!builder.GetInsertBlock()->getTerminator()) {
    builder.CreateBr(continuation);
  }
  nativeHandlers_.resize(saved);
  for (std::size_t i = 0; i < handlers.size(); ++i) {
    const auto& source = statement.handlers[i];
    const auto& handler = handlers[i];
    builder.SetInsertPoint(handler.block);
    auto* caught =
        exceptionStorage(storageTypes(*module, ctx.exceptionTraceDepth).value,
                         "caught.exception");
    auto take =
        module->getOrInsertFunction("sun_eh_take", builder.getVoidTy(),
                                    builder.getPtrTy(), builder.getPtrTy());
    builder.CreateCall(take, {handler.exception, caught});
    scopes().push(source.body->getLocation());
    sun::semantic_analysis::DeclarationId binding;
    if (source.binding) {
      binding = source.binding->getDeclarationId();
      auto* slot = gen_.createEntryBlockAlloca(function, "catch.binding",
                                               builder.getPtrTy());
      builder.CreateStore(
          builder.CreateConstGEP1_64(builder.getInt8Ty(), caught,
                                     layout::payloadOffset),
          slot);
      scopes().back().variables[binding] = slot;
      scopes().back().indirectBindings.insert(binding);
    }
    nativeCatches_.push_back({function, binding, caught});
    codegen(*source.body);
    nativeCatches_.pop_back();
    scopes().pop();
    if (!builder.GetInsertBlock()->getTerminator()) {
      builder.CreateBr(continuation);
    }
  }
  builder.SetInsertPoint(continuation);
  if (pred_empty(continuation)) {
    builder.CreateUnreachable();
  }
  return builder.getInt32(0);
}
}  // namespace sun::codegen::exceptions
