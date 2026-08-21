#pragma once

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>

#include <map>
#include <set>

#include "ast.h"                 // Your pure AST header with ASTNodeType
#include "codegen.h"             // Your CodegenContext definition
#include "debug_info_builder.h"  // DWARF emission (-g)
#include "error.h"               // Error handling
#include "extern_c.h"            // The extern "C" boundary
#include "llvm_type_resolver.h"  // LLVM type resolution
#include "thread_utils.h"        // Thread support utilities
#include "types.h"               // Type system

using NamedValueMap = std::map<std::string, llvm::AllocaInst*>;

// Convert a condition value to i1 (non-zero test for numeric conditions)
llvm::Value* coerceCondToBool(CodegenContext& ctx, llvm::Value* condV);

// Information about a heap allocation that needs automatic cleanup
struct OwnedAllocation {
  llvm::Value* ptrAlloca;  // Alloca storing the heap pointer
  std::string varName;     // Variable name (for debugging)
  bool moved;              // If true, ownership was transferred - don't free
  sun::TypePtr pointeeType;  // Type of the pointed-to object (for recursive
                             // field cleanup)
};

// Information about a stack value that needs drop code at scope exit:
// class instances (deinit + field recursion) or payload enums with owning
// payloads (synthesized drop function)
struct ClassAllocation {
  llvm::AllocaInst* alloca;  // Alloca storing the instance/storage
  std::string varName;       // Variable name (for debugging)
  bool moved;                // If true, ownership transferred - don't drop
  sun::TypePtr type;         // Class or payload-enum type
};

// Scope object containing variables and allocation tracking
struct CodegenScope {
  NamedValueMap variables;
  bool isFunctionBoundary = false;  // True for scopes marking function entry
  bool hasDebugScope = false;  // True when a DILexicalBlock was opened with it
  std::vector<OwnedAllocation> ownedAllocations;
  std::vector<ClassAllocation> classAllocations;
  // Names whose alloca holds a POINTER to the value rather than the value
  // itself (compound match-payload bindings borrow the payload slot in place)
  std::set<std::string> indirectBindings;
};

// Closure context for nested functions
struct ClosureContext {
  llvm::StructType* fatType;  // Only used for lambdas
  llvm::StructType* envType;
  llvm::Value* envOrFatPtr;  // Either env* (named functions) or fat* (lambdas)
  bool isDirectEnv;          // true: envOrFatPtr is env*, false: it's fat*
  std::vector<Capture> captures;
  std::map<std::string, unsigned> captureIndex;
  std::map<std::string, llvm::Type*> captureTypes;
};

// Closure info for a function - stored to know how to call it
struct FunctionClosureInfo {
  std::vector<Capture> captures;  // Names of captured variables in order
  bool hasClosure;  // Whether this function uses closure calling convention
};

// Result of declaring a function's LLVM signature
struct FuncDeclResult {
  llvm::Function* func;
  llvm::StructType* fatType;
  llvm::StructType* envType;
  llvm::Type* returnType;
  llvm::Type* valueType;  // Underlying type before error union wrapping
  bool canError;
};

// Loop context for break/continue statement codegen
struct LoopContext {
  llvm::BasicBlock* continueBlock;  // Block to jump to for 'continue'
  llvm::BasicBlock* breakBlock;     // Block to jump to for 'break'
  size_t cleanupDepth;  // Scope index of the loop body; break/continue emit
                        // cleanup for scopes at or above this depth
};

// Try block context for exception handling. Throwing calls made inside a try
// block are emitted as `invoke`s that unwind to this landing pad (or, when
// scopes with live owners must be cleaned first, to a per-call-site cleanup
// pad that drops them and then branches into dispatchBB).
struct TryContext {
  llvm::BasicBlock* landingPad;  // Plain landing pad (no owners to clean)
  llvm::BasicBlock* dispatchBB;  // Catch-clause dispatch (target of pads)
  llvm::PHINode* excPhi;         // Exception-pointer phi at dispatchBB entry
  size_t scopeDepth;             // Scope index of the try body scope
};

/**
 * Traverses the AST and generates LLVM IR using the provided CodegenContext.
 */
class CodegenVisitor {
  CodegenContext& ctx;
  llvm::Module* module;

  // Type registry for class/interface types (shared with semantic analyzer)
  std::shared_ptr<sun::TypeRegistry> typeRegistry;

  // Type resolver for sun::Type -> llvm::Type conversion
  LLVMTypeResolver typeResolver;

  // DWARF debug metadata emission; no-op unless -g
  sun::DebugInfoBuilder debugInfo;

  // Stack of closure contexts for nested function compilation
  std::vector<ClosureContext> closureStack;

  // Stack of loop contexts for break/continue handling
  std::vector<LoopContext> loopStack;

  // Stack of try contexts for error propagation to catch blocks
  std::vector<TryContext> tryStack;

  // Map from function name to its closure info (for calling)
  // Functions with hasClosure=false can be called directly
  std::map<std::string, FunctionClosureInfo> functionInfo;

  // Everything specific to the `extern "C"` boundary: symbol renames, C ABI
  // signature lowering, and argument marshalling. See extern_c.h.
  sun::cabi::ExternCEmitter externC;

  // Counter for generating unique names for anonymous lambdas
  unsigned lambdaCounter = 0;

  // Information about a global variable that needs runtime initialization
  struct StaticInitInfo {
    llvm::GlobalVariable* globalVar;  // The global variable
    std::string varName;              // Variable name (for diagnostics)
    sun::TypePtr varType;             // Variable type
    std::shared_ptr<sun::ClassType>
        classType;            // Class type (if class, else nullptr)
    const ExprAST* initExpr;  // The initialization expression
    Position location;        // Declaration site (for diagnostics)
  };

  // Queue of global variables that need runtime initialization
  // This includes class instances, function call results, etc.
  std::vector<StaticInitInfo> staticInits;

  // Track which classes have actually been code-generated
  std::set<std::string> codegenedClasses;

  // Track user-defined functions (for IR filtering - excludes library code)
  std::set<std::string> userDefinedFunctions;

  // Track class specializations from precompiled generics (library code)
  // These need codegen but shouldn't show in IR dump
  std::set<std::string> librarySpecializations;

