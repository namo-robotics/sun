#pragma once

// codegen_visitor.h — The AST walk that emits LLVM IR
//
// Nine things this class holds rather than is, each with its own header:
//
//   CodegenState        the module, the type registry, the type resolver,
//                       DWARF emission, and the frame being emitted
//   ScopeManager        the scope stack and every drop it has to write
//   FunctionRegistry    calling conventions, provenance, name lookup
//   ClassGenerator      classes, interfaces, enums, generic instantiation
//   FunctionGenerator   functions, lambdas, closures, returns
//   VariableGenerator   variables, lvalues, globals
//   LoopGenerator       loops and the jumps out of them
//   ErrorGenerator      throw, try/catch, calls that may unwind
//   IntrinsicsGenerator compiler intrinsics and libc built-ins
//
// They all share the one CodegenState by reference and reach each other back
// through this class, the way the semantic components share SemanticContext
// and reach back through SemanticAnalyzer.
//
// What is left here is the walk itself: the node dispatch, and the expression
// kinds that belong to no component in particular — literals, operators,
// calls, indexing and match. Its implementation is codegen_visitor.cpp
// (dispatch, literals, operators) plus src/codegen/expressions/.
//
// Rules that need no codegen state live outside the class so other passes can
// reach the same answers: sun::codegen::ops (support/scalar_ops.h) and
// sun::codegen::layout (support/struct_access.h).

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>

#include <map>
#include <set>
#include <type_traits>

#include "ast.h"                                      // Pure AST header with ASTNodeType
#include "codegen/abi/extern_c.h"                     // The extern "C" boundary
#include "codegen/classes/class_generator.h"          // Classes, interfaces, enums
#include "codegen/codegen.h"                          // CodegenContext
#include "codegen/codegen_state.h"                    // Shared state for one codegen run
#include "codegen/errors/error_generator.h"           // throw, try/catch, unwinding calls
#include "codegen/functions/function_generator.h"     // Functions, lambdas, closures
#include "codegen/functions/function_registry.h"      // Function lookup and conventions
#include "codegen/intrinsics/intrinsics_generator.h"  // Intrinsics, built-ins
#include "codegen/loops/loop_generator.h"             // Loops and their jumps
#include "codegen/scopes/scope_manager.h"             // Scope stack and drop emission
#include "codegen/support/type_checks.h"              // requireType / tryGetType helpers
#include "codegen/variables/variable_generator.h"     // Variables, lvalues and globals
#include "semantic_analysis/types.h"                  // Type system
#include "support/error.h"                            // Error handling

// Convert a condition value to i1 (non-zero test for numeric conditions)
llvm::Value* coerceCondToBool(CodegenContext& ctx, llvm::Value* condV);

/**
 * Traverses the AST and generates LLVM IR using the provided CodegenContext.
 */
class CodegenVisitor {
  // ---------------------------------------------------------------
  // State
  // ---------------------------------------------------------------

  // Everything this run shares: the module, the type registry, the type
  // resolver, DWARF emission, and the frame currently being emitted.
  CodegenState state_;

  // Names this class and its source files reach the shared state by. They
  // alias state_ — the state lives there, not here.
  CodegenContext& ctx;
  llvm::Module* module;
  std::shared_ptr<sun::TypeRegistry>& typeRegistry;
  LLVMTypeResolver& typeResolver;
  sun::DebugInfoBuilder& debugInfo;
  llvm::Value*& thisPtr;
  std::shared_ptr<sun::ClassType>& currentClass;
  bool& currentFunctionCanError;
  bool& currentFunctionReturnsRef;
  llvm::Type*& currentFunctionValueType;

  // ---------------------------------------------------------------
  // Components
  // ---------------------------------------------------------------

  // The scope stack for the function being emitted, and the drop code for
  // everything it owns. Container-shaped: scopes.back(), scopes.size().
  ScopeManager scopes{state_, *this};

  // Classes, interfaces, enums, and generic instantiation
  ClassGenerator classes{state_, *this};

