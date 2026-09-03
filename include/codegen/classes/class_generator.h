#pragma once

// class_generator.h — Classes, interfaces, enums, and generic instantiation
//
// Everything that turns a type definition into IR:
//
//   classes     the struct layout, each method's signature and body, and the
//               method closure ABI that gives every method its receiver
//   interfaces  the vtables behind dynamic dispatch, and the fat pointer
//               { data, vtable } a class becomes when it is used as one; an
//               owning conversion moves the class into stable heap storage
//   enums       the definition itself; the payload machinery lives in
//               enums.cpp and the drop glue in scope_manager.cpp
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
#include "semantic_analysis/types.h"

class CodegenVisitor;
class FunctionRegistry;
class IntrinsicsGenerator;
class ScopeManager;

/**
 * Emits class, interface and enum definitions, member access, method bodies,
 * interface dispatch, and the specializations generics ask for.
 */
class ClassGenerator {
 public:
  ClassGenerator(CodegenState& state, CodegenVisitor& gen)
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

  ClassGenerator(const ClassGenerator&) = delete;
  ClassGenerator& operator=(const ClassGenerator&) = delete;

  // ---------------------------------------------------------------
  // Definitions
  // ---------------------------------------------------------------

  llvm::Value* codegen(const ClassDefinitionAST& expr);
  llvm::Value* codegen(const InterfaceDefinitionAST& expr);
  llvm::Value* codegen(const EnumDefinitionAST& expr);

  // A class that arrived from precompiled bitcode: register its type and
  // emit only the generic specializations this program asked for.
  llvm::Value* codegenPrecompiledClass(const ClassDefinitionAST& expr,
                                       const std::string& className);

  // Declare the methods of a class a block defines — including each
  // specialization of a generic class — before any body is emitted
  void declareBlockClassMethods(const ClassDefinitionAST& expr);

  // ---------------------------------------------------------------
  // Members and receivers
  // ---------------------------------------------------------------

  llvm::Value* codegen(const ThisExprAST& expr);
  llvm::Value* codegen(const MemberAccessAST& expr);
  llvm::Value* codegen(const MemberAssignmentAST& expr);
  llvm::Value* codegen(const StructLiteralAST& expr);

  // A module is compile-time only, so `mod.name` reads and writes the global
  // that the member's own declaration emitted.
  llvm::GlobalVariable* moduleMemberGlobal(const ExprAST& object,
                                           const std::string& symbol);

  // ---------------------------------------------------------------
  // Construction
  // ---------------------------------------------------------------

  // Build a class instance in a stack slot and run its constructor
  llvm::Value* codegenStackClassInstance(const CallExprAST& expr,
                                         const std::string& className,
                                         sun::ClassType& classType);

  // Result of constructor lookup - contains method info and mangled name
  struct ConstructorLookup {
    const sun::ClassMethod* method = nullptr;
    std::string mangledName;
    bool found() const { return method != nullptr || !mangledName.empty(); }
  };

  // Look up a constructor (init method) that matches the given argument types
  ConstructorLookup lookupConstructor(
      sun::ClassType* classType,
      const std::vector<std::unique_ptr<ExprAST>>& args);

  // Overload for pre-collected argument types
  ConstructorLookup lookupConstructor(sun::ClassType* classType,
                                      const std::vector<sun::TypePtr>& argTypes);

  // ---------------------------------------------------------------
  // Generic instantiation
  // ---------------------------------------------------------------

  llvm::Value* codegen(const GenericCallAST& expr);

  // Generate a method body for an already-declared function
  void generateMethodBody(const FunctionAST& methodFunc,
                          const std::string& mangledName);

  // Declare a method function from a specialized AST (no body generated)
  llvm::Function* declareMethodFromAST(const FunctionAST& specializedAST,
                                       const std::string& mangledName);

  // ---------------------------------------------------------------
  // Interface dispatch
  // ---------------------------------------------------------------

  // Creates a borrowed fat pointer { data_ptr, vtable_ptr }. The concrete
  // object remains in its current owner's storage.
  llvm::Value* createInterfaceFatPointer(llvm::Value* objectPtr,
                                         sun::ClassType* classType,
                                         sun::InterfaceType* ifaceType);

  // Moves a concrete class into heap storage and creates an owning interface
  // fat pointer. The vtable's final slot drops and frees that erased object.
  llvm::Value* createOwnedInterfaceFatPointer(
      llvm::Value* objectPtr, sun::ClassType* classType,
      sun::InterfaceType* ifaceType);

  // Returns the vtable global for a (class, interface) pair, building it on
  // demand if the class was not codegen'd in this module (e.g. an stdlib error
  // class referenced only by a `throw`). Missing methods are declared as
  // externals resolved from the defining module at link/JIT time.
  llvm::GlobalVariable* getOrCreateInterfaceVtable(
      sun::ClassType* classType, sun::InterfaceType* ifaceType);

  // Prepares a class argument for a ref Interface parameter by creating a
  // fat pointer on the stack. Returns nullptr if not a class->ref Interface
  // conversion, otherwise returns pointer to the fat pointer on stack.
  llvm::Value* prepareClassForRefInterface(llvm::Value* classPtr,
                                           sun::TypePtr argType,
                                           sun::TypePtr paramType);