  // Generic class AST registry: baseName -> ClassDefinitionAST
  std::map<std::string, const ClassDefinitionAST*> genericClassASTs;

  // Current 'this' pointer (set when compiling methods)
  llvm::Value* thisPtr = nullptr;

  // Current class being compiled (for method name resolution)
  std::shared_ptr<sun::ClassType> currentClass = nullptr;

  // Vtable globals for interface dispatch.
  // Key is (className, interfaceName), value is the vtable global.
  // Vtable contains function pointers for each interface method in declaration
  // order.
  std::map<std::pair<std::string, std::string>, llvm::GlobalVariable*>
      vtableGlobals;


  // Functions declared from precompiled bitcode (before codegen starts)
  // Used to distinguish library declarations from codegen-created forward decls
  std::set<std::string> precompiledFunctions_;

  // Thread support utilities (syscalls, types)
  ThreadUtils threadUtils;

 public:
  explicit CodegenVisitor(CodegenContext& ctx,
                          std::shared_ptr<sun::TypeRegistry> registry)
      : ctx(ctx),
        module(ctx.mainModule.get()),
        typeRegistry(std::move(registry)),
        typeResolver(ctx.getContext(), &ctx.mainModule->getDataLayout()),
        debugInfo(ctx.mainModule.get(), ctx.debugInfoEnabled()),
        externC(ctx, ctx.mainModule.get()),
        threadUtils(ctx, ctx.mainModule.get()) {}

  // Run DIBuilder finalization; call after all codegen, before verifyModule.
  void finalizeDebugInfo() { debugInfo.finalize(); }

  // Snapshot the module's current function declarations.
  // Call after declareAvailableFunctions() but before codegen().
  void snapshotPrecompiledFunctions() {
    for (auto& F : *module) {
      if (!F.getName().empty()) {
        precompiledFunctions_.insert(F.getName().str());
      }
    }
  }

  // Check if a function was declared from precompiled bitcode
  bool isPrecompiledFunction(const std::string& name) const {
    return precompiledFunctions_.count(name) > 0;
  }

  // Emit static initialization function for globals that need runtime init
  // Should be called after all top-level codegen but before main is called
  void emitStaticInitFunction();

  llvm::Value* codegen(const BlockExprAST& block);
  llvm::Value* codegen(const ExprAST& expr);
  std::pair<Function*, llvm::StructType*> codegen(
      const PrototypeAST& proto, llvm::StructType* envType, bool isLambda,
      llvm::Type* returnType = nullptr);
  llvm::Value* codegenFunc(FunctionAST& func);
  llvm::Value* codegenGenericFunc(FunctionAST& func);
  llvm::Value* codegenExternFunc(FunctionAST& func);
  FuncDeclResult declareFuncSignature(PrototypeAST& proto);
  llvm::Value* codegenLambda(LambdaAST& lambda);
  llvm::Value* codegen(const ForExprAST& expr);
  llvm::Value* codegen(const ForInExprAST& expr);
  llvm::Value* codegen(const WhileExprAST& expr);

  std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

  /// Get the set of user-defined function names (for IR filtering)
  const std::set<std::string>& getUserDefinedFunctions() const {
    return userDefinedFunctions;
  }

 private:
  // Expression handlers
  llvm::Value* codegen(const NumberExprAST& expr);
  llvm::Value* codegen(const StringLiteralAST& expr);
  llvm::Value* codegen(const VariableReferenceAST& expr);
  llvm::Value* codegen(const VariableAssignmentAST& expr);
  llvm::Value* codegen(const VariableCreationAST& expr);
  llvm::Value* codegen(const ReferenceCreationAST& expr);
  llvm::Value* codegen(const UnaryExprAST& expr);
  llvm::Value* codegen(const BinaryExprAST& expr);
  llvm::Value* codegen(const CallExprAST& expr);
  llvm::Value* codegen(const IfExprAST& expr);
  llvm::Value* codegen(const TernaryExprAST& expr);
  llvm::Value* codegen(const MatchExprAST& expr);
  llvm::Value* codegen(const ReturnExprAST& expr);
  llvm::Value* codegen(const BreakAST& expr);
  llvm::Value* codegen(const ContinueAST& expr);

  // Call helpers for different calling conventions
  llvm::Value* codegenFunctionCall(const CallExprAST& expr,
                                   const std::string& calleeName,
                                   const sun::FunctionType& funcType);
  llvm::Value* codegenLambdaCall(const CallExprAST& expr,
                                 const std::string& calleeName,
                                 const sun::LambdaType& lambdaType);

  // Method call dispatch helpers (in call_expressions.cpp)
  // Top-level method call handler: dispatches to appropriate sub-handler
  // Payload enums: struct-valued like classes, handled by pointer
  static bool isPayloadEnum(const sun::TypePtr& t) {
    return t && t->isEnum() &&
           static_cast<const sun::EnumType*>(t.get())->hasPayload();
  }

  // Enum variant construction: EnumName.Variant(args...) -> storage alloca ptr
  llvm::Value* codegenEnumVariantConstruction(const CallExprAST& expr,
                                              sun::EnumType& enumType,
                                              const sun::EnumVariant& variant);

  // Variant access without arguments: i32 constant for payload-free enums,
  // tagged storage alloca for unit variants of payload enums
  llvm::Value* codegenEnumVariantAccess(sun::EnumType& enumType,
                                        const sun::EnumVariant& variant);

  // Tag-switch match with payload destructuring
  llvm::Value* codegenEnumMatch(const MatchExprAST& expr,
                                sun::EnumType& enumType);

  llvm::Value* codegenMethodCall(const CallExprAST& expr,
                                 const MemberAccessAST& memberAccess);

  // Handles builtin type methods: Thread.join(), array.shape(), ptr._get(),
  // etc. Returns nullptr if not a builtin type method (caller should continue).
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

  // Look up an LLVM Function for a class method by name.
  // Uses getMangledMethodName (with paramSuffix) first, falls back to plain
  // "TypeName_methodName" for legacy/simple cases.
  llvm::Function* findClassMethod(
      const std::shared_ptr<sun::ClassType>& classType,
      const std::string& typeName, const std::string& methodName);

