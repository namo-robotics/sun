#pragma once

/** Translates analyzed Sun programs into LLVM instructions. */
namespace sun::codegen {
class CodegenVisitor;
}
/** Provides the generator for class storage and method operations. */
namespace sun::codegen::classes {
class ClassGenerator;
}
/** Provides the registry of generated functions and their metadata. */
namespace sun::codegen::functions {
class FunctionGenerator;
}
/** Provides the registry of generated functions and their metadata. */
namespace sun::codegen::functions {
class FunctionRegistry;
}
/** Provides the scope manager responsible for variable storage and cleanup. */
namespace sun::codegen::scopes {
class ScopeManager;
}

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
#include <llvm/IR/ValueHandle.h>

#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "codegen/codegen_state.h"

/** Generates storage and access operations for Sun variables. */
namespace sun::codegen::variables {
using sun::ast::BlockExprAST;
using sun::ast::CompoundAssignmentAST;
using sun::ast::ExprAST;
using sun::ast::VariableCreationAST;
using sun::semantic_analysis::DeclarationId;
using sun::types::ClassType;
using sun::types::TypePtr;

/**
 * Emits variable creation, reference and assignment, the lvalue addresses
 * assignments write through, and the static initialization of globals.
 */
class VariableGenerator {
 public:
  /** Binds variable generation to the shared expression visitor and state. */
  VariableGenerator(sun::codegen::CodegenState& state,
                    sun::codegen::CodegenVisitor& gen)
      : state_(state),
        gen_(gen),
        ctx(state.ctx),
        module(state.module),
        typeResolver(state.typeResolver),
        debugInfo(state.debugInfo) {}

  /** Binds variable generation to the shared expression visitor and state. */
  VariableGenerator(const VariableGenerator&) = delete;
  /** Disallows assignment so ownership and object identity cannot be duplicated. */
  VariableGenerator& operator=(const VariableGenerator&) = delete;

  // ---------------------------------------------------------------
  // Creation, reference, assignment
  // ---------------------------------------------------------------

  /** Emits LLVM instructions for this syntax node and returns its generated value. */
  llvm::Value* codegen(const VariableCreationAST& expr);
  /** Emits LLVM instructions for this syntax node and returns its generated value. */
  llvm::Value* codegen(const sun::ast::VariableReferenceAST& expr);
  /** Emits LLVM instructions for this syntax node and returns its generated value. */
  llvm::Value* codegen(const sun::ast::VariableAssignmentAST& expr);
  /** Emits LLVM instructions for this syntax node and returns its generated value. */
  llvm::Value* codegen(const sun::ast::ReferenceCreationAST& expr);
  /** Emits LLVM instructions for this syntax node and returns its generated value. */
  llvm::Value* codegen(const CompoundAssignmentAST& expr);

  /**
   * Assigns an already-evaluated value to a variable slot (a local alloca or
   * a global). A compound value drops what the slot held and MOVES the source
   * in; self-assignment emits nothing.
   */
  void assignToVariableSlot(llvm::Value* slot, llvm::Value* value,
                            const TypePtr& varType, const std::string& name);

  /** Creates and registers LLVM storage for a global declaration. */
  llvm::GlobalVariable* createGlobalVariable(
      DeclarationId id, const std::string& name, llvm::Type* type,
      llvm::Constant* initializer = nullptr);

  /** Declare imported and C globals before emitting dependent bodies. */
  void declareBlockExternalGlobals(const BlockExprAST& block);

  /** Find storage for the global selected during semantic analysis. */
  llvm::GlobalVariable* findGlobal(DeclarationId id) const;

  /**
   * Emits the static initialization function for the globals that could not
   * be constant-initialized. Call after all top-level codegen, before main.
   *
   * `initOrder` places the function among those of other bundles: lower
   * values run first, and a module's value is one more than the highest among
   * its imports. `bundleHash` names the bundle being built and is empty for a
   * program; a bundle's function runs at most once even when its code reaches
   * a program through several imports.
   */
  void emitStaticInitFunction(uint32_t initOrder,
                              const std::string& bundleHash);

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
  /** Emits the storage address of an addressable expression. */
  llvm::Value* codegenAddress(const ExprAST& expr);

  /**
   * Also handles conditional reference bindings, whose address is
   * a phi of the branches'. Only borrow bindings take that path.
   */
  llvm::Value* codegenBorrowAddress(const ExprAST& expr);

  /**
   * Codegens a member-access object down to (objectPtr, ClassType*), applying
   * the generic-`this` fixup and unwrapping raw_ptr/static_ptr/ref to class.
   * ClassType* is null when the object is not class-shaped.
   */
  std::pair<llvm::Value*, ClassType*> codegenObjectPtr(const ExprAST& object);

  // ---------------------------------------------------------------
  // Reading and writing through a reference
  // ---------------------------------------------------------------