  // Functions, lambdas, closures and returns
  FunctionGenerator functions_{state_, *this};

  // Variables: creation, reference, assignment, lvalues and globals
  VariableGenerator variables{state_, *this};

  // Loops, and the jumps out of them
  LoopGenerator loops{state_, *this};

  // throw, try/catch, and every call that may unwind
  ErrorGenerator errors{state_, *this};

  // Compiler intrinsics and libc built-ins; owns the thread helpers they need
  IntrinsicsGenerator intrinsics{state_, *this};

  // Everything specific to the `extern "C"` boundary: symbol renames, C ABI
  // signature lowering, and argument marshalling. See extern_c.h.
  sun::cabi::ExternCEmitter externC;

  // Calling conventions, where each function came from, and name lookup.
  // Declared after externC because it resolves renamed externs through it.
  FunctionRegistry functions{state_, externC};

 public:
  explicit CodegenVisitor(CodegenContext& ctx,
                          std::shared_ptr<sun::TypeRegistry> registry)
      : state_(ctx, std::move(registry)),
        ctx(state_.ctx),
        module(state_.module),
        typeRegistry(state_.typeRegistry),
        typeResolver(state_.typeResolver),
        debugInfo(state_.debugInfo),
        thisPtr(state_.frame.thisPtr),
        currentClass(state_.frame.currentClass),
        currentFunctionCanError(state_.frame.canError),
        currentFunctionReturnsRef(state_.frame.returnsRef),
        currentFunctionValueType(state_.frame.valueType),
        externC(ctx, ctx.mainModule.get()) {}

  // ---------------------------------------------------------------
  // What the driver calls
  // ---------------------------------------------------------------

  llvm::Value* codegen(const BlockExprAST& block);
  llvm::Value* codegen(const ExprAST& expr);

  // A node kind whose codegen lives on a component must be dispatched to that
  // component. Without this, the missing overload would bind to
  // codegen(const ExprAST&) instead — which re-enters the dispatch switch and
  // recurses until the stack runs out. Make it a compile error instead.
  template <typename T>
    requires(!std::is_same_v<T, ExprAST> && std::is_base_of_v<ExprAST, T>)
  llvm::Value* codegen(const T&) = delete;

  // Emit static initialization for globals that need it. Call after all
  // top-level codegen but before main runs.
  void emitStaticInitFunction() { variables.emitStaticInitFunction(); }

  // Run DIBuilder finalization; call after all codegen, before verifyModule.
  void finalizeDebugInfo() { debugInfo.finalize(); }

  // Snapshot the module's current function declarations.
  // Call after declareAvailableFunctions() but before codegen().
  void snapshotPrecompiledFunctions() { functions.snapshotPrecompiled(*module); }

  /// Get the set of user-defined function names (for IR filtering)
  const std::set<std::string>& getUserDefinedFunctions() const {
    return functions.userDefined();
  }

  /// Record a function as user-written, so an IR dump keeps it. The set is
  /// keyed by name, so a pass that renames a function has to say so.
  void noteUserDefinedFunction(const std::string& name) {
    functions.noteUserDefined(name);
  }

  // ---------------------------------------------------------------
  // The components, for each other
  // ---------------------------------------------------------------

  // The state every codegen component shares
  CodegenState& state() { return state_; }

  ScopeManager& scopeManager() { return scopes; }
  ClassGenerator& classGenerator() { return classes; }
  FunctionGenerator& functionGenerator() { return functions_; }
  VariableGenerator& variableGenerator() { return variables; }
  ErrorGenerator& errorGenerator() { return errors; }
  IntrinsicsGenerator& intrinsicsGenerator() { return intrinsics; }
  FunctionRegistry& functionRegistry() { return functions; }
  sun::cabi::ExternCEmitter& externCEmitter() { return externC; }

  // ---------------------------------------------------------------
  // Helpers the components share
  //
  // Public for that reason only — nothing outside src/codegen/ calls them.
  // ---------------------------------------------------------------