  // Handles module-qualified function calls: mymod.foo()
  llvm::Value* codegenModuleFunctionCall(const CallExprAST& expr,
                                         sun::ModuleType* moduleType,
                                         const std::string& funcName,
                                         const MemberAccessAST& memberAccess);

  // Class codegen
  llvm::Value* codegen(const ClassDefinitionAST& expr);
  llvm::Value* codegenPrecompiledClass(const ClassDefinitionAST& expr,
                                       const std::string& className);
  llvm::Value* codegen(const ThisExprAST& expr);
  llvm::Value* codegen(const MemberAccessAST& expr);
  llvm::Value* codegen(const MemberAssignmentAST& expr);
  llvm::Value* codegenStackClassInstance(const CallExprAST& expr,
                                         const std::string& className,
                                         sun::ClassType& classType);

  // Declare a method function from a specialized AST (no body generated)
  llvm::Function* declareMethodFromAST(const FunctionAST& specializedAST,
                                       const std::string& mangledName);

  // Declare every method of one class (no bodies)
  void declareClassMethods(const ClassDefinitionAST& expr,
                           const std::shared_ptr<sun::ClassType>& classType);

  // Declare the methods of a class a block defines — including each
  // specialization of a generic class — before any body is emitted
  void declareBlockClassMethods(const ClassDefinitionAST& expr);

  // Declare one function signature, body to follow
  void forwardDeclareFunction(const PrototypeAST& proto);

  // Declare every function and method a block defines, before any body is
  // emitted, so calls may name things defined further down the block
  void declareBlockSignatures(const BlockExprAST& block);

  // Generate a method body for an already-declared function
  void generateMethodBody(const FunctionAST& methodFunc,
                          const std::string& mangledName);

  // Enum codegen
  llvm::Value* codegen(const EnumDefinitionAST& expr);

  // Generate constructor argument values, handling ref parameters correctly.
  // Arg 0 is the method closure { ctorFunc, thisPtr }.
  std::vector<llvm::Value*> generateCtorArgs(
      llvm::Function* ctorFunc, llvm::Value* thisPtr,
      const std::vector<std::unique_ptr<ExprAST>>& args,
      const std::vector<sun::TypePtr>& paramTypes);

  // Result of constructor lookup - contains method info and mangled name
  struct ConstructorLookup {
    const sun::ClassMethod* method = nullptr;
    std::string mangledName;
    bool found() const { return method != nullptr || !mangledName.empty(); }
  };

  // Look up a constructor (init method) that matches the given argument types
  // Returns the matching method info and mangled name for codegen
  ConstructorLookup lookupConstructor(
      sun::ClassType* classType,
      const std::vector<std::unique_ptr<ExprAST>>& args);

  // Overload for pre-collected argument types
  ConstructorLookup lookupConstructor(sun::ClassType* classType,
                                      const std::vector<sun::TypePtr>& argTypes);

  // Interface dynamic dispatch support
  // Creates a fat pointer { data_ptr, vtable_ptr } for passing a class instance
  // to an interface-typed parameter.
  llvm::Value* createInterfaceFatPointer(llvm::Value* objectPtr,
                                         sun::ClassType* classType,
                                         sun::InterfaceType* ifaceType);

  // Returns the vtable global for a (class, interface) pair, building it on
  // demand if the class was not codegen'd in this module (e.g. an stdlib error
  // class referenced only by a `throw`). Missing methods are declared as
  // externals resolved from the defining module at link/JIT time.
  llvm::GlobalVariable* getOrCreateInterfaceVtable(
      sun::ClassType* classType, sun::InterfaceType* ifaceType);

  // Converts a class argument to an interface fat pointer if the parameter
  // expects an interface. Returns the original value if no conversion needed.
  llvm::Value* convertToInterfaceIfNeeded(llvm::Value* argVal,
                                          sun::TypePtr argType,
                                          sun::TypePtr paramType);

  // Prepares a class argument for a ref Interface parameter by creating a
  // fat pointer on the stack. Returns nullptr if not a class->ref Interface
  // conversion, otherwise returns pointer to the fat pointer on stack.
  llvm::Value* prepareClassForRefInterface(llvm::Value* classPtr,
                                           sun::TypePtr argType,
                                           sun::TypePtr paramType);

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

  // Find a function by its resolved Sun-side name, translating renamed
  // externs (`as "symbol"`) to the C symbol they were declared under.
  llvm::Function* lookupCallTarget(const std::string& name);

  // Build the argument list for a direct call, applying every parameter
  // coercion Sun performs at a call boundary. Shared by plain,
  // module-qualified and generic calls. Returns false if an argument failed
  // to codegen.
  bool buildDirectCallArgs(const std::vector<std::unique_ptr<ExprAST>>& args,
                           const std::vector<sun::TypePtr>& paramTypes,
                           llvm::Function* func,
                           std::vector<llvm::Value*>& argValues);

