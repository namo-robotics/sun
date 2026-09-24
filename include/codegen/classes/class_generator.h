#pragma once

/** Translates analyzed Sun programs into LLVM instructions. */
namespace sun::codegen {
class CodegenVisitor;
}
/** Provides the registry of generated functions and their metadata. */
namespace sun::codegen::functions {
class FunctionRegistry;
}
/** Provides the generator for built-in operations. */
namespace sun::codegen::intrinsics {
class IntrinsicsGenerator;
}
/** Provides the scope manager responsible for variable storage and cleanup. */
namespace sun::codegen::scopes {
class ScopeManager;
}

// class_generator.h — Classes, interfaces, and generic instantiation
//
// Class and interface definitions become IR through:
//
//   classes     the struct layout, each method's signature and body, and the
//               method closure ABI that gives every method its receiver
//   interfaces  the vtables behind dynamic dispatch, and the fat pointer
//               { data, vtable } a class becomes when it is used as one; an
//               owning conversion moves the class into stable heap storage
//   generics    each specialization semantic analysis asked for, emitted
//               against the template's own definition scope
//
// It also owns reading and writing members — `obj.field`, `obj.method`, and
// a module's members, which are compile-time only and resolve to the globals
// their own declarations emitted.

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Value.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "ast.h"
#include "codegen/codegen_state.h"
#include "semantic_analysis/argument_conversion.h"
#include "semantic_analysis/type_registry.h"
#include "types/types.h"

/** Provides the generator for class storage and method operations. */
namespace sun::codegen::classes {
using sun::ast::ClassDefinitionAST;
using sun::ast::ExprAST;
using sun::semantic_analysis::DeclarationId;
using sun::types::ClassType;
using sun::types::InterfaceType;
using sun::types::TypePtr;

/**
 * Emits class, interface and enum definitions, member access, method bodies,
 * interface dispatch, and the specializations generics ask for.
 */
class ClassGenerator {
 public:
  /** Binds class generation to shared compiler state and the expression
   * visitor. */
  ClassGenerator(sun::codegen::CodegenState& state,
                 sun::codegen::CodegenVisitor& gen)
      : state_(state),
        gen_(gen),
        ctx(state.ctx),
        module(state.module),
        typeRegistry(state.typeRegistry),
        typeResolver(state.typeResolver),
        debugInfo(state.debugInfo),
        thisPtr(state.frame.thisPtr),
        currentClass(state.frame.currentClass),
        currentFunctionCanError(state.frame.canError),
        currentFunctionReturnsRef(state.frame.returnsRef),
        currentFunctionValueType(state.frame.valueType) {}

  /** Binds class generation to shared compiler state and the expression
   * visitor. */
  ClassGenerator(const ClassGenerator&) = delete;
  /** Disallows assignment so ownership and object identity cannot be
   * duplicated. */
  ClassGenerator& operator=(const ClassGenerator&) = delete;

  // ---------------------------------------------------------------
  // Definitions
  // ---------------------------------------------------------------

  /** Emits LLVM instructions for this syntax node and returns its generated
   * value. */
  llvm::Value* codegen(const ClassDefinitionAST& expr);
  /** Emits LLVM instructions for this syntax node and returns its generated
   * value. */
  llvm::Value* codegen(const sun::ast::InterfaceDefinitionAST& expr);

  /**
   * A class that arrived from precompiled bitcode: register its type and
   * emit only the generic specializations this program asked for.
   */
  llvm::Value* codegenPrecompiledClass(const ClassDefinitionAST& expr);

  /**
   * Declare the methods of a class a block defines — including each
   * specialization of a generic class — before any body is emitted
   */
  void declareBlockClassMethods(const ClassDefinitionAST& expr);

  // ---------------------------------------------------------------
  // Members and receivers
  // ---------------------------------------------------------------

  /** Emits LLVM instructions for this syntax node and returns its generated
   * value. */
  llvm::Value* codegen(const sun::ast::ThisExprAST& expr);
  /** Emits LLVM instructions for this syntax node and returns its generated
   * value. */
  llvm::Value* codegen(const sun::ast::MemberAccessAST& expr);
  /** Emits LLVM instructions for this syntax node and returns its generated
   * value. */
  llvm::Value* codegen(const sun::ast::MemberAssignmentAST& expr);
  /** Emits LLVM instructions for this syntax node and returns its generated
   * value. */
  llvm::Value* codegen(const sun::ast::StructLiteralAST& expr);