  // The node dispatch that codegen(const ExprAST&) wraps. Call codegen().
  llvm::Value* codegenExpression(const ExprAST& expr);

  // Payload enums: struct-valued like classes, handled by pointer
  static bool isPayloadEnum(const sun::TypePtr& t) {
    return t && t->isEnum() &&
           static_cast<const sun::EnumType*>(t.get())->hasPayload();
  }

  /**
   * Applies move semantics for class arguments passed by value.
   * Loads the struct value and zeros the source memory to prevent double-free.
   * If the argument is not a pointer to a class, returns it unchanged.
   */
  llvm::Value* applyMoveSemantics(llvm::Value* argVal, sun::TypePtr argSunType);

  /**
   * Materializes a struct or array return value to the caller's stack.
   * Functions return classes and sized arrays as LLVM aggregate values; using
   * the result (reaching a field, indexing) needs an addressable location.
   * Skips error unions { i1, T } and non-owning views { ptr, i32, ptr }.
   */
  llvm::Value* materializeStructReturn(llvm::Value* callResult);

  // The lvalue surface, forwarded to VariableGenerator so every component
  // reaches it the same way. tryCodegenAddress returns nullptr for shapes
  // with no addressable slot (class __index__ targets, slices, closure
  // captures, temporaries); codegenAddress throws instead.
  llvm::Value* tryCodegenAddress(const ExprAST& expr) {
    return variables.tryCodegenAddress(expr);
  }
  llvm::Value* codegenAddress(const ExprAST& expr) {
    return variables.codegenAddress(expr);
  }
  llvm::Value* codegenBorrowAddress(const ExprAST& expr) {
    return variables.codegenBorrowAddress(expr);
  }

  // Assign an already-evaluated value to a variable slot (local alloca or
  // global). Compound values (classes, payload enums) drop the overwritten
  // value first and MOVE the source in; self-assignment emits nothing.
  void assignToVariableSlot(llvm::Value* slot, llvm::Value* value,
                            const sun::TypePtr& varType,
                            const std::string& name) {
    variables.assignToVariableSlot(slot, value, varType, name);
  }

  // Codegen a member-access object down to (objectPtr, ClassType*), applying
  // the generic-`this` fixup and unwrapping raw_ptr/static_ptr/ref to class.
  // ClassType* is null when the object is not class-shaped.
  std::pair<llvm::Value*, sun::ClassType*> codegenObjectPtr(
      const ExprAST& object) {
    return variables.codegenObjectPtr(object);
  }

  // Method closure ABI: methods take a ptr to { ptr func, ptr env } as their
  // hidden first argument; env holds the receiver ('this'). Returns a ptr to
  // an entry-block alloca holding { fnPtr, receiverPtr } (stores emitted at
  // the current insert point, so loops don't grow the stack).
  llvm::Value* materializeMethodClosure(
      llvm::Value* fnPtr, llvm::Value* receiverPtr,
      llvm::StringRef name = "method.closure");

  // Closure struct VALUE { fnPtr, receiverPtr } via insertvalue (for method
  // references in value position).
  llvm::Value* materializeMethodClosureValue(llvm::Value* fnPtr,
                                             llvm::Value* receiverPtr);

  // Lower a call's arguments as semantic analysis decided (one ArgConversion
  // per argument), appending to argValues. The one argument loop for every
  // kind of call. `paramTypes` supplies the target type where a conversion
  // needs one, `calleeTy` the LLVM parameter types for closure values.
  // `firstArg` skips leading arguments the caller lowered itself.
  // Returns false if an argument failed to codegen.
  bool emitCallArguments(const std::vector<std::unique_ptr<ExprAST>>& args,
                         const std::vector<sun::ArgConversion>& conversions,
                         const std::vector<sun::TypePtr>& paramTypes,
                         llvm::FunctionType* calleeTy,
                         std::vector<llvm::Value*>& argValues,
                         const std::string& calleeName, size_t firstArg = 0);

