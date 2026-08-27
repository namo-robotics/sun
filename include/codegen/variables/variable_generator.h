#pragma once

// variable_generator.h — Variables: creating them, reading them, writing them
//
// Locals, globals and borrows, plus the lvalue machinery every assignment
// needs. Three things here are worth knowing:
//
//   a global that cannot be a constant is zero-initialized and its
//   initializer queued, then run in one static-init function before main
//
//   `ref r = x` stores the referent's ADDRESS, so binding, reading and
//   writing through a reference all go through the address path rather than
//   loading the value
//
//   overwriting a compound variable drops the old value first and moves the
//   new one in, because Sun never copies a compound implicitly

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include <string>
#include <type_traits>
#include <vector>

#include "ast.h"
#include "codegen/codegen_state.h"

class CodegenVisitor;
class ClassGenerator;
class FunctionGenerator;
class FunctionRegistry;
class ScopeManager;

/**
 * Emits variable creation, reference and assignment, the lvalue addresses
 * assignments write through, and the static initialization of globals.
 */
class VariableGenerator {
 public:
  VariableGenerator(CodegenState& state, CodegenVisitor& gen)
      : state_(state),
        gen_(gen),
        ctx(state.ctx),
        module(state.module),
        typeResolver(state.typeResolver),
        debugInfo(state.debugInfo) {}

  VariableGenerator(const VariableGenerator&) = delete;
  VariableGenerator& operator=(const VariableGenerator&) = delete;

  // ---------------------------------------------------------------
  // Creation, reference, assignment
  // ---------------------------------------------------------------

  llvm::Value* codegen(const VariableCreationAST& expr);
  llvm::Value* codegen(const VariableReferenceAST& expr);
  llvm::Value* codegen(const VariableAssignmentAST& expr);
  llvm::Value* codegen(const ReferenceCreationAST& expr);
  llvm::Value* codegen(const CompoundAssignmentAST& expr);

  /**
   * Assigns an already-evaluated value to a variable slot (a local alloca or
   * a global). A compound value drops what the slot held and MOVES the source
   * in; self-assignment emits nothing.
   */
  void assignToVariableSlot(llvm::Value* slot, llvm::Value* value,
                            const sun::TypePtr& varType,
                            const std::string& name);

  llvm::GlobalVariable* createGlobalVariable(
      const std::string& name, llvm::Type* type,
      llvm::Constant* initializer = nullptr);

  /**
   * Emits the static initialization function for the globals that could not
   * be constant-initialized. Call after all top-level codegen, before main.
   */
  void emitStaticInitFunction();

  // ---------------------------------------------------------------
  // Lvalues
  // ---------------------------------------------------------------

  /**
   * The storage address of an assignable expression. tryCodegenAddress
   * returns nullptr for shapes with no addressable slot (class __index__
   * targets, slices, closure captures, temporaries); codegenAddress throws
   * instead. Neither ever spills a value to a temporary alloca.
   */
  llvm::Value* tryCodegenAddress(const ExprAST& expr);
  llvm::Value* codegenAddress(const ExprAST& expr);

  /**
   * Same, plus conditional lvalues (`ref r = c ? a.x : b.y`), whose address is
   * a phi of the branches'. Only borrow bindings take that path.
   */
  llvm::Value* codegenBorrowAddress(const ExprAST& expr);

  /**
   * Codegens a member-access object down to (objectPtr, ClassType*), applying
   * the generic-`this` fixup and unwrapping raw_ptr/static_ptr/ref to class.
   * ClassType* is null when the object is not class-shaped.
   */
  std::pair<llvm::Value*, sun::ClassType*> codegenObjectPtr(
      const ExprAST& object);

  // ---------------------------------------------------------------
  // Reading and writing through a reference
  // ---------------------------------------------------------------

  llvm::LoadInst* createLoadForLocalVar(const std::string& name);
  llvm::LoadInst* createLoadForGlobalVar(const std::string& varName);
  llvm::Value* createLoadForRef(const std::string& varName,
                                const sun::ReferenceType& refType);
  void createStoreForRef(const std::string& varName,
                         const sun::ReferenceType& refType,
                         llvm::Value* value);

 private:
  CodegenState& state_;
  CodegenVisitor& gen_;

  // Aliases into the shared state, so the emission code reads the same way
  // the rest of codegen does
  CodegenContext& ctx;
  llvm::Module* module;
  LLVMTypeResolver& typeResolver;
  sun::DebugInfoBuilder& debugInfo;

  /**
   * A global variable whose initializer has to run at program start.
   */
  struct StaticInitInfo {
    llvm::GlobalVariable* globalVar;  // The global variable
    std::string varName;              // Variable name (for diagnostics)
    sun::TypePtr varType;             // Variable type
    std::shared_ptr<sun::ClassType>
        classType;            // Class type (if class, else nullptr)
    const ExprAST* initExpr;  // The initialization expression
    Position location;        // Declaration site (for diagnostics)
  };

  // Globals still waiting for their initializer to be emitted
  std::vector<StaticInitInfo> staticInits;

  llvm::Value* genLocalVar(const VariableCreationAST& expr,
                           llvm::Type* varType);
  llvm::Value* genFunctionVariable(const VariableCreationAST& expr);
  llvm::Constant* genGlobalArray(const VariableCreationAST& expr);
  llvm::Constant* genGlobalVarForConstantExpr(const VariableCreationAST& expr,
                                              llvm::Type* varType);
  llvm::GlobalVariable* genGlobalClassVar(const VariableCreationAST& expr,
                                          sun::ClassType& classType);
  llvm::GlobalVariable* genGlobalVarWithRuntimeInit(
      const VariableCreationAST& expr, llvm::Type* varType);

  // Compound assignment: address-once -> load -> op -> store
  llvm::Value* emitCompoundOpValue(const CompoundAssignmentAST& expr,
                                   llvm::Value* cur, llvm::Type* slotTy,
                                   const sun::TypePtr& slotSunType);

  // What variable codegen borrows from the rest of codegen.
  // The BlockExprAST overload matters: without it a block would bind to
  // codegen(const ExprAST&), which attaches an expression debug location the
  // block path does not want.
  llvm::Value* codegen(const ExprAST& expr);
  llvm::Value* codegen(const BlockExprAST& block);

  // A node kind with its own overload must not silently bind to the
  // ExprAST forwarder above. Make it a compile error instead.
  template <typename T>
    requires(!std::is_same_v<T, ExprAST> && !std::is_same_v<T, BlockExprAST> &&
             !std::is_same_v<T, VariableCreationAST> &&
             !std::is_same_v<T, VariableReferenceAST> &&
             !std::is_same_v<T, VariableAssignmentAST> &&
             !std::is_same_v<T, ReferenceCreationAST> &&
             !std::is_same_v<T, CompoundAssignmentAST> &&
             std::is_base_of_v<ExprAST, T>)
  llvm::Value* codegen(const T&) = delete;

  ScopeManager& scopes();
  FunctionRegistry& functions();
  ClassGenerator& classes();
  FunctionGenerator& functionGen();
  llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                           llvm::StringRef varName,
                                           llvm::Type* type);
  void debugDeclareLocal(llvm::AllocaInst* alloca, const std::string& name,
                         const sun::TypePtr& type, const Position& loc);
};