  /**
   * A module is compile-time only, so `mod.name` reads and writes the global
   * that the member's own declaration emitted.
   */
  llvm::GlobalVariable* moduleMemberGlobal(const ExprAST& object,
                                           DeclarationId id);

  // ---------------------------------------------------------------
  // Construction
  // ---------------------------------------------------------------

  /**
   * Build a class instance in a stack slot and run its constructor
   */
  llvm::Value* codegenStackClassInstance(const sun::ast::CallExprAST& expr,
                                         ClassType& classType);

  // ---------------------------------------------------------------
  // Generic instantiation
  // ---------------------------------------------------------------

  /** Emits LLVM instructions for this syntax node and returns its generated
   * value. */
  llvm::Value* codegen(const sun::ast::GenericCallAST& expr);

  /**
   * Generate a method body for an already-declared function
   */
  void generateMethodBody(const sun::ast::FunctionAST& methodFunc);

  /**
   * Declare a method function from a specialized AST (no body generated)
   */
  llvm::Function* declareMethodFromAST(
      const sun::ast::FunctionAST& specializedAST);

  // ---------------------------------------------------------------
  // Interface dispatch

  /** Converts an erased child value to an ancestor without copying its object.
   */
  llvm::Value* upcastInterface(llvm::Value* value, InterfaceType* source,
                               InterfaceType* target, bool borrowed);

  // ---------------------------------------------------------------

  /**
   * Creates a borrowed fat pointer { data_ptr, vtable_ptr }. The concrete
   * object remains in its current owner's storage.
   */
  llvm::Value* createInterfaceFatPointer(llvm::Value* objectPtr,
                                         ClassType* classType,
                                         InterfaceType* ifaceType);

  /**
   * Moves a concrete class into heap storage and creates an owning interface
   * fat pointer. The vtable's drop slot drops and frees that erased object.
   */
  llvm::Value* createOwnedInterfaceFatPointer(llvm::Value* objectPtr,
                                              ClassType* classType,
                                              InterfaceType* ifaceType);

  /**
   * Returns the vtable global for a (class, interface) pair, building it on
   * demand if the class was not codegen'd in this module (e.g. an stdlib error
   * class referenced only by a `throw`). Missing methods are declared as
   * externals resolved from the defining module at link/JIT time.
   */
  llvm::GlobalVariable* getOrCreateInterfaceVtable(ClassType* classType,
                                                   InterfaceType* ifaceType);

  /**
   * Prepares a class argument for a ref Interface parameter by creating a
   * fat pointer on the stack. Returns nullptr if not a class->ref Interface
   * conversion, otherwise returns pointer to the fat pointer on stack.
   */
  llvm::Value* prepareClassForRefInterface(llvm::Value* classPtr,
                                           TypePtr argType, TypePtr paramType);

 private:
  sun::codegen::CodegenState& state_;
  sun::codegen::CodegenVisitor& gen_;

  // Aliases into the shared state, so the emission code reads the same way
  // the rest of codegen does
  sun::codegen::CodegenContext& ctx;
  llvm::Module* module;
  std::shared_ptr<sun::semantic_analysis::TypeRegistry>& typeRegistry;
  sun::codegen::LLVMTypeResolver& typeResolver;
  sun::codegen::DebugInfoBuilder& debugInfo;

  // Classes that have actually been code-generated
  std::set<DeclarationId> codegenedClasses;

  // Class specializations from precompiled generics (library code).
  // These need codegen but shouldn't show in an IR dump.
  std::set<DeclarationId> librarySpecializations;

  /** Shared dispatch tables, keyed by concrete class and interface. */
  std::map<std::pair<DeclarationId, DeclarationId>, llvm::GlobalVariable*>
      vtableGlobals;

  /**
   * Emits the type-specific routine that destroys and frees an erased object.
   */
  llvm::Function* getOrCreateInterfaceDropFunction(ClassType* classType);

  /**
   * Declare every method of one class (no bodies)
   */
  void declareClassMethods(const ClassDefinitionAST& expr,
                           const std::shared_ptr<ClassType>& classType);