  // Bring two scalar operands to a common type (int/float widening); throws
  // on incompatible types
  void unifyBinaryOperands(llvm::Value*& L, llvm::Value*& R,
                           const sun::TypePtr& lhsSunType,
                           const sun::TypePtr& rhsSunType, const Position& loc);

  // Emit an arithmetic/bitwise/shift op on unified operands; shared by
  // binary expressions and compound assignment
  llvm::Value* emitBinaryOp(TokenKind op, llvm::Value* L, llvm::Value* R,
                            bool unsignedOp, const Position& loc);

  // Integer division/remainder with signedness; shared by the plain binary
  // path and codegenSafeDivision
  llvm::Value* createIntDivRem(llvm::Value* L, llvm::Value* R, bool isModulo,
                               bool isUnsigned);

  // Variant access without arguments: i32 constant for payload-free enums,
  // tagged storage alloca for unit variants of payload enums
  llvm::Value* codegenEnumVariantAccess(sun::EnumType& enumType,
                                        const sun::EnumVariant& variant);

  // Element address for the slice-aware index form
  llvm::Value* codegenIndexElementPtr(const IndexAST& expr);

  // Class __index__/__setindex__ protocol pieces, decomposed so compound
  // assignment can box the indices and resolve the receiver exactly once.
  // The boxed indices are a `ref array<i64>` view value.
  llvm::Value* boxIndicesToArrayRef(const IndexAST& expr);
  llvm::Value* emitClassIndexCall(llvm::Value* objectPtr, llvm::Value* idxView,
                                  sun::ClassType* classType);
  llvm::Value* emitClassSetIndexCall(llvm::Value* objectPtr,
                                     llvm::Value* idxView, llvm::Value* value,
                                     sun::ClassType* classType);
  llvm::Function* declareIndexProtocolMethod(sun::ClassType* classType,
                                             const sun::ClassMethod& method,
                                             const std::string& mangledName,
                                             llvm::Type* valueParamType);

  // `arr.ndims()` and `arr.dim(i)` on a sized array or a view
  llvm::Value* codegenArrayQuery(const CallExprAST& call,
                                 const MemberAccessAST& member);

  // Views: the { ptr data, i32 ndims, ptr dims } value a sized array decays
  // to at a `ref array<T>` site. The dims table is a private constant global.
  llvm::Constant* arrayDimsTable(const std::vector<size_t>& dims);
  llvm::Value* emitArrayView(llvm::Value* storagePtr,
                             const std::vector<size_t>& dims);
  // The view value from however an unsized-array expression arrived: the
  // value itself, or a pointer to where it is stored
  llvm::Value* loadArrayView(llvm::Value* value);

  // Copy a sized array's inline storage from src to dest; a move also
  // invalidates the source so its own drop releases nothing
  void emitArrayTransfer(llvm::Value* dest, llvm::Value* src,
                         const sun::ArrayType& type, bool move);
  // Store one element into a slot of inline storage (compounds move in)
  void storeArrayElement(llvm::Value* slotPtr, llvm::Value* elemVal,
                         const sun::TypePtr& elemSunType,
                         llvm::Type* slotType);

  // An alloca in the function's entry block, so loops don't grow the stack
  AllocaInst* createEntryBlockAlloca(Function* func, StringRef varName,
                                     llvm::Type* type = nullptr) {
    IRBuilder<> builder(&func->getEntryBlock(), func->getEntryBlock().begin());
    if (!type) type = Type::getDoubleTy(ctx.getContext());
    return builder.CreateAlloca(type, nullptr, varName);
  }

  // Check if a function was declared from precompiled bitcode
  bool isPrecompiledFunction(const std::string& name) const {
    return functions.isPrecompiled(name);
  }