  // Generate the arguments for a call across the C boundary, applying only
  // Sun's own coercions. C-specific marshalling is ExternCEmitter's job.
  bool buildExternCallArgs(const CallExprAST& expr,
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

  // The node dispatch that codegen(const ExprAST&) wraps. Call codegen().
  llvm::Value* codegenExpression(const ExprAST& expr);

  // Widen an integer value to destTy; the source expression's Sun type
  // decides zero- vs sign-extension (unsigned -> zext). This is the single
  // place that owns that rule.
  llvm::Value* extendInt(llvm::Value* value, llvm::Type* destTy,
                         const sun::TypePtr& sourceType);

  // Compound assignment (lvalues.cpp): address-once -> load -> op -> store
  llvm::Value* codegen(const CompoundAssignmentAST& expr);
  llvm::Value* emitCompoundOpValue(const CompoundAssignmentAST& expr,
                                   llvm::Value* cur, llvm::Type* slotTy,
                                   const sun::TypePtr& slotSunType);

  // Lvalue facility (lvalues.cpp): compute the storage address of an
  // assignable expression. tryCodegenAddress returns nullptr for shapes with
  // no addressable slot (class __index__ targets, slices, closure captures,
  // temporaries); codegenAddress throws instead. Neither ever spills a value
  // to a temporary alloca.
  llvm::Value* tryCodegenAddress(const ExprAST& expr);
  llvm::Value* codegenAddress(const ExprAST& expr);
  // Same, plus conditional lvalues (`ref r = c ? a.x : b.y`), whose address
  // is a phi of the branches'. Only borrow bindings take that path.
  llvm::Value* codegenBorrowAddress(const ExprAST& expr);

  // Field pointer for a class member (shared by member read/write/address)
  llvm::Value* getFieldPtr(sun::ClassType* classType, llvm::Value* objectPtr,
                           const sun::ClassField& field,
                           const std::string& name);

  // Alignment for field accesses, honouring packed layout. Thin wrappers over
  // sun::packed (include/packed_layout.h) that supply the module's DataLayout.
  llvm::Align fieldAlign(const sun::ClassType* owner, llvm::Type* fieldTy);

  // Write a value into a storage slot, copying the struct when the slot is a
  // class (codegen of a class expression yields its address, not the struct).
  // `owner` is the enclosing class when the slot is a field, for packed
  // alignment; nullptr for a standalone slot.
  void storeIntoSlot(llvm::Value* dest, llvm::Value* value,
                     const sun::TypePtr& slotType,
                     const sun::ClassType* owner = nullptr);
  llvm::Align lvalueAlign(const ExprAST& target, llvm::Type* slotTy);

  // Assign an already-evaluated value to a variable slot (local alloca or
  // global). Compound values (classes, payload enums) drop the overwritten
  // value first and MOVE the source in; self-assignment emits nothing.
  void assignToVariableSlot(llvm::Value* slot, llvm::Value* value,
                            const sun::TypePtr& varType,
                            const std::string& name);

  // Codegen a member-access object down to (objectPtr, ClassType*), applying
  // the generic-`this` fixup and unwrapping raw_ptr/static_ptr/ref to class.
  // ClassType* is null when the object is not class-shaped.
  std::pair<llvm::Value*, sun::ClassType*> codegenObjectPtr(
      const ExprAST& object);

  // Class __index__/__setindex__ protocol pieces, decomposed so compound
  // assignment can box the indices and resolve the receiver exactly once
  llvm::AllocaInst* boxIndicesToArrayRef(const IndexAST& expr);
  llvm::Value* emitClassIndexCall(llvm::Value* objectPtr,
                                  llvm::AllocaInst* idxArr,
                                  sun::ClassType* classType);
  llvm::Value* emitClassSetIndexCall(llvm::Value* objectPtr,
                                     llvm::AllocaInst* idxArr,
                                     llvm::Value* value,
                                     sun::ClassType* classType);

  // Integer division/remainder with signedness; shared by the plain binary
  // path and codegenSafeDivision
  llvm::Value* createIntDivRem(llvm::Value* L, llvm::Value* R, bool isModulo,
                               bool isUnsigned);

  // Bring two scalar operands to a common type (int/float widening); throws
  // on incompatible types
  void unifyBinaryOperands(llvm::Value*& L, llvm::Value*& R,
                           const sun::TypePtr& lhsSunType,
                           const sun::TypePtr& rhsSunType,
                           const Position& loc);

  // Emit an arithmetic/bitwise/shift op on unified operands; shared by
  // binary expressions and compound assignment
  llvm::Value* emitBinaryOp(TokenKind op, llvm::Value* L, llvm::Value* R,
                            bool unsignedOp, const Position& loc);

  // Coerces a lambda argument to the callee's closure struct param type:
  // loads lambda literals (alloca ptr) and rebuilds closure values carrying
  // a differently-named but structurally identical struct type.
  llvm::Value* loadClosureForLambdaParam(llvm::Value* argVal,
                                         sun::TypePtr paramType,
                                         llvm::Type* expectedTy);

  // Method closure ABI: methods take a ptr to { ptr func, ptr env } as their
  // hidden first argument; env holds the receiver ('this'). Returns a ptr to
  // an entry-block alloca holding { fnPtr, receiverPtr } (stores emitted at
  // the current insert point, so loops don't grow the stack).
  llvm::Value* materializeMethodClosure(llvm::Value* fnPtr,
                                        llvm::Value* receiverPtr,
                                        llvm::StringRef name = "method.closure");

  // Closure struct VALUE { fnPtr, receiverPtr } via insertvalue (for method
  // references in value position).
  llvm::Value* materializeMethodClosureValue(llvm::Value* fnPtr,
                                             llvm::Value* receiverPtr);

  // Bound method reference: obj.method in value position (lambda-typed).
  llvm::Value* codegenBoundMethodReference(const MemberAccessAST& expr,
                                           llvm::Value* objectPtr,
                                           sun::ClassType* classType);

  // Method prologue: unwrap the receiver from the closure arg into a
  // 'this.addr' alloca, set thisPtr, and register "this" in scope.
  void emitMethodPrologueThis(Function* func);

  // Look up a method function by mangled name, declaring an external with
  // the closure ABI signature if not yet in the module.
  llvm::Function* getOrDeclareMethodFunction(
      const std::string& mangledName,
      const std::vector<sun::TypePtr>& paramTypes,
      const sun::TypePtr& returnType, bool canThrow);

  // Call classType's deinit() on receiver if it defines one (declares the
  // external on demand).
  void emitDeinitCall(const sun::ClassType* classType, llvm::Value* receiver);

  // Pointer intrinsics codegen (in pointers.cpp)
  llvm::Value* codegenSizeofIntrinsic(sun::TypePtr typeArg);
  llvm::Value* codegenInitIntrinsic(
      sun::TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenLoadIntrinsic(
      sun::TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenStoreIntrinsic(
      sun::TypePtr typeArg, const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenStaticPtrDataIntrinsic(
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenStaticPtrLenIntrinsic(
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenPtrAsRawIntrinsic(
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenAddressOfIntrinsic(
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenToRefIntrinsic(
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenIsIntrinsic(
      const std::string& targetName,
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenDeinitIntrinsic(
      sun::TypePtr typeArg,
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenLoadI64Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenStoreI64Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenMallocIntrinsic(const CallExprAST& expr);
  llvm::Value* codegenFreeIntrinsic(const CallExprAST& expr);
  llvm::Value* codegenMemcpyIntrinsic(const CallExprAST& expr);
  llvm::Value* codegenMemsetIntrinsic(const CallExprAST& expr);
  llvm::Value* codegenConvertIntrinsic(
      sun::TypePtr targetType,
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenBitcastIntrinsic(
      sun::TypePtr targetType,
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenPtrOffsetIntrinsic(const CallExprAST& expr);

  // Bit intrinsics (in intrinsics/bits.cpp)
  llvm::Value* codegenMulHiU64Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenCountZerosIntrinsic(const CallExprAST& expr,
                                          bool leading);

  // Atomic intrinsics (in intrinsics.cpp)
  llvm::Value* codegenAtomicCmpxchgI32Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenAtomicStoreI32Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenAtomicLoadI32Intrinsic(const CallExprAST& expr);

  // Futex intrinsics (in intrinsics.cpp)
  llvm::Value* codegenFutexWaitIntrinsic(const CallExprAST& expr);
  llvm::Value* codegenFutexWakeIntrinsic(const CallExprAST& expr);

  // Pointer member access codegen (in pointers.cpp)
  llvm::Value* codegenStaticPtrMemberAccess(const MemberAccessAST& expr,
                                            sun::StaticPointerType* ptrType);
  llvm::Value* codegenRawPtrMemberAccess(const MemberAccessAST& expr,
                                         sun::RawPointerType* ptrType);

  // Array codegen (in arrays.cpp)
  llvm::Value* codegen(const ArrayLiteralAST& expr);
  llvm::Value* codegen(const StructLiteralAST& expr);
  llvm::Value* codegen(const ArrayIndexAST& expr);  // Legacy
  llvm::Value* codegen(const IndexAST& expr);       // New slice-aware indexing
  llvm::Value* codegen(const IndexedAssignmentAST& expr);
  llvm::Value* codegenArrayElementPtr(const ArrayIndexAST& expr);
  llvm::Value* codegenIndexElementPtr(const IndexAST& expr);  // New
  llvm::Value* codegenArrayShape(const MemberAccessAST& expr);
  // Class indexing via __index__ and __slice__ methods
  llvm::Value* codegenClassIndex(const IndexAST& expr, llvm::Value* objectPtr,
                                 sun::ClassType* classType);
  llvm::Value* codegenClassSlice(const IndexAST& expr, llvm::Value* objectPtr,
                                 sun::ClassType* classType);
  llvm::Value* codegenClassSetIndex(const IndexAST& indexExpr,
                                    const ExprAST* valueExpr,
                                    sun::ClassType* classType);

  // Error handling codegen (try/catch/throw)
  llvm::Value* codegen(const TryCatchExprAST& expr);
  llvm::Value* codegen(const ThrowExprAST& expr);

  // Unsafe block codegen (pass-through to body)
  llvm::Value* codegen(const UnsafeBlockAST& expr);

  // LLVM Exception Handling helpers
  // Get or declare C++ ABI exception handling functions
  llvm::FunctionCallee getCxaAllocateException();
  llvm::FunctionCallee getCxaThrow();
  llvm::FunctionCallee getCxaBeginCatch();
  llvm::FunctionCallee getCxaEndCatch();
  llvm::FunctionCallee getCxaRethrow();
  llvm::Constant* getPersonalityFunction();
  llvm::Constant* getSunExceptionTypeInfo();

  // Ensure `fn` has a personality function set (needed for any function that
  // contains an invoke/landingpad). Idempotent.
  void ensurePersonality(llvm::Function* fn);

  // Emit __cxa_throw(excPtr, tinfo, null) (as an invoke to the innermost try's
  // landing pad if inside a try, else a plain call), terminate the current
  // block with unreachable, and leave the builder in a fresh dead block.
  void emitCxaThrowAndUnreachable(llvm::Value* excPtr);

  // Emit a call that may unwind. If `canThrow` and we are inside a try block,
  // emits an `invoke` unwinding to the innermost try's landing pad and
  // continues codegen in the normal-dest block; otherwise emits a plain call.
  llvm::Value* emitPossiblyThrowingCall(llvm::FunctionType* fnTy,
                                        llvm::Value* callee,
                                        llvm::ArrayRef<llvm::Value*> args,
                                        bool canThrow, const llvm::Twine& name);
  llvm::Value* emitPossiblyThrowingCall(llvm::FunctionCallee callee,
                                        llvm::ArrayRef<llvm::Value*> args,
                                        bool canThrow, const llvm::Twine& name);

  // Generic function call codegen: create<T>(allocator, args...)
  llvm::Value* codegen(const GenericCallAST& expr);

  // Safe arithmetic: returns error on division by zero
  llvm::Value* codegenSafeDivision(llvm::Value* L, llvm::Value* R,
                                    bool isModulo = false,
                                    bool isUnsigned = false);

  // Short-circuit logical operators (and, or)
  llvm::Value* codegenLogicalOp(const BinaryExprAST& expr);

  // Interface codegen
  llvm::Value* codegen(const InterfaceDefinitionAST& expr);

  std::vector<CodegenScope> scopes;

  CodegenScope& pushScope() {
    scopes.emplace_back();
    return scopes.back();
  }

  // Scope for a source block (if/else, loop, try/catch): also opens a
  // DILexicalBlock so debuggers see block-accurate variable visibility
  // (no-op without -g). popScope() closes it symmetrically.
  CodegenScope& pushScope(const Position& loc) {
    auto& scope = pushScope();
    scope.hasDebugScope = debugInfo.pushLexicalBlock(*ctx.builder, loc);
    return scope;
  }

  void popScope() {
    if (scopes.empty()) return;
    // Run this scope's pending drops unless the block already terminated
    // (return/break/throw paths emit their own multi-scope cleanup first).
    llvm::BasicBlock* bb = ctx.builder->GetInsertBlock();
    if (bb && !bb->getTerminator()) {
      emitCleanupForScope(scopes.back());
    }
    if (scopes.back().hasDebugScope) debugInfo.popLexicalBlock();
    scopes.pop_back();
  }

  // Index of the innermost function-boundary scope. Falls back to the
  // innermost scope (old single-scope cleanup behavior) if none is marked,
  // so an unmarked context can never emit references into another function.
  size_t functionBoundaryDepth() const {
    for (size_t i = scopes.size(); i-- > 0;) {
      if (scopes[i].isFunctionBoundary) return i;
    }
    return scopes.empty() ? 0 : scopes.size() - 1;
  }

  // Saved insertion point for restoring after nested codegen
  struct SavedInsertPoint {
    llvm::BasicBlock* block = nullptr;
    llvm::BasicBlock::iterator point;
    bool valid = false;
  };

  // Stack of saved insertion points for nested function/lambda codegen
  std::vector<SavedInsertPoint> insertPointStack;

  // Save current builder insertion point (before nested function/lambda
  // codegen)
  void saveInsertPoint() {
    SavedInsertPoint saved;
    saved.block = ctx.builder->GetInsertBlock();
    if (saved.block) {
      saved.point = ctx.builder->GetInsertPoint();
      saved.valid = true;
    }
    insertPointStack.push_back(saved);
  }

  // Restore builder insertion point (after nested function/lambda codegen)
  void restoreInsertPoint() {
    if (insertPointStack.empty()) return;
    const auto& saved = insertPointStack.back();
    if (saved.valid && saved.block) {
      ctx.builder->SetInsertPoint(saved.block, saved.point);
    }
    insertPointStack.pop_back();
  }

  // Emit cleanup code for all owned allocations and class variables from the
  // innermost scope down to the innermost function boundary (used by return
  // paths and function ends).
  // For ptr<T>: frees the allocation, recursively freeing ptr<T> fields
  // if T is a class For class variables: calls deinit() method if it exists,
  // recursively deinits class fields
  void emitScopeCleanup();

  // Emit cleanup for a single scope's allocations (LIFO), without popping it
  void emitCleanupForScope(CodegenScope& scope);

  // Emit cleanup for all scopes from the innermost down to index `depth`
  // (inclusive), without popping any. Used by break/continue/throw paths that
  // jump out of several scopes at once.
  void emitCleanupToDepth(size_t depth);

  // True if any scope at or above `depth` holds a live (non-moved) owner —
  // i.e. unwinding past this point would need cleanup
  bool hasLiveOwners(size_t depth) const {
    for (size_t i = depth; i < scopes.size(); ++i) {
      for (const auto& a : scopes[i].classAllocations)
        if (!a.moved) return true;
      for (const auto& a : scopes[i].ownedAllocations)
        if (!a.moved) return true;
    }
    return false;
  }

  // Helper: emit cleanup code for ptr<T> and raw_ptr<T> fields in a class
  // Recursively frees pointer fields before the containing object is freed
  // Also frees raw_ptr<T> fields (used for dynamic data in classes)
  void emitFieldCleanup(llvm::Value* objectPtr, const sun::ClassType* classType,
                        const std::string& baseName,
                        llvm::FunctionCallee freeFunc);

  // Helper: emit deinit calls for class fields that have deinit methods
  // Recursively calls deinit on nested class fields; enum-typed fields with
  // owning payloads are dropped through their synthesized drop function
  void emitFieldDeinit(llvm::Value* objectPtr, const sun::ClassType* classType,
                       const std::string& baseName);

  // Get or emit the synthesized drop function for a payload enum with owning
  // payloads: `void __sun_enum_drop$<Enum>(ptr storage)` switches on the tag,
  // drops each owning payload, then poisons the tag so a second drop is a
  // no-op. Returns nullptr when the enum needs no drop code.
  llvm::Function* getOrCreateEnumDropFunction(sun::EnumType& enumType);

  // Emit a drop of the payload-enum storage at `storagePtr` (no-op when the
  // enum needs no drop code)
  void emitEnumDrop(sun::EnumType& enumType, llvm::Value* storagePtr);

  // Drop whatever value of `type` lives at `ptr`, in place: class deinit +
  // field recursion, or the enum drop function. No-op for other types.
  void emitDropInPlace(const sun::TypePtr& type, llvm::Value* ptr,
                       const std::string& name = "drop");

  // Track a new owned allocation in current scope
  void trackOwnedAllocation(llvm::Value* ptrAlloca, const std::string& name,
                            sun::TypePtr pointeeType = nullptr) {
    if (!scopes.empty()) {
      scopes.back().ownedAllocations.push_back(
          {ptrAlloca, name, false, pointeeType});
    }
  }

  // Track a new class or payload-enum allocation in current scope for
  // automatic drop at scope exit. Enums are tracked only when they actually
  // need drop code. An alloca already tracked (e.g. a constructor temporary
  // later adopted by a variable) keeps its single entry — double-tracking
  // would double-drop.
  void trackClassAllocation(llvm::AllocaInst* alloca, const std::string& name,
                            sun::TypePtr type) {
    if (scopes.empty()) return;
    if (type && type->isEnum() && !sun::typeNeedsDrop(type)) return;
    for (auto& scope : scopes) {
      for (auto& alloc : scope.classAllocations) {
        if (alloc.alloca == alloca) {
          alloc.varName = name;  // adopt the variable's name for diagnostics
          return;
        }
      }
    }
    scopes.back().classAllocations.push_back(
        {alloca, name, false, std::move(type)});
  }

  // Mark a class allocation as moved/deinited (don't auto-deinit at scope exit)
  void markClassAllocationAsDeinited(llvm::Value* alloca) {
    for (auto& scope : scopes) {
      for (auto& alloc : scope.classAllocations) {
        if (alloc.alloca == alloca) {
          alloc.moved = true;
          return;
        }
      }
    }
  }

  // Mark an allocation as moved (ownership transferred, don't free)
  void markAsMoved(const std::string& name) {
    for (auto& scope : scopes) {
      for (auto& alloc : scope.ownedAllocations) {
        if (alloc.varName == name) {
          alloc.moved = true;
          return;
        }
      }
    }
  }

  // True if `name` resolves (in the current function) to an indirect binding
  // — its alloca holds the value's address, not the value
  bool isIndirectBinding(const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      if (it->variables.count(name)) return it->indirectBindings.count(name);
      if (it->isFunctionBoundary) break;
    }
    return false;
  }

  // Storage address of a compound local: the alloca itself, or for an
  // indirect binding the pointer it holds
  llvm::Value* compoundStorageAddress(const std::string& name) {
    AllocaInst* alloca = findVariable(name);
    if (!alloca) return nullptr;
    if (isIndirectBinding(name)) {
      return ctx.builder->CreateLoad(
          llvm::PointerType::getUnqual(ctx.getContext()), alloca,
          name + ".borrow");
    }
    return alloca;
  }

  /**
   * Finds a variable in the current (last) scope.
   * Respects function boundaries - doesn't search past outer function scopes.
   * Variables from outer functions should be accessed via closures instead.
   */
  AllocaInst* findVariable(const std::string& name) {
    // Search from innermost scope to outermost
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto found = it->variables.find(name);
      if (found != it->variables.end()) {
        return found->second;
      }
      // Stop at function boundary - outer function scopes are inaccessible
      // (captured variables should be accessed via closure stack)
      if (it->isFunctionBoundary) {
        break;
      }
    }
    return nullptr;
  }


  /**
   * Loads a variable from the closure context if it exists.
   * @return llvm::LoadInst* if found, nullptr otherwise.
   */
  llvm::LoadInst* createLoadVarFromClosure(const std::string& name);

  // Address of a captured variable's storage: env slot for by-value
  // captures, the stored pointer for [ref x] captures. nullptr when name is
  // not a capture.
  llvm::Value* createCaptureSlotAddress(const std::string& name,
                                        llvm::Type** valueTypeOut = nullptr,
                                        bool* byRefOut = nullptr);

  // Env-slot initializer at closure creation: value for by-value captures,
  // referent address for [ref x] captures
  llvm::Value* computeCaptureInitValue(const Capture& cap);

  GlobalVariable* createGlobalVariable(const std::string& name,
                                       llvm::Type* type,
                                       llvm::Constant* initializer = nullptr);

  AllocaInst* createEntryBlockAlloca(Function* func, StringRef varName,
                                     llvm::Type* type = nullptr) {
    IRBuilder<> builder(&func->getEntryBlock(), func->getEntryBlock().begin());
    if (!type) type = Type::getDoubleTy(ctx.getContext());
    return builder.CreateAlloca(type, nullptr, varName);
  }

  // Attach a #dbg_declare for a user variable (no-op without -g)
  void debugDeclareLocal(llvm::AllocaInst* alloca, const std::string& name,
                         const sun::TypePtr& type, const Position& loc) {
    debugInfo.declareLocal(*ctx.builder, alloca, name, type, loc);
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

  /**
   * Finds alloca for variable in the local scopes and creates a load
   * instruction.
   * @return LoadInst* if found, nullptr otherwise.
   */
  llvm::LoadInst* createLoadForLocalVar(const std::string& name);

  /**
   * Finds global variable and creates a load instruction.
   * @return LoadInst* if found, nullptr otherwise.
   */
  llvm::LoadInst* createLoadForGlobalVar(const std::string& varName);

  /**
   * Loads the value from a reference variable.
   * Handles both direct aliases and indirect references (to globals).
   * @return Value* if found, nullptr otherwise.
   */
  llvm::Value* createLoadForRef(const std::string& varName,
                                const sun::ReferenceType& refType);

  /**
   * Stores a value through a reference variable.
   * Handles both direct aliases and indirect references (to globals).
   */
  void createStoreForRef(const std::string& varName,
                         const sun::ReferenceType& refType, llvm::Value* value);

  /**
   * Codegens a new local variable.
   * The variable must not be a function pointer or global constant.
   * @return Value* representing the variable's value.
   */
  llvm::Value* genLocalVar(const VariableCreationAST& expr,
                           llvm::Type* varType);

  /**
   * Applies move semantics for class arguments passed by value.
   * Loads the struct value and zeros the source memory to prevent double-free.
   * If the argument is not a pointer to a class, returns it unchanged.
   * @param argVal The argument value (pointer to class struct).
   * @param argSunType The Sun type of the argument.
   * @return The loaded struct value if class, otherwise argVal unchanged.
   */
  llvm::Value* applyMoveSemantics(llvm::Value* argVal, sun::TypePtr argSunType);

  /**
   * Materializes a struct return value to the caller's stack.
   * Functions return class types as LLVM struct values. To use the result
   * (access fields, call methods), we need an addressable location.
   * Skips error unions { i1, T } and array fat structs { ptr, i32, ptr }.
   * @param callResult The return value from a function call.
   * @return Pointer to stack-allocated copy if struct, otherwise unchanged.
   */
  llvm::Value* materializeStructReturn(llvm::Value* callResult);

  /**
   * Copies a returned array's data/dims to caller's stack.
   * Arrays returned by value have pointers to callee's stack which become
   * dangling after return. This allocates storage on the caller's stack
   * and copies the contents, returning a new fat struct with valid pointers.
   * @param arrayFat The array fat struct { ptr data, i32 ndims, ptr dims }
   * @param arrayType The Sun ArrayType for size calculation (must be sized)
   * @return New fat struct with caller's stack pointers
   */
  llvm::Value* copyArrayToCallerStack(llvm::Value* arrayFat,
                                      const sun::ArrayType* arrayType);

  /**
   * Prepares an argument value for a reference parameter.
   * Handles variable references, member access, arrays, and raw_ptr auto-deref.
   * @param argExpr The argument expression.
   * @param argSunType The Sun type of the argument.
   * @return Pointer value suitable for passing as a reference parameter.
   */
  llvm::Value* prepareRefArgument(const ExprAST* argExpr,
                                  sun::TypePtr argSunType,
                                  bool allowTemporaryCopy = true);

  // Emit a method call's user arguments against paramTypes (ref parameters
  // take the argument's address), appending to argValues after the closure.
  // Shared by plain and generic method calls. False if an argument failed.
  bool emitMethodArguments(const CallExprAST& expr,
                           const std::vector<sun::TypePtr>& paramTypes,
                           llvm::Function* callee,
                           std::vector<llvm::Value*>& argValues);

  /**
   * Codegens a new global array variable.
   * Creates global data storage and dims array, returns the fat struct
   * constant.
   * @return Constant* representing the global array fat struct.
   */
  llvm::Constant* genGlobalArray(const VariableCreationAST& expr);

  /**
   * Codegens a new global variable for a constant expression.
   * The expression must not be a function pointer or function literal.
   * @return Constant* representing the global variable.
   */
  llvm::Constant* genGlobalVarForConstantExpr(const VariableCreationAST& expr,
                                              llvm::Type* varType);

  /**
   * Codegens a global class variable.
   * Creates a zero-initialized global and queues ctor call for static init.
   * @return GlobalVariable* for the class instance.
   */
  llvm::GlobalVariable* genGlobalClassVar(const VariableCreationAST& expr,
                                          sun::ClassType& classType);

  /**
   * Codegens a global variable requiring runtime initialization.
   * Creates a zero-initialized global and queues init expr for static init.
   * @return GlobalVariable* for the variable.
   */
  llvm::GlobalVariable* genGlobalVarWithRuntimeInit(
      const VariableCreationAST& expr, llvm::Type* varType);

  /**
   * Codegens a new function variable.
   * The value must be a function literal.
   * @return Value* representing the function pointer or fat closure pointer
   */
  llvm::Value* genFunctionVariable(const VariableCreationAST& expr);

  llvm::StructType* createEnvTypeForFunc(const PrototypeAST& proto);
  llvm::StructType* createFatTypeForFunc(Function* func,
                                         llvm::StructType* envType,
                                         const PrototypeAST& proto);

  llvm::Value* createFatClosure(Function* func, StructType* fatType,
                                StructType* envType, const PrototypeAST& proto);

  llvm::Value* createEnvClosure(StructType* envType, const PrototypeAST& proto);

  // Built-in intrinsics (libc calls; see intrinsics/libc.h). The registry
  // and dispatcher live in src/codegen/intrinsics/builtins.cpp; the codegen
  // methods below live in the per-area files beside it.
  bool isBuiltinFunction(const std::string& name);
  llvm::Value* codegenBuiltin(const std::string& name, const CallExprAST& expr);

  // Print built-ins
  llvm::Value* codegenPrintI32(const CallExprAST& expr);
  llvm::Value* codegenPrintI64(const CallExprAST& expr);
  llvm::Value* codegenPrintF64(const CallExprAST& expr);
  llvm::Value* codegenPrintString(const CallExprAST& expr);
  llvm::Value* codegenPrintBytes(const CallExprAST& expr);
  llvm::Value* codegenPrintNewline();

  // File I/O built-ins
  llvm::Value* codegenFileOpen(const CallExprAST& expr);
  llvm::Value* codegenFileClose(const CallExprAST& expr);
  llvm::Value* codegenFileWrite(const CallExprAST& expr);
  llvm::Value* codegenFileRead(const CallExprAST& expr);

  // Extended file I/O built-ins
  llvm::Value* codegenLseek(const CallExprAST& expr);
  llvm::Value* codegenFstat(const CallExprAST& expr);
  llvm::Value* codegenFsync(const CallExprAST& expr);
  llvm::Value* codegenFtruncate(const CallExprAST& expr);
  llvm::Value* codegenUnlink(const CallExprAST& expr);
  llvm::Value* codegenRename(const CallExprAST& expr);
  llvm::Value* codegenMkdir(const CallExprAST& expr);
  llvm::Value* codegenRmdir(const CallExprAST& expr);
  llvm::Value* codegenWrite(const CallExprAST& expr);
  llvm::Value* codegenRead(const CallExprAST& expr);

  // Network socket built-ins
  llvm::Value* codegenSocket(const CallExprAST& expr);
  llvm::Value* codegenBind(const CallExprAST& expr);
  llvm::Value* codegenListen(const CallExprAST& expr);
  llvm::Value* codegenAccept(const CallExprAST& expr);
  llvm::Value* codegenConnect(const CallExprAST& expr);
  llvm::Value* codegenSend(const CallExprAST& expr);
  llvm::Value* codegenRecv(const CallExprAST& expr);
  llvm::Value* codegenShutdown(const CallExprAST& expr);
  llvm::Value* codegenSetSockOpt(const CallExprAST& expr);
  llvm::Value* codegenGetSockOpt(const CallExprAST& expr);

  // High-level IPv4 socket helpers (build sockaddr_in internally)
  llvm::Value* codegenBindIPv4(const CallExprAST& expr);
  llvm::Value* codegenConnectIPv4(const CallExprAST& expr);
  llvm::Value* codegenAcceptFd(const CallExprAST& expr);

  // -------------------------------------------------------------------
  // Thread support (uses ThreadUtils for syscalls and types)
  // -------------------------------------------------------------------

  /**
   * Generates IR for a spawn(lambda) expression.
   *
   * Emits code that:
   *   1. Allocates a stack for the child thread (via mmap)
   *   2. Allocates and initializes a ThreadContext struct
   *   3. Allocates space for the result value
   *   4. Copies the lambda's captures into the context
   *   5. Calls clone() to create the child thread
   *   6. In the child: jumps to the trampoline
   *   7. In the parent: builds and returns the ThreadHandle
   *
   * @param expr The SpawnExprAST containing the lambda to spawn.
   * @return ThreadHandle struct value for use with join().
   */
  llvm::Value* codegen(const SpawnExprAST& expr);

  /**
   * Generates IR for Thread<T>.join() method call.
   *
   * Emits code that:
   *   1. Extracts the ThreadContext pointer from the handle
   *   2. Loops on futex_wait until context->futex_word != 1
   *   3. Loads the result from context->result_slot
   *   4. Frees the thread's stack and context memory (munmap)
   *   5. Returns the result value
   *
   * After join() returns, the thread resources are fully cleaned up
   * and the handle should not be used again.
   *
   * @param threadHandle The ThreadHandle returned by spawn().
   * @param resultType Sun type of the thread's result (T in Thread<T>).
   * @return The value returned by the spawned lambda.
   */
  llvm::Value* codegenThreadJoin(llvm::Value* threadHandle,
                                 sun::TypePtr resultType);

  // Error handling context: tracks if current function can return errors
  bool currentFunctionCanError = false;

  // True while generating the body of a function declared to return `ref T`;
  // reference returns must return the referent's address
  bool currentFunctionReturnsRef = false;
  llvm::Type* currentFunctionValueType = nullptr;  // The T in {i1, T}
};