#pragma once

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>

#include <set>

#include "ast.h"                 // Your pure AST header with ASTNodeType
#include "codegen.h"             // Your CodegenContext definition
#include "error.h"               // Error handling
#include "llvm_type_resolver.h"  // LLVM type resolution
#include "types.h"               // Type system

using NamedValueMap = std::map<std::string, llvm::AllocaInst*>;

// Scope object containing variables
struct CodegenScope {
  NamedValueMap variables;
  bool isFunctionBoundary = false;  // True for scopes marking function entry
};

// Information about a heap allocation that needs automatic cleanup
struct OwnedAllocation {
  llvm::Value* ptrAlloca;  // Alloca storing the heap pointer
  std::string varName;     // Variable name (for debugging)
  bool moved;              // If true, ownership was transferred - don't free
  bool isMmap;             // If true, use munmap instead of free
  llvm::Value*
      sizeAlloca;  // For mmap: alloca storing the size (munmap needs it)
  sun::TypePtr pointeeType;  // Type of the pointed-to object (for recursive
                             // field cleanup)
};

// Information about a class instance that needs deinit at scope exit
struct ClassAllocation {
  llvm::AllocaInst* alloca;  // Alloca storing the class instance
  std::string varName;       // Variable name (for debugging)
  bool moved;                // If true, ownership transferred - don't deinit
  std::shared_ptr<sun::ClassType> type;  // Class type (for method lookup)
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

// Loop context for break/continue statement codegen
struct LoopContext {
  llvm::BasicBlock* continueBlock;  // Block to jump to for 'continue'
  llvm::BasicBlock* breakBlock;     // Block to jump to for 'break'
};

// Metadata for heap allocations (attached to LLVM values)
struct AllocationMetadata {
  bool isMmap;        // If true, use munmap instead of free
  llvm::Value* size;  // For mmap: the size value (munmap needs it)
};

// Try block context for error propagation
struct TryContext {
  llvm::BasicBlock* catchBlock;    // Block to jump to on error
  llvm::Value* errorResultAlloca;  // Alloca to store the error union result
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

  // Stack of closure contexts for nested function compilation
  std::vector<ClosureContext> closureStack;

  // Stack of loop contexts for break/continue handling
  std::vector<LoopContext> loopStack;

  // Stack of try contexts for error propagation to catch blocks
  std::vector<TryContext> tryStack;

  // Map from function name to its closure info (for calling)
  // Functions with hasClosure=false can be called directly
  std::map<std::string, FunctionClosureInfo> functionInfo;

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
  };

  // Queue of global variables that need runtime initialization
  // This includes class instances, function call results, etc.
  std::vector<StaticInitInfo> staticInits;

  // Stack of namespace names for qualified name generation
  std::vector<std::string> namespaceStack;

  // Using imports: map from simple name to mangled name
  // E.g., "Vec" -> "sun_Vec" when using sun.Vec
  std::map<std::string, std::string> usingImports;

  // Wildcard imports: list of module prefixes for wildcard using statements
  // E.g., "sun_" when using sun;
  std::vector<std::string> wildcardImports;

  // Prefix wildcard imports: list of (module prefix, name prefix) pairs
  // E.g., ("sun_", "Mat") for using sun.Mat* imports sun.Matrix, sun.MatrixView
  std::vector<std::pair<std::string, std::string>> prefixWildcardImports;

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

  // Allocation metadata for heap allocations (keyed by the pointer value)
  // Used to track whether a value came from mmap vs malloc for cleanup
  std::map<llvm::Value*, AllocationMetadata> allocationMetadata;

  // Vtable globals for interface dispatch.
  // Key is (className, interfaceName), value is the vtable global.
  // Vtable contains function pointers for each interface method in declaration
  // order.
  std::map<std::pair<std::string, std::string>, llvm::GlobalVariable*>
      vtableGlobals;

  // Stack of variadic argument expressions for pack expansion
  // Each entry is a (param_name, expressions) pair for a variadic call context
  std::vector<std::pair<std::string, std::vector<const ExprAST*>>>
      variadicArgsStack;

  // Library symbols from precompiled modules (for skipping pre-declared
  // specializations)
  const std::set<std::string>* librarySymbols_ = nullptr;

 public:
  explicit CodegenVisitor(CodegenContext& ctx,
                          std::shared_ptr<sun::TypeRegistry> registry)
      : ctx(ctx),
        module(ctx.mainModule.get()),
        typeRegistry(std::move(registry)),
        typeResolver(ctx.getContext()) {}

