/** Registers and eagerly indexes metadata used exclusively by the in-process JIT. */
#include "driver/jit/unwind_registration.h"

#include <cstdint>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <set>

#include "llvm/DebugInfo/DWARF/DWARFDataExtractor.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugFrame.h"
#include "llvm/ExecutionEngine/JITLink/EHFrameSupport.h"
#include "llvm/ExecutionEngine/Orc/EHFrameRegistrationPlugin.h"
#include "llvm/Support/Errc.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/ExecutorProcessControl.h"

/** Registers native metadata for code executed in the compiler's process. */
namespace sun::driver::jit {
/** Keeps platform indexing and registration implementation details private. */
namespace {

/** Moves the unwinder's lazy frame indexing into JITLink registration.
 * This uses the platform extension _Unwind_Find_FDE rather than initiating an
 * unwind or running generated code. Normal deregistration releases the index.
 * A successful lookup does not prove indexing succeeded under memory pressure:
 * libgcc can silently fall back to a linear search if its allocation fails.
 */
class EagerEHFrameRegistrar final : public llvm::jitlink::EHFrameRegistrar {
  /** Receives the platform ABI's three address bases for a frame description. */
  struct FrameBases {
    std::uintptr_t text;
    std::uintptr_t data;
    std::uintptr_t function;
  };

  /** Looks up a registered frame without modifying an active exception object. */
  using FindFrame = const void* (*)(const void*, FrameBases*);

 public:
  /** Binds the host target and its process-wide frame lookup implementation. */
  explicit EagerEHFrameRegistrar(const llvm::Triple& target)
      : target_(target), findFrame_(reinterpret_cast<FindFrame>(
                             dlsym(RTLD_DEFAULT, "_Unwind_Find_FDE"))) {}

  /** Parses every frame, registers it, and indexes it before publishing the code.
   * Parse or lookup failure rejects materialization; lookup failure also undoes
   * registration so no stale unwind data survives failed materialization.
   */
  llvm::Error registerEHFrames(llvm::orc::ExecutorAddrRange range) override {
    std::lock_guard lock(mutex_);
    if (!findFrame_)
      return llvm::createStringError(
          llvm::errc::not_supported,
          "Eager JIT unwind indexing requires _Unwind_Find_FDE");

    // JITLink has applied relocations and finalized this in-process allocation.
    llvm::DWARFDebugFrame frames(target_.getArch(), true, range.Start.getValue());
    llvm::DWARFDataExtractor data(
        llvm::StringRef(range.Start.toPtr<const char*>(), range.size()),
        target_.isLittleEndian(), target_.isArch64Bit() ? 8 : 4);
    if (auto error = frames.parse(data)) return error;
    // Allocate bookkeeping before registering native pointers. LLVM may request
    // deregistration even after this method returned a materialization error.
    if (!registered_.insert(range.Start.getValue()).second)
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "Duplicate JIT unwind frame registration");
    if (auto error = registrar_.registerEHFrames(range)) {
      registered_.erase(range.Start.getValue());
      return error;
    }

    for (const auto& entry : frames.entries()) {
      const auto* frame = llvm::dyn_cast<llvm::dwarf::FDE>(&entry);
      if (!frame || !frame->getAddressRange()) continue;
      FrameBases bases{};
      const auto* found = findFrame_(
          reinterpret_cast<const void*>(frame->getInitialLocation()), &bases);
      const auto expected = range.Start.getValue() + frame->getOffset();
      if (reinterpret_cast<std::uintptr_t>(found) != expected) {
        registered_.erase(range.Start.getValue());
        return llvm::joinErrors(
            llvm::createStringError(
                llvm::errc::invalid_argument,
                "Eager JIT unwind indexing could not find its registered frame"),
            registrar_.deregisterEHFrames(range));
      }
    }
    return llvm::Error::success();
  }

  /** Releases frame registrations and their native indexes before code is unmapped. */
  llvm::Error deregisterEHFrames(llvm::orc::ExecutorAddrRange range) override {
    std::lock_guard lock(mutex_);
    if (!registered_.erase(range.Start.getValue())) return llvm::Error::success();
    return registrar_.deregisterEHFrames(range);
  }

 private:
  llvm::Triple target_;
  FindFrame findFrame_;
  std::mutex mutex_;
  std::set<std::uint64_t> registered_;
  llvm::jitlink::InProcessEHFrameRegistrar registrar_;
};

/** Keeps compiler-generated unwind indirection cells within their owning module.
 * Sharing a weak DW.ref cell lets one module's CIE point into another module's
 * storage. Removing the latter would invalidate the surviving unwind tables.
 */
class IsolatedEHFrameRegistrationPlugin final
    : public llvm::orc::EHFrameRegistrationPlugin {
 public:
  /** Retains LLVM's registration lifecycle with a selected native registrar. */
  IsolatedEHFrameRegistrationPlugin(
      llvm::orc::ExecutionSession& session,
      std::unique_ptr<llvm::jitlink::EHFrameRegistrar> registrar)
      : EHFrameRegistrationPlugin(session, std::move(registrar)) {}

  /** Prevents cross-module coalescing of ELF personality pointer cells. */
  void modifyPassConfig(llvm::orc::MaterializationResponsibility& responsibility,
                        llvm::jitlink::LinkGraph& graph,
                        llvm::jitlink::PassConfiguration& config) override {
    for (auto* symbol : graph.defined_symbols()) {
      if (symbol->hasName() && (*symbol->getName()).starts_with("DW.ref.")) {
        symbol->setScope(llvm::jitlink::Scope::Local);
        symbol->setLinkage(llvm::jitlink::Linkage::Strong);
      }
    }
    EHFrameRegistrationPlugin::modifyPassConfig(responsibility, graph, config);
  }
};
}  // namespace

void addUnwindRegistration(llvm::orc::ObjectLinkingLayer& layer,
                           llvm::orc::ExecutionSession& session) {
  const auto& target = session.getExecutorProcessControl().getTargetTriple();
  layer.addPlugin(std::make_unique<IsolatedEHFrameRegistrationPlugin>(
      session, std::make_unique<EagerEHFrameRegistrar>(target)));
}
}  // namespace sun::driver::jit
