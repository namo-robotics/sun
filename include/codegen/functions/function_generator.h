#pragma once

// function_generator.h — Functions, lambdas, closures and returns
//
// Named functions are thin pointers. Lambdas and bound methods use a fat
// { fn, env } value. A capture list entry that says `ref` borrows; otherwise it
// is owned by the closure, so a compound moves into the environment and the
// scope that built it drops it.
//
// Returns live here too, because what a return must do is decided by the
// function being emitted: hand back a referent's address for `ref T`, or
// unwind rather than return for an error.

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "ast.h"
#include "codegen/abi/extern_c.h"
#include "codegen/codegen_state.h"

class CodegenVisitor;
class ClassGenerator;
class FunctionRegistry;
class ScopeManager;

/**
 * The closure environment in scope while a function body is emitted.
 */
struct ClosureContext {
  llvm::StructType* fatType;  // Only used for lambdas
  llvm::StructType* envType;
  llvm::Value* fatPtr;
  std::vector<Capture> captures;
  std::map<std::string, unsigned> captureIndex;
  std::map<std::string, llvm::Type*> captureTypes;
};

/**
 * What declaring a function's LLVM signature produced.
 */
struct FuncDeclResult {
  llvm::Function* func;
  llvm::StructType* fatType;
  llvm::StructType* envType;
  llvm::Type* returnType;
  llvm::Type* valueType;  // Underlying type before error union wrapping
  bool canError;
};

/**
 * Emits function and lambda definitions, the closures they need, and returns.
 */
class FunctionGenerator {
 public:
  FunctionGenerator(CodegenState& state, CodegenVisitor& gen)
      : state_(state),
        gen_(gen),
        ctx(state.ctx),
        module(state.module),
        typeResolver(state.typeResolver),
        debugInfo(state.debugInfo),
        currentFunctionCanError(state.frame.canError),
        currentFunctionReturnsRef(state.frame.returnsRef),
        currentFunctionValueType(state.frame.valueType) {}

  FunctionGenerator(const FunctionGenerator&) = delete;
  FunctionGenerator& operator=(const FunctionGenerator&) = delete;

  // ---------------------------------------------------------------
  // Definitions
  // ---------------------------------------------------------------

  llvm::Value* codegenFunc(FunctionAST& func);
  llvm::Value* codegenGenericFunc(FunctionAST& func);
  llvm::Value* codegenExternFunc(FunctionAST& func);
  llvm::Value* codegenLambda(LambdaAST& lambda);

  // Declare a prototype's LLVM signature, body to follow
  std::pair<llvm::Function*, llvm::StructType*> codegen(
      const PrototypeAST& proto, llvm::StructType* envType, bool isLambda,
      llvm::Type* returnType = nullptr);
  FuncDeclResult declareFuncSignature(PrototypeAST& proto);

  // Declare one function signature, body to follow
  void forwardDeclareFunction(const PrototypeAST& proto);

  // Declare every function and method a block defines, before any body is
  // emitted, so calls may name things defined further down the block
  void declareBlockSignatures(const BlockExprAST& block);

  // ---------------------------------------------------------------
  // Returns
  // ---------------------------------------------------------------

  llvm::Value* codegen(const ReturnExprAST& expr);

  // ---------------------------------------------------------------
  // Captures
  // ---------------------------------------------------------------

  /**
   * Address of a captured variable's storage: the environment slot for a
   * by-value capture, the stored pointer for a `[ref x]` capture. Returns
   * nullptr when the name is not a capture.
   */
  llvm::Value* createCaptureSlotAddress(const std::string& name,
                                        llvm::Type** valueTypeOut = nullptr,
                                        bool* byRefOut = nullptr,
                                        bool* ownedOut = nullptr);

  /**
   * Loads a variable from the closure context if it is one.
   */
  llvm::LoadInst* createLoadVarFromClosure(const std::string& name);

 private:
  CodegenState& state_;
  CodegenVisitor& gen_;

  // Aliases into the shared state, so the emission code reads the same way
  // the rest of codegen does
  CodegenContext& ctx;
  llvm::Module* module;
  LLVMTypeResolver& typeResolver;
  sun::DebugInfoBuilder& debugInfo;
  bool& currentFunctionCanError;
  bool& currentFunctionReturnsRef;
  llvm::Type*& currentFunctionValueType;

  // Closure environments for lambda compilations in flight
  std::vector<ClosureContext> closureStack;

  // Counter for generating unique names for anonymous lambdas
  unsigned lambdaCounter = 0;

  // Environment slot initializer at closure creation: the value for a
  // by-value capture, the referent's address for a `[ref x]` capture
  llvm::Value* computeCaptureInitValue(const Capture& cap);

  llvm::StructType* createEnvTypeForFunc(const PrototypeAST& proto);
  llvm::StructType* createFatTypeForFunc(llvm::Function* func,
                                         llvm::StructType* envType,
                                         const PrototypeAST& proto);
  llvm::Value* createFatClosure(llvm::Function* func, llvm::StructType* fatType,
                                llvm::StructType* envType,
                                const PrototypeAST& proto);

  // Fill a closure environment's capture slots. Owned captures of compound
  // values move in and the slot is registered for drop.
  bool fillCaptureSlots(llvm::StructType* envType, llvm::Value* envAlloca,
                        const PrototypeAST& proto,
                        llvm::IRBuilder<>& entryBuilder);

  // What function codegen borrows from the rest of codegen.
  // The BlockExprAST overload matters: without it a body would bind to
  // codegen(const ExprAST&), which attaches an expression debug location the
  // block path does not want.
  llvm::Value* codegen(const ExprAST& expr);
  llvm::Value* codegen(const BlockExprAST& block);

  // A node kind with its own overload must not silently bind to the
  // ExprAST forwarder above. Make it a compile error instead.
  template <typename T>
    requires(!std::is_same_v<T, ExprAST> && !std::is_same_v<T, BlockExprAST> &&
             !std::is_same_v<T, ReturnExprAST> && std::is_base_of_v<ExprAST, T>)
  llvm::Value* codegen(const T&) = delete;

  ScopeManager& scopes();
  sun::cabi::ExternCEmitter& externC();
  llvm::LoadInst* createLoadForLocalVar(const std::string& name);
  llvm::LoadInst* createLoadForGlobalVar(const std::string& varName);
  llvm::Value* applyMoveSemantics(llvm::Value* argVal, sun::TypePtr argSunType);
  FunctionRegistry& functions();
  ClassGenerator& classes();
  llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                           llvm::StringRef varName,
                                           llvm::Type* type);
  void debugDeclareParam(llvm::AllocaInst* alloca, const std::string& name,
                         const PrototypeAST& proto, unsigned userArgIdx,
                         unsigned argNoBase = 1);
};
