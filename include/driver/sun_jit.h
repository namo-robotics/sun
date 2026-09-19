#pragma once

#include <memory>

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITLink/EHFrameSupport.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/DebugObjectManagerPlugin.h"
#include "llvm/ExecutionEngine/Orc/EHFrameRegistrationPlugin.h"
#include "llvm/ExecutionEngine/Orc/EPCDebugObjectRegistrar.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/ExecutorProcessControl.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"

using namespace llvm;
/** Declares LLVM JIT types referenced by the execution interface. */
using namespace llvm::orc;

/** Coordinates compilation, dependency loading, linking, and program execution. */
namespace sun::driver {

/**
 * The ORC JIT behind `sun file.sun`: one dylib, host-targeted, resolving
 * unknown symbols from the compiler's own process. The object-linking layer
 * is JITLink on every platform, so the JIT has one linker's behavior to test
 * and one set of quirks to learn. (RuntimeDyld is not an option: its Mach-O
 * support is legacy and mishandles arm64 unwind sections.)
 */
class SunJIT {
 private:
  std::unique_ptr<ExecutionSession> ES;

  DataLayout DL;
  // Captured before JTMB is moved into the compile layer; codegen stamps it
  // onto the module so C ABI classification knows the target.
  Triple TT;
  MangleAndInterner Mangle;

  std::unique_ptr<ObjectLayer> ObjLayer;
  IRCompileLayer CompileLayer;

  JITDylib& MainJD;

  /**
   * Build the JITLink object layer. Eh-frame registration lets thrown Sun
   * errors unwind through JITed frames. (If exception interop proves
   * incomplete on Apple Silicon, the known next step is MachOPlatform with
   * the ORC runtime, which also registers compact-unwind info.) The debug
   * object plugin hands each JITed ELF object to gdb's JIT interface so -g
   * modules are debuggable under the JIT; it is a no-op when no debugger is
   * attached and for non-ELF objects.
   */
  static std::unique_ptr<ObjectLayer> makeObjectLayer(ExecutionSession& ES) {
    auto Layer = std::make_unique<ObjectLinkingLayer>(ES);
    Layer->addPlugin(std::make_unique<EHFrameRegistrationPlugin>(
        ES, std::make_unique<jitlink::InProcessEHFrameRegistrar>()));
    // The registrar calls straight into this process's copy of LLVM's gdb
    // loader, the same way the eh-frame registrar above does. Every object is
    // registered, not only those with debug sections, so gdb can still place
    // breakpoints by name and symbolize backtraces in programs run without -g.
    Layer->addPlugin(std::make_unique<DebugObjectManagerPlugin>(
        ES,
        std::make_unique<EPCDebugObjectRegistrar>(
            ES, ExecutorAddr::fromPtr(&llvm_orc_registerJITLoaderGDBWrapper)),
        /*RequireDebugSections=*/false, /*AutoRegisterCode=*/true));
    return Layer;
  }

 public:
  /** Creates a JIT engine using the execution session and target data layout. */
  SunJIT(std::unique_ptr<ExecutionSession> ES, JITTargetMachineBuilder JTMB,
         DataLayout DL)
      : ES(std::move(ES)),
        DL(std::move(DL)),
        TT(JTMB.getTargetTriple()),
        Mangle(*this->ES, this->DL),
        ObjLayer(makeObjectLayer(*this->ES)),
        CompileLayer(*this->ES, *ObjLayer,
                     std::make_unique<ConcurrentIRCompiler>(std::move(JTMB))),
        MainJD(this->ES->createBareJITDylib("<main>")) {
    MainJD.addGenerator(
        cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(
            DL.getGlobalPrefix())));
  }

  /** Ends the JIT execution session and releases its resources. */
  ~SunJIT() {
    if (auto Err = ES->endSession()) ES->reportError(std::move(Err));
  }

  /**
   * Resolve symbols out of a native static library (.a), the way the AOT
   * linker would. Used for archives carried inside .moon bundles.
   */
  Error addStaticLibrary(const std::string& Path) {
    auto G = StaticLibraryDefinitionGenerator::Load(*ObjLayer, Path.c_str());
    if (!G) return G.takeError();
    MainJD.addGenerator(std::move(*G));
    return Error::success();
  }

  /**
   * Create a host JIT with the requested backend optimization setting.
   */
  static Expected<std::unique_ptr<SunJIT>> Create(bool optimize = true) {
    auto EPC = SelfExecutorProcessControl::Create();
    if (!EPC) return EPC.takeError();

    auto ES = std::make_unique<ExecutionSession>(std::move(*EPC));

    JITTargetMachineBuilder JTMB(
        ES->getExecutorProcessControl().getTargetTriple());
    JTMB.setCodeGenOptLevel(optimize ? CodeGenOptLevel::Default
                                    : CodeGenOptLevel::None);
    // The same settings LLJIT picks for JITLink. Position-independent code
    // reaches host symbols through GOT and PLT entries; non-PIC code would
    // instead need libc and the compiler's own globals (`environ`, for one)
    // within 32-bit reach of the JITed code, which nothing guarantees. The
    // small code model is spelled out because LLVM's default for a JIT on
    // x86-64 is the large model, whose absolute-address sequences JITLink
    // does not need.
    JTMB.setRelocationModel(Reloc::PIC_);
    JTMB.setCodeModel(CodeModel::Small);

    auto DL = JTMB.getDefaultDataLayoutForTarget();
    if (!DL) return DL.takeError();

    return std::make_unique<SunJIT>(std::move(ES), std::move(JTMB),
                                    std::move(*DL));
  }

  /** Returns the data layout stored by this object. */
  const DataLayout& getDataLayout() const { return DL; }

  /** Returns the target triple stored by this object. */
  const Triple& getTargetTriple() const { return TT; }

  /** Returns the main jit dylib stored by this object. */
  JITDylib& getMainJITDylib() { return MainJD; }

  /** Adds an LLVM module to the JIT under the supplied resource tracker. */
  Error addModule(ThreadSafeModule TSM, ResourceTrackerSP RT = nullptr) {
    if (!RT) RT = MainJD.getDefaultResourceTracker();
    return CompileLayer.add(RT, std::move(TSM));
  }

  /** Finds the address of a symbol available to the JIT. */
  Expected<ExecutorSymbolDef> lookup(StringRef Name) {
    return ES->lookup({&MainJD}, Mangle(Name.str()));
  }

  /**
   * The symbol-table spelling of a C-level name on this platform (Mach-O
   * adds a leading underscore). Definitions handed to the JIT must use it,
   * or JIT'd code referencing the name will not find them.
   */
  SymbolStringPtr mangle(StringRef Name) { return Mangle(Name.str()); }

  /** Returns the execution session stored by this object. */
  ExecutionSession& getExecutionSession() { return *ES; }
};

}  // namespace sun::driver