  // Attach a #dbg_declare for a source parameter (no-op without -g).
  // DWARF argNo is 1-based; argNoBase is 2 for methods, whose slot 1 is the
  // artificial 'this'.
  void debugDeclareParam(llvm::AllocaInst* alloca, const std::string& name,
                         const PrototypeAST& proto, unsigned userArgIdx,
                         unsigned argNoBase = 1) {
    sun::TypePtr type =
        proto.hasResolvedParamTypes() &&
                userArgIdx < proto.getResolvedParamTypes().size()
            ? proto.getResolvedParamTypes()[userArgIdx]
            : nullptr;
    debugInfo.declareParameter(*ctx.builder, alloca, name, type,
                               proto.getLocation(), argNoBase + userArgIdx);
  }

 private:
  // ---------------------------------------------------------------
  // Literals and operators (codegen_visitor.cpp)
  // ---------------------------------------------------------------

  llvm::Value* codegen(const NumberExprAST& expr);
  llvm::Value* codegen(const CharLiteralAST& expr);
  llvm::Value* codegen(const StringLiteralAST& expr);
  llvm::Value* codegen(const UnaryExprAST& expr);
  llvm::Value* codegen(const BinaryExprAST& expr);

  // Short-circuit logical operators (and, or)
  llvm::Value* codegenLogicalOp(const BinaryExprAST& expr);

  // Widen an integer value to destTy; the source expression's Sun type
  // decides zero- vs sign-extension (unsigned -> zext).
  llvm::Value* extendInt(llvm::Value* value, llvm::Type* destTy,
                         const sun::TypePtr& sourceType);

  // ---------------------------------------------------------------
  // Conditionals, match and enum destructuring
  // ---------------------------------------------------------------

  llvm::Value* codegen(const IfExprAST& expr);
  llvm::Value* codegen(const TernaryExprAST& expr);
  llvm::Value* codegen(const MatchExprAST& expr);

  // Enum variant construction: EnumName.Variant(args...) -> storage alloca ptr
  llvm::Value* codegenEnumVariantConstruction(const CallExprAST& expr,
                                              sun::EnumType& enumType,
                                              const sun::EnumVariant& variant);

  // Tag-switch match with payload destructuring
  llvm::Value* codegenEnumMatch(const MatchExprAST& expr,
                                sun::EnumType& enumType);

  // ---------------------------------------------------------------
  // Calls (call_expressions.cpp)
  // ---------------------------------------------------------------

  llvm::Value* codegen(const CallExprAST& expr);

  // Call helpers for different calling conventions
  llvm::Value* codegenFunctionCall(const CallExprAST& expr,
                                   const std::string& calleeName,
                                   const sun::FunctionType& funcType);
  llvm::Value* codegenLambdaCall(const CallExprAST& expr,
                                 const std::string& calleeName,
                                 const sun::LambdaType& lambdaType);

  // Top-level method call handler: dispatches to the sub-handlers below
  llvm::Value* codegenMethodCall(const CallExprAST& expr,
                                 const MemberAccessAST& memberAccess);

  // Handles builtin type methods: Thread.join(), static_ptr.length()/.raw().
  // Returns nullptr if not a builtin type method (caller should continue).
  llvm::Value* codegenBuiltinTypeMethod(const CallExprAST& expr,
                                        llvm::Value* objectPtr,
                                        sun::TypePtr objectType,
                                        const std::string& methodName);

  // Handles interface method dispatch via vtable
  llvm::Value* codegenInterfaceMethodCall(const CallExprAST& expr,
                                          llvm::Value* objectPtr,
                                          sun::InterfaceType* ifaceType,
                                          const std::string& methodName);

  // Handles class method dispatch (regular and generic)
  llvm::Value* codegenClassMethodCall(const CallExprAST& expr,
                                      llvm::Value* objectPtr,
                                      sun::ClassType* classType,
                                      const std::string& methodName,
                                      const MemberAccessAST* memberAccess);

  // Handles module-qualified function calls: mymod.foo()
  llvm::Value* codegenModuleFunctionCall(const CallExprAST& expr,
                                         sun::ModuleType* moduleType,
                                         const std::string& funcName,
                                         const MemberAccessAST& memberAccess);