  /**
   * Method prologue: unwrap the receiver from the closure arg into a
   * 'this.addr' alloca, set the frame's thisPtr, and register "this" in scope.
   */
  void emitMethodPrologueThis(llvm::Function* func);

  /**
   * Generate constructor argument values as semantic analysis decided them.
   * Arg 0 is the method closure { ctorFunc, thisPtr }.
   */
  std::vector<llvm::Value*> generateCtorArgs(
      llvm::Function* ctorFunc, llvm::Value* thisPtr,
      const std::vector<std::unique_ptr<ExprAST>>& args,
      const std::vector<sun::semantic_analysis::ArgConversion>& conversions,
      const std::vector<TypePtr>& paramTypes);

  /**
   * Bound method reference: obj.method in value position (lambda-typed).
   */
  llvm::Value* codegenBoundMethodReference(
      const sun::ast::MemberAccessAST& expr, llvm::Value* objectPtr);

  // The frame currently being emitted; class codegen is what sets the
  // receiver and the return contract for a method body
  llvm::Value*& thisPtr;
  std::shared_ptr<ClassType>& currentClass;
  bool& currentFunctionCanError;
  bool& currentFunctionReturnsRef;
  llvm::Type*& currentFunctionValueType;

  /**
   * What class codegen borrows from the rest of codegen.
   * The BlockExprAST overload matters: without it a method body would bind to
   * codegen(const ExprAST&), which attaches an expression debug location the
   * block path does not want.
   */
  llvm::Value* codegen(const ExprAST& expr);
  /** Emits a block, optionally skipping a prefix already emitted by its caller.
   */
  llvm::Value* codegen(const sun::ast::BlockExprAST& block, size_t start = 0);

  /**
   * A node kind with its own overload must not silently bind to the
   * ExprAST forwarder above: that path attaches an expression debug location,
   * so a block routed through it changes DWARF output. Make it a compile
   * error instead. Add an overload here when a new kind is needed.
   */
  template <typename T>
    requires(!std::is_same_v<T, ExprAST> &&
             !std::is_same_v<T, sun::ast::BlockExprAST> &&
             std::is_base_of_v<ExprAST, T>)
  llvm::Value* codegen(const T&) = delete;

  /** Provides the scope manager responsible for variable storage and cleanup.
   */
  sun::codegen::scopes::ScopeManager& scopes();
  /** Provides the registry of generated functions and their metadata. */
  sun::codegen::functions::FunctionRegistry& functions();
  /** Provides the generator for built-in operations. */
  sun::codegen::intrinsics::IntrinsicsGenerator& intrinsics();
  /** Allocates local storage in the function entry block. */
  llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                           llvm::StringRef varName,
                                           llvm::Type* type);
  /** Associates generated parameter storage with its source declaration. */
  void debugDeclareParam(llvm::AllocaInst* alloca, const std::string& name,
                         const sun::ast::PrototypeAST& proto,
                         unsigned userArgIdx, unsigned argNoBase = 1);
  /** Emits the address of the receiver used for member operations. */
  std::pair<llvm::Value*, ClassType*> codegenObjectPtr(const ExprAST& object);
  /** Builds a callable value that retains the method receiver. */
  llvm::Value* materializeMethodClosure(llvm::Value* fnPtr,
                                        llvm::Value* receiverPtr,
                                        llvm::StringRef name);
  /** Pairs a method pointer with its receiver to form a callable value. */
  llvm::Value* materializeMethodClosureValue(llvm::Value* fnPtr,
                                             llvm::Value* receiverPtr);
  /** Emits argument values and applies the conversions selected by analysis. */
  bool emitCallArguments(
      const std::vector<std::unique_ptr<ExprAST>>& args,
      const std::vector<sun::semantic_analysis::ArgConversion>& conversions,
      const std::vector<TypePtr>& paramTypes, llvm::FunctionType* calleeTy,
      std::vector<llvm::Value*>& argValues, const std::string& calleeName,
      size_t firstArg = 0);
  /** Stores a generated value in the variable's allocated storage. */
  void assignToVariableSlot(llvm::Value* slot, llvm::Value* value,
                            const TypePtr& varType, const std::string& name);
  /** Reports whether a function is supplied by an already compiled library. */
  bool isPrecompiledFunction(const std::string& name);
};

}  // namespace sun::codegen::classes
