#pragma once

/** Translates analyzed Sun programs into LLVM instructions. */
namespace sun::codegen {
class CodegenVisitor;
}
/** Provides the generator for class storage and method operations. */
namespace sun::codegen::classes {
class ClassGenerator;
}
/** Provides the scope manager responsible for variable storage and cleanup. */
namespace sun::codegen::scopes {
class ScopeManager;
}

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

/** Provides the registry of generated functions and their metadata. */
namespace sun::codegen::functions {
using sun::ast::BlockExprAST;
using sun::ast::ExprAST;
using sun::ast::FunctionAST;
using sun::ast::PrototypeAST;

class FunctionRegistry;

/**
 * The closure environment in scope while a function body is emitted.
 */
struct ClosureContext {
  llvm::StructType* fatType;  // Only used for lambdas
  llvm::StructType* envType;
  llvm::Value* fatPtr;
  std::vector<sun::ast::Capture> captures;
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
  /** Binds function generation to the shared expression visitor and state. */
  FunctionGenerator(sun::codegen::CodegenState& state,
                    sun::codegen::CodegenVisitor& gen)
      : state_(state),
        gen_(gen),
        ctx(state.ctx),
        module(state.module),
        typeResolver(state.typeResolver),
        debugInfo(state.debugInfo),
        currentFunctionCanError(state.frame.canError),
        currentFunctionReturnsRef(state.frame.returnsRef),
        currentFunctionValueType(state.frame.valueType) {}

  /** Binds function generation to the shared expression visitor and state. */
  FunctionGenerator(const FunctionGenerator&) = delete;
  /** Disallows assignment so ownership and object identity cannot be duplicated. */
  FunctionGenerator& operator=(const FunctionGenerator&) = delete;

  // ---------------------------------------------------------------
  // Definitions
  // ---------------------------------------------------------------

  /** Emits LLVM code for a function body. */
  llvm::Value* codegenFunc(FunctionAST& func);
  /** Emits LLVM code for a specialized generic function. */
  llvm::Value* codegenGenericFunc(FunctionAST& func);
  /** Emits LLVM code for an external function declaration. */
  llvm::Value* codegenExternFunc(FunctionAST& func);
  /** Emits LLVM code for a lambda and its captured environment. */
  llvm::Value* codegenLambda(sun::ast::LambdaAST& lambda);

  /**
   * Declare a prototype's LLVM signature, body to follow
   */
  std::pair<llvm::Function*, llvm::StructType*> codegen(
      const PrototypeAST& proto, llvm::StructType* envType, bool isLambda,
      llvm::Type* returnType = nullptr);
  /** Declares an LLVM function from its analyzed Sun signature. */
  FuncDeclResult declareFuncSignature(PrototypeAST& proto);

  /**
   * Declare one function signature, body to follow
   */
  void forwardDeclareFunction(const PrototypeAST& proto);

  /**
   * Declare every function and method in a block's module subtree before any
   * body is emitted, so calls may name things defined later in merged input.
   */
  void declareBlockSignatures(const BlockExprAST& block);

  // ---------------------------------------------------------------
  // Returns
  // ---------------------------------------------------------------

  /** Emits LLVM instructions for this syntax node and returns its generated value. */
  llvm::Value* codegen(const sun::ast::ReturnExprAST& expr);

  // ---------------------------------------------------------------
  // Captures
  // ---------------------------------------------------------------

  /**
   * Address of a captured variable's storage: the environment slot for a
   * by-value capture, the stored pointer for a `[ref x]` capture. Returns
   * nullptr when the name is not a capture.
   */
  llvm::Value* createCaptureSlotAddress(
      sun::semantic_analysis::DeclarationId id,
      llvm::Type** valueTypeOut = nullptr, bool* byRefOut = nullptr,
      bool* ownedOut = nullptr);

  /**
   * Loads a variable from the closure context if it is one.
   */
  llvm::LoadInst* createLoadVarFromClosure(
      sun::semantic_analysis::DeclarationId id);

 private:
  sun::codegen::CodegenState& state_;
  sun::codegen::CodegenVisitor& gen_;

  // Aliases into the shared state, so the emission code reads the same way
  // the rest of codegen does
  sun::codegen::CodegenContext& ctx;
  llvm::Module* module;
  sun::codegen::LLVMTypeResolver& typeResolver;
  sun::codegen::DebugInfoBuilder& debugInfo;
  bool& currentFunctionCanError;
  bool& currentFunctionReturnsRef;
  llvm::Type*& currentFunctionValueType;

  // Closure environments for lambda compilations in flight
  std::vector<ClosureContext> closureStack;

  // Counter for generating unique names for anonymous lambdas
  unsigned lambdaCounter = 0;

  /**
   * Environment slot initializer at closure creation: the value for a
   * by-value capture, the referent's address for a `[ref x]` capture
   */
  llvm::Value* computeCaptureInitValue(const sun::ast::Capture& cap);

  /** Builds the LLVM structure holding a function's captured environment. */
  llvm::StructType* createEnvTypeForFunc(const PrototypeAST& proto);
  /** Builds the LLVM representation combining a function and its environment. */
  llvm::StructType* createFatTypeForFunc(llvm::Function* func,
                                         llvm::StructType* envType,
                                         const PrototypeAST& proto);
  /** Creates a callable value containing its function pointer and captures. */
  llvm::Value* createFatClosure(llvm::Function* func, llvm::StructType* fatType,
                                llvm::StructType* envType,
                                const PrototypeAST& proto);

  /**
   * Fill a closure environment's capture slots. Owned captures of compound
   * values move in and the slot is registered for drop.
   */
  bool fillCaptureSlots(llvm::StructType* envType, llvm::Value* envAlloca,
                        const PrototypeAST& proto,
                        llvm::IRBuilder<>& entryBuilder);

  /**
   * What function codegen borrows from the rest of codegen.
   * The BlockExprAST overload matters: without it a body would bind to
   * codegen(const ExprAST&), which attaches an expression debug location the
   * block path does not want.
   */
  llvm::Value* codegen(const ExprAST& expr);
  /** Emits LLVM instructions for this syntax node and returns its generated value. */
  llvm::Value* codegen(const BlockExprAST& block);

  /**
   * A node kind with its own overload must not silently bind to the
   * ExprAST forwarder above. Make it a compile error instead.
   */
  template <typename T>
    requires(!std::is_same_v<T, ExprAST> && !std::is_same_v<T, BlockExprAST> &&
             !std::is_same_v<T, sun::ast::ReturnExprAST> &&
             std::is_base_of_v<ExprAST, T>)
  llvm::Value* codegen(const T&) = delete;

  /** Provides the scope manager responsible for variable storage and cleanup. */
  sun::codegen::scopes::ScopeManager& scopes();
  /** Provides the emitter for calls using the C calling convention. */
  sun::codegen::abi::ExternCEmitter& externC();
  /** Transfers an argument value according to its ownership requirements. */
  llvm::Value* applyMoveSemantics(llvm::Value* argVal,
                                  sun::types::TypePtr argSunType);
  /** Provides the registry of generated functions and their metadata. */
  FunctionRegistry& functions();
  /** Provides the generator for class storage and method operations. */
  sun::codegen::classes::ClassGenerator& classes();
  /** Allocates local storage in the function entry block. */
  llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                           llvm::StringRef varName,
                                           llvm::Type* type);
  /** Associates generated parameter storage with its source declaration. */
  void debugDeclareParam(llvm::AllocaInst* alloca, const std::string& name,
                         const PrototypeAST& proto, unsigned userArgIdx,
                         unsigned argNoBase = 1);
};

}  // namespace sun::codegen::functions