 private:
  CodegenState& state_;
  CodegenVisitor& gen_;

  // Aliases into the shared state, so the emission code reads the same way
  // the rest of codegen does
  CodegenContext& ctx;
  llvm::Module* module;
  std::shared_ptr<sun::TypeRegistry>& typeRegistry;
  LLVMTypeResolver& typeResolver;
  sun::DebugInfoBuilder& debugInfo;

  // Classes that have actually been code-generated
  std::set<std::string> codegenedClasses;

  // Class specializations from precompiled generics (library code).
  // These need codegen but shouldn't show in an IR dump.
  std::set<std::string> librarySpecializations;

  // Generic class AST registry: baseName -> ClassDefinitionAST
  std::map<std::string, const ClassDefinitionAST*> genericClassASTs;

  // Vtable globals for interface dispatch, keyed by (class, interface).
  // Each holds method pointers in declaration order, then the concrete drop
  // routine in its final slot.
  std::map<std::pair<std::string, std::string>, llvm::GlobalVariable*>
      vtableGlobals;

  // Borrowed interface vtables share method slots with owning vtables but end
  // in a no-op drop routine.
  std::map<std::pair<std::string, std::string>, llvm::GlobalVariable*>
      borrowedVtableGlobals;

  llvm::GlobalVariable* getOrCreateBorrowedInterfaceVtable(
      sun::ClassType* classType, sun::InterfaceType* ifaceType);

  // Emits the type-specific routine that destroys and frees an erased object.
  llvm::Function* getOrCreateInterfaceDropFunction(
      sun::ClassType* classType);

  // Declare every method of one class (no bodies)
  void declareClassMethods(const ClassDefinitionAST& expr,
                           const std::shared_ptr<sun::ClassType>& classType);

  // Method prologue: unwrap the receiver from the closure arg into a
  // 'this.addr' alloca, set the frame's thisPtr, and register "this" in scope.
  void emitMethodPrologueThis(llvm::Function* func);

  // Generate constructor argument values as semantic analysis decided them.
  // Arg 0 is the method closure { ctorFunc, thisPtr }.
  std::vector<llvm::Value*> generateCtorArgs(
      llvm::Function* ctorFunc, llvm::Value* thisPtr,
      const std::vector<std::unique_ptr<ExprAST>>& args,
      const std::vector<sun::ArgConversion>& conversions,
      const std::vector<sun::TypePtr>& paramTypes);

  // Bound method reference: obj.method in value position (lambda-typed).
  llvm::Value* codegenBoundMethodReference(const MemberAccessAST& expr,
                                           llvm::Value* objectPtr,
                                           sun::ClassType* classType);

  // The frame currently being emitted; class codegen is what sets the
  // receiver and the return contract for a method body
  llvm::Value*& thisPtr;
  std::shared_ptr<sun::ClassType>& currentClass;
  bool& currentFunctionCanError;
  bool& currentFunctionReturnsRef;
  llvm::Type*& currentFunctionValueType;

  // What class codegen borrows from the rest of codegen.
  // The BlockExprAST overload matters: without it a method body would bind to
  // codegen(const ExprAST&), which attaches an expression debug location the
  // block path does not want.
  llvm::Value* codegen(const ExprAST& expr);
  llvm::Value* codegen(const BlockExprAST& block);

  // A node kind with its own overload must not silently bind to the
  // ExprAST forwarder above: that path attaches an expression debug location,
  // so a block routed through it changes DWARF output. Make it a compile
  // error instead. Add an overload here when a new kind is needed.
  template <typename T>
    requires(!std::is_same_v<T, ExprAST> && !std::is_same_v<T, BlockExprAST> &&
             std::is_base_of_v<ExprAST, T>)
  llvm::Value* codegen(const T&) = delete;

  ScopeManager& scopes();
  FunctionRegistry& functions();
  IntrinsicsGenerator& intrinsics();
  llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                           llvm::StringRef varName,
                                           llvm::Type* type);
  void debugDeclareParam(llvm::AllocaInst* alloca, const std::string& name,
                         const PrototypeAST& proto, unsigned userArgIdx,
                         unsigned argNoBase = 1);
  llvm::Value* codegenEnumVariantAccess(sun::EnumType& enumType,
                                        const sun::EnumVariant& variant);
  std::pair<llvm::Value*, sun::ClassType*> codegenObjectPtr(
      const ExprAST& object);
  llvm::Value* materializeMethodClosure(llvm::Value* fnPtr,
                                        llvm::Value* receiverPtr,
                                        llvm::StringRef name);
  llvm::Value* materializeMethodClosureValue(llvm::Value* fnPtr,
                                             llvm::Value* receiverPtr);
  bool emitCallArguments(const std::vector<std::unique_ptr<ExprAST>>& args,
                         const std::vector<sun::ArgConversion>& conversions,
                         const std::vector<sun::TypePtr>& paramTypes,
                         llvm::FunctionType* calleeTy,
                         std::vector<llvm::Value*>& argValues,
                         const std::string& calleeName, size_t firstArg = 0);
  void assignToVariableSlot(llvm::Value* slot, llvm::Value* value,
                            const sun::TypePtr& varType,
                            const std::string& name);
  bool isPrecompiledFunction(const std::string& name);
};