  // Widens integer or float arguments to match parameter type if needed.
  // Handles i32->i64, f32->f64, etc. Returns the original value if no widening
  // needed.
  llvm::Value* widenNumericIfNeeded(llvm::Value* argVal,
                                    const sun::TypePtr& paramType,
                                    const sun::TypePtr& sourceType);

  // Narrows a static_ptr<T> fat { ptr, i64 } argument to the bare data
  // pointer a raw_ptr<T> parameter expects. No-op for any other type pairing.
  llvm::Value* coerceStaticPtrToRawPtr(llvm::Value* argVal,
                                       const sun::TypePtr& argSunType,
                                       const sun::TypePtr& paramType);

  // Generate the arguments for a call across the C boundary, carrying out
  // only Sun's own conversions. C-specific marshalling is ExternCEmitter's job.
  bool emitExternArguments(const CallExprAST& expr,
                           const std::vector<sun::TypePtr>& paramTypes,
                           std::vector<sun::cabi::PreparedArg>& out);

  // Emit a call to a C function whose signature needed ABI rewriting: its
  // LLVM parameters no longer line up one-to-one with the Sun arguments, so
  // the normal argument path cannot be used. Shared by plain and
  // module-qualified call sites.
  llvm::Value* emitMarshalledExternCall(
      const CallExprAST& expr, const std::vector<sun::TypePtr>& paramTypes,
      llvm::Function* func);

  // Read through a reference where a plain value is wanted. A reference is
  // an address until something consumes it; this is what does the consuming.
  llvm::Value* loadIfRef(llvm::Value* value, const sun::TypePtr& type);

  // Coerces a lambda argument to the callee's closure struct param type:
  // loads lambda literals (alloca ptr) and rebuilds closure values carrying
  // a differently-named but structurally identical struct type.
  llvm::Value* loadClosureForLambdaParam(llvm::Value* argVal,
                                         sun::TypePtr paramType,
                                         llvm::Type* expectedTy);

  // Field 0 (data) or 1 (length) of a static_ptr fat pointer, whether it
  // arrived as the struct value or as its address
  llvm::Value* extractStaticPtrField(llvm::Value* fatPtr, unsigned index,
                                     const sun::TypePtr& staticPtrType,
                                     const char* name);

  /**
   * Prepares an argument value for a reference parameter.
   * Handles variable references, member access, arrays, and raw_ptr auto-deref.
   */
  llvm::Value* prepareRefArgument(const ExprAST* argExpr,
                                  sun::TypePtr argSunType,
                                  bool allowTemporaryCopy = true);

  // ---------------------------------------------------------------
  // Arrays and indexing (arrays.cpp)
  // ---------------------------------------------------------------

  llvm::Value* codegen(const ArrayLiteralAST& expr);
  llvm::Value* codegen(const ArrayIndexAST& expr);  // Legacy
  llvm::Value* codegen(const IndexAST& expr);       // New slice-aware indexing
  llvm::Value* codegen(const IndexedAssignmentAST& expr);
  llvm::Value* codegenArrayElementPtr(const ArrayIndexAST& expr);

  // Class indexing via __index__ and __slice__ methods
  llvm::Value* codegenClassIndex(const IndexAST& expr, llvm::Value* objectPtr,
                                 sun::ClassType* classType);
  llvm::Value* codegenClassSlice(const IndexAST& expr, llvm::Value* objectPtr,
                                 sun::ClassType* classType);
  llvm::Value* codegenClassSetIndex(const IndexAST& indexExpr,
                                    const ExprAST* valueExpr,
                                    sun::ClassType* classType);

  // Attach a #dbg_declare for a user variable (no-op without -g)
  void debugDeclareLocal(llvm::AllocaInst* alloca, const std::string& name,
                         const sun::TypePtr& type, const Position& loc) {
    debugInfo.declareLocal(*ctx.builder, alloca, name, type, loc);
  }
};