  // Set library symbols for checking pre-declared specializations
  void setLibrarySymbols(const std::set<std::string>* symbols) {
    librarySymbols_ = symbols;
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
  llvm::Value* codegenLambda(LambdaAST& lambda);
  llvm::Value* codegen(const ForExprAST& expr);
  llvm::Value* codegen(const ForInExprAST& expr);
  llvm::Value* codegen(const WhileExprAST& expr);

  // Namespace support
  void enterNamespace(const std::string& name) {
    namespaceStack.push_back(name);
  }
  void exitNamespace() {
    if (!namespaceStack.empty()) namespaceStack.pop_back();
  }
  std::string getMangledName(const std::string& name) const;

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

  // Generate a method body for an already-declared function
  void generateMethodBody(const FunctionAST& methodFunc,
                          const std::string& mangledName);

  // Enum codegen
  llvm::Value* codegen(const EnumDefinitionAST& expr);

  // Generate constructor argument values, handling ref parameters correctly
  std::vector<llvm::Value*> generateCtorArgs(
      llvm::Value* thisPtr, const std::vector<std::unique_ptr<ExprAST>>& args,
      const std::vector<sun::TypePtr>& paramTypes);

  // Interface dynamic dispatch support
  // Creates a fat pointer { data_ptr, vtable_ptr } for passing a class instance
  // to an interface-typed parameter.
  llvm::Value* createInterfaceFatPointer(llvm::Value* objectPtr,
                                         sun::ClassType* classType,
                                         sun::InterfaceType* ifaceType);

  // Converts a class argument to an interface fat pointer if the parameter
  // expects an interface. Returns the original value if no conversion needed.
  llvm::Value* convertToInterfaceIfNeeded(llvm::Value* argVal,
                                          sun::TypePtr argType,
                                          sun::TypePtr paramType);

  // Widens integer or float arguments to match parameter type if needed.
  // Handles i32->i64, f32->f64, etc. Returns the original value if no widening
  // needed.
  llvm::Value* widenNumericIfNeeded(llvm::Value* argVal,
                                    sun::TypePtr paramType);

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
  llvm::Value* codegenIsIntrinsic(
      const std::string& targetName,
      const std::vector<std::unique_ptr<ExprAST>>& args);
  llvm::Value* codegenLoadI64Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenStoreI64Intrinsic(const CallExprAST& expr);
  llvm::Value* codegenMallocIntrinsic(const CallExprAST& expr);
  llvm::Value* codegenFreeIntrinsic(const CallExprAST& expr);

  // Pointer member access codegen (in pointers.cpp)
  llvm::Value* codegenStaticPtrMemberAccess(const MemberAccessAST& expr,
                                            sun::StaticPointerType* ptrType);
  llvm::Value* codegenRawPtrMemberAccess(const MemberAccessAST& expr,
                                         sun::RawPointerType* ptrType);

  // Array codegen (in arrays.cpp)
  llvm::Value* codegen(const ArrayLiteralAST& expr);
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

  // Generic function call codegen: create<T>(allocator, args...)
  llvm::Value* codegen(const GenericCallAST& expr);

  // Safe arithmetic: returns error on division by zero
  llvm::Value* codegenSafeDivision(llvm::Value* L, llvm::Value* R);

  // Interface codegen
  llvm::Value* codegen(const InterfaceDefinitionAST& expr);

  std::vector<CodegenScope> scopes;

  // Stack of owned allocations per scope (for automatic cleanup)
  std::vector<std::vector<OwnedAllocation>> ownedAllocations;

  // Stack of class allocations per scope (for automatic deinit)
  std::vector<std::vector<ClassAllocation>> classAllocations;

  CodegenScope& pushScope() {
    scopes.emplace_back();
    ownedAllocations.emplace_back();  // Also push owned allocations scope
    classAllocations.emplace_back();  // Also push class allocations scope
    return scopes.back();
  }

  void popScope() {
    if (!scopes.empty()) scopes.pop_back();
    if (!ownedAllocations.empty()) ownedAllocations.pop_back();
    if (!classAllocations.empty()) classAllocations.pop_back();
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

  // Emit cleanup code for all owned allocations and class variables in current
  // scope For ptr<T>: frees the allocation, recursively freeing ptr<T> fields
  // if T is a class For class variables: calls deinit() method if it exists,
  // recursively deinits class fields
  void emitScopeCleanup();

  // Helper: emit cleanup code for ptr<T> and raw_ptr<T> fields in a class
  // Recursively frees pointer fields before the containing object is freed
  // Also frees raw_ptr<T> fields (used for dynamic data in classes)
  void emitFieldCleanup(llvm::Value* objectPtr, const sun::ClassType* classType,
                        const std::string& baseName,
                        llvm::FunctionCallee freeFunc);

  // Helper: emit deinit calls for class fields that have deinit methods
  // Recursively calls deinit on nested class fields
  void emitFieldDeinit(llvm::Value* objectPtr, const sun::ClassType* classType,
                       const std::string& baseName);

  // Track a new owned allocation in current scope
  void trackOwnedAllocation(llvm::Value* ptrAlloca, const std::string& name,
                            bool isMmap = false,
                            llvm::Value* sizeAlloca = nullptr,
                            sun::TypePtr pointeeType = nullptr) {
    if (!ownedAllocations.empty()) {
      ownedAllocations.back().push_back(
          {ptrAlloca, name, false, isMmap, sizeAlloca, pointeeType});
    }
  }

  // Track a new class allocation in current scope for automatic deinit
  void trackClassAllocation(llvm::AllocaInst* alloca, const std::string& name,
                            std::shared_ptr<sun::ClassType> type) {
    if (!classAllocations.empty()) {
      classAllocations.back().push_back({alloca, name, false, std::move(type)});
    }
  }

  // Mark an allocation as moved (ownership transferred, don't free)
  void markAsMoved(const std::string& name) {
    for (auto& scope : ownedAllocations) {
      for (auto& alloc : scope) {
        if (alloc.varName == name) {
          alloc.moved = true;
          return;
        }
      }
    }
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

  GlobalVariable* createGlobalVariable(const std::string& name,
                                       llvm::Type* type,
                                       llvm::Constant* initializer = nullptr);

  AllocaInst* createEntryBlockAlloca(Function* func, StringRef varName,
                                     llvm::Type* type = nullptr) {
    IRBuilder<> builder(&func->getEntryBlock(), func->getEntryBlock().begin());
    if (!type) type = Type::getDoubleTy(ctx.getContext());
    return builder.CreateAlloca(type, nullptr, varName);
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
   * Unwraps an error union value { i1 isError, T value }.
   * If the value is an error union and we're in an error-returning function,
   * generates error propagation code. Returns the unwrapped inner value.
   * If value is not an error union, returns it unchanged.
   */
  llvm::Value* unwrapErrorUnion(llvm::Value* value, llvm::Type* expectedType,
                                llvm::Function* func);

  /**
   * Unwraps an error union from a call result.
   * Handles both try-catch blocks and error-returning function contexts.
   * If value is not an error union, returns it unchanged.
   */
  llvm::Value* unwrapCallErrorUnion(llvm::Value* callResult);

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
                                  sun::TypePtr argSunType);

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

  llvm::Constant* createGlobalFatClosure(Function* func, StructType* fatType,
                                         const PrototypeAST& proto);

  // Built-in functions (raw syscalls, no libc)
  bool isBuiltinFunction(const std::string& name);
  llvm::Value* codegenBuiltin(const std::string& name, const CallExprAST& expr);

  // Raw x86_64 syscall: write(fd, buf, len) -> bytes_written
  llvm::Value* emitRawSyscallWrite(llvm::Value* fd, llvm::Value* buf,
                                   llvm::Value* len);

  // Print built-ins using raw syscalls
  llvm::Value* codegenPrintI32(const CallExprAST& expr);
  llvm::Value* codegenPrintI64(const CallExprAST& expr);
  llvm::Value* codegenPrintF64(const CallExprAST& expr);
  llvm::Value* codegenPrintString(const CallExprAST& expr);
  llvm::Value* codegenPrintBytes(const CallExprAST& expr);
  llvm::Value* codegenPrintNewline();

  // File I/O built-ins using raw syscalls
  llvm::Value* codegenFileOpen(const CallExprAST& expr);
  llvm::Value* codegenFileClose(const CallExprAST& expr);
  llvm::Value* codegenFileWrite(const CallExprAST& expr);
  llvm::Value* codegenFileRead(const CallExprAST& expr);

  // Error handling context: tracks if current function can return errors
  bool currentFunctionCanError = false;
  llvm::Type* currentFunctionValueType = nullptr;  // The T in {i1, T}
};