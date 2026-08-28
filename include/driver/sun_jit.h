#pragma once

#include <memory>

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/JITLink/EHFrameSupport.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/EHFrameRegistrationPlugin.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/ExecutorProcessControl.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"

using namespace llvm;
using namespace llvm::orc;

/// The ORC JIT behind `sun file.sun`: one dylib, host-targeted, resolving
/// unknown symbols from the compiler's own process. The object-linking layer
/// is per-platform: JITLink for Mach-O (RuntimeDyld's Mach-O support is
/// legacy and mishandles arm64 unwind sections), RuntimeDyld elsewhere.
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

  /// Build the object layer that fits the host's object format. On Mach-O:
  /// JITLink, with eh-frame registration so thrown Sun errors unwind through
  /// JITed frames. (If exception interop proves incomplete on Apple Silicon,
  /// the known next step is MachOPlatform with the ORC runtime, which also
  /// registers compact-unwind info.) Everywhere else: RuntimeDyld, with the
  /// GDB registration listener so -g modules are debuggable under the JIT.
  static std::unique_ptr<ObjectLayer> makeObjectLayer(ExecutionSession& ES,
                                                      const Triple& TT) {
    if (TT.isOSBinFormatMachO()) {
      auto Layer = std::make_unique<ObjectLinkingLayer>(ES);
      Layer->addPlugin(std::make_unique<EHFrameRegistrationPlugin>(
          ES, std::make_unique<jitlink::InProcessEHFrameRegistrar>()));
      return Layer;
    }

    auto Layer = std::make_unique<RTDyldObjectLinkingLayer>(
        ES, []() { return std::make_unique<SectionMemoryManager>(); });
    // Register JITed objects with gdb's JIT interface so -g modules are
    // debuggable under the JIT (no-op when no debugger is attached).
    if (auto* gdbListener = JITEventListener::createGDBRegistrationListener()) {
      Layer->registerJITEventListener(*gdbListener);
    }
    if (TT.isOSBinFormatCOFF()) {
      Layer->setOverrideObjectFlagsWithResponsibilityFlags(true);
      Layer->setAutoClaimResponsibilityForObjectSymbols(true);
    }
    return Layer;
  }

 public:
  SunJIT(std::unique_ptr<ExecutionSession> ES, JITTargetMachineBuilder JTMB,
         DataLayout DL)
      : ES(std::move(ES)),
        DL(std::move(DL)),
        TT(JTMB.getTargetTriple()),
        Mangle(*this->ES, this->DL),
        ObjLayer(makeObjectLayer(*this->ES, TT)),
        CompileLayer(*this->ES, *ObjLayer,
                     std::make_unique<ConcurrentIRCompiler>(std::move(JTMB))),
        MainJD(this->ES->createBareJITDylib("<main>")) {
    MainJD.addGenerator(
        cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(
            DL.getGlobalPrefix())));
  }

  ~SunJIT() {
    if (auto Err = ES->endSession()) ES->reportError(std::move(Err));
  }

  /// Resolve symbols out of a native static library (.a), the way the AOT
  /// linker would. Used for archives carried inside .moon bundles.
  Error addStaticLibrary(const std::string& Path) {
    auto G = StaticLibraryDefinitionGenerator::Load(*ObjLayer, Path.c_str());
    if (!G) return G.takeError();
    MainJD.addGenerator(std::move(*G));
    return Error::success();
  }

  static Expected<std::unique_ptr<SunJIT>> Create() {
    auto EPC = SelfExecutorProcessControl::Create();
    if (!EPC) return EPC.takeError();

    auto ES = std::make_unique<ExecutionSession>(std::move(*EPC));

    JITTargetMachineBuilder JTMB(
        ES->getExecutorProcessControl().getTargetTriple());

    auto DL = JTMB.getDefaultDataLayoutForTarget();
    if (!DL) return DL.takeError();

    return std::make_unique<SunJIT>(std::move(ES), std::move(JTMB),
                                    std::move(*DL));
  }

  const DataLayout& getDataLayout() const { return DL; }

  const Triple& getTargetTriple() const { return TT; }

  JITDylib& getMainJITDylib() { return MainJD; }

  Error addModule(ThreadSafeModule TSM, ResourceTrackerSP RT = nullptr) {
    if (!RT) RT = MainJD.getDefaultResourceTracker();
    return CompileLayer.add(RT, std::move(TSM));
  }

  Expected<ExecutorSymbolDef> lookup(StringRef Name) {
    return ES->lookup({&MainJD}, Mangle(Name.str()));
  }

  ExecutionSession& getExecutionSession() { return *ES; }
};