  /** Loads the value stored for a local declaration. */
  llvm::LoadInst* createLoadForLocalVar(DeclarationId id);
  /**
   * Reads the value of a global declaration. Inside a function this emits a
   * load. In a file-scope initializer no code can be emitted, so a `const`
   * number global yields its compile-time value instead and any other global
   * is a compilation error reported at `location`. Returns null when the
   * declaration has no global storage.
   */
  llvm::Value* createLoadForGlobalVar(DeclarationId id,
                                      const sun::support::Position& location);
  /** Loads a value through the reference stored for a declaration. */
  llvm::Value* createLoadForRef(DeclarationId id,
                                const sun::types::ReferenceType& refType);
  /** Stores a value through the reference bound to a declaration. */
  void createStoreForRef(DeclarationId id,
                         const sun::types::ReferenceType& refType,
                         llvm::Value* value);

 private:
  /** Bind a created or imported global to its source declaration. */
  llvm::GlobalVariable* bindGlobal(DeclarationId id,
                                   llvm::GlobalVariable* global);
  std::unordered_map<DeclarationId, llvm::WeakTrackingVH> globals_;
  /**
   * Compile-time values of the `const` number globals defined in this
   * program. File-scope initializers read these because no load can be
   * emitted outside a function.
   */
  std::unordered_map<DeclarationId, llvm::Constant*> constantGlobalValues_;
  sun::codegen::CodegenState& state_;
  sun::codegen::CodegenVisitor& gen_;

  // Aliases into the shared state, so the emission code reads the same way
  // the rest of codegen does
  sun::codegen::CodegenContext& ctx;
  llvm::Module* module;
  sun::codegen::LLVMTypeResolver& typeResolver;
  sun::codegen::DebugInfoBuilder& debugInfo;

  /**
   * A global variable whose initializer has to run at program start.
   */
  struct StaticInitInfo {
    llvm::GlobalVariable* globalVar;  // The global variable
    std::string varName;              // Variable name (for diagnostics)
    TypePtr varType;                  // Variable type
    std::shared_ptr<ClassType>
        classType;            // Class type (if class, else nullptr)
    const ExprAST* initExpr;  // The initialization expression
    sun::support::Position location;  // Declaration site (for diagnostics)
  };

  // Globals still waiting for their initializer to be emitted
  std::vector<StaticInitInfo> staticInits;

  /** Allocates and initializes storage for a local variable. */
  llvm::Value* genLocalVar(const VariableCreationAST& expr,
                           llvm::Type* varType);
  /** Creates storage for a variable holding a callable value. */
  llvm::Value* genFunctionVariable(const VariableCreationAST& expr);
  /** Creates global array storage and its initializer. */
  llvm::Constant* genGlobalArray(const VariableCreationAST& expr);
  /**
   * Evaluates a file-scope initializer at compile time and converts it to
   * `varType`. Fails compilation when the initializer is not a constant
   * expression.
   */
  llvm::Constant* foldGlobalInitializer(const VariableCreationAST& expr,
                                        llvm::Type* varType);
  /** Initializes global storage with a compile-time constant. */
  llvm::Constant* genGlobalVarForConstantExpr(const VariableCreationAST& expr,
                                              llvm::Type* varType);
  /** Creates and initializes global storage for a class value. */
  llvm::GlobalVariable* genGlobalClassVar(const VariableCreationAST& expr,
                                          ClassType& classType);
  /** Creates global storage and arranges initialization during program startup. */
  llvm::GlobalVariable* genGlobalVarWithRuntimeInit(
      const VariableCreationAST& expr, llvm::Type* varType);

  /**
   * Compound assignment: address-once -> load -> op -> store
   */
  llvm::Value* emitCompoundOpValue(const CompoundAssignmentAST& expr,
                                   llvm::Value* cur, llvm::Type* slotTy,
                                   const TypePtr& slotSunType);

  /**
   * What variable codegen borrows from the rest of codegen.
   * The BlockExprAST overload matters: without it a block would bind to
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
             !std::is_same_v<T, VariableCreationAST> &&
             !std::is_same_v<T, sun::ast::VariableReferenceAST> &&
             !std::is_same_v<T, sun::ast::VariableAssignmentAST> &&
             !std::is_same_v<T, sun::ast::ReferenceCreationAST> &&
             !std::is_same_v<T, CompoundAssignmentAST> &&
             std::is_base_of_v<ExprAST, T>)
  llvm::Value* codegen(const T&) = delete;

  /** Provides the scope manager responsible for variable storage and cleanup. */
  sun::codegen::scopes::ScopeManager& scopes();
  /** Provides the registry of generated functions and their metadata. */
  sun::codegen::functions::FunctionRegistry& functions();
  /** Provides the generator for class storage and method operations. */
  sun::codegen::classes::ClassGenerator& classes();
  /** Provides the generator for function bodies and callable values. */
  sun::codegen::functions::FunctionGenerator& functionGen();
  /** Allocates local storage in the function entry block. */
  llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                           llvm::StringRef varName,
                                           llvm::Type* type);
  /** Associates local storage with its source variable for debugging. */
  void debugDeclareLocal(llvm::AllocaInst* alloca, const std::string& name,
                         const TypePtr& type,
                         const sun::support::Position& loc);
};

}  // namespace sun::codegen::variables
