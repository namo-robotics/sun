/** Generates native exception storage and prepares libraries for JIT execution.
 */
#include "driver/exception_runtime.h"

#include <dlfcn.h>
#include <llvm/Support/Program.h>

#include <fstream>

#include "driver/compiler.h"
#include "runtime/exceptions.h"

/** Keeps the layout and native linking details out of ordinary driver
 * construction. */
namespace sun::driver {
/** Defines native storage at object emission, keeping TLS out of portable Moon
 * bitcode. */
bool defineExceptionRuntimeStorage(llvm::Module& module,
                                   std::string& errorMsg) {
  namespace layout = sun::abi::exceptions;
  unsigned depth = layout::defaultTraceDepth;
  if (auto* flag = module.getModuleFlag("sun.exceptions.trace_depth")) {
    auto* value = llvm::mdconst::dyn_extract<llvm::ConstantInt>(flag);
    if (!value || value->getZExtValue() > layout::maximumTraceDepth) {
      errorMsg = "Invalid exception trace depth metadata";
      return false;
    }
    depth = value->getZExtValue();
  }
  auto& context = module.getContext();
  if (!module.getNamedGlobal("sun_exception_trace_depth")) {
    new llvm::GlobalVariable(
        module, llvm::Type::getInt64Ty(context), true,
        llvm::GlobalValue::WeakODRLinkage,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), depth),
        "sun_exception_trace_depth");
  }
  if (!module.getNamedGlobal("sun_exception_state")) {
    auto* type = llvm::ArrayType::get(llvm::Type::getInt8Ty(context),
                                      layout::threadStateSizeForDepth(depth));
    auto* storage = new llvm::GlobalVariable(
        module, type, false, llvm::GlobalValue::WeakODRLinkage,
        llvm::ConstantAggregateZero::get(type), "sun_exception_state", nullptr,
        llvm::GlobalVariable::GeneralDynamicTLSModel);
    storage->setAlignment(llvm::Align(layout::alignment));
  }
  return true;
}

/** Keeps native file emission and linker invocation separate from runtime
 * ownership. */
namespace {
/** Emits strong definitions so the JIT library cannot share the host's TLS
 * block. */
void emitJITStorageObject(unsigned depth,
                          const std::filesystem::path& storage) {
  llvm::LLVMContext context;
  llvm::Module module("exception_storage", context);
  module.addModuleFlag(llvm::Module::Error, "sun.exceptions.trace_depth",
                       depth);
  std::string diagnostic;
  if (!defineExceptionRuntimeStorage(module, diagnostic)) {
    sun::support::logAndThrowError("Cannot define exception storage: " +
                                   diagnostic);
  }
  // Each loaded library owns its storage; weak definitions could coalesce with
  // the host.
  for (auto& global : module.globals()) {
    global.setLinkage(llvm::GlobalValue::ExternalLinkage);
  }
  if (!emitObjectFile(module, storage.string(), diagnostic)) {
    sun::support::logAndThrowError("Cannot emit exception storage: " +
                                   diagnostic);
  }
}

/** Extracts the already-compiled runtime; its machine code supports every
 * depth. */
void extractRuntimeArchive(const std::filesystem::path& archive) {
  std::ofstream archiveOutput(archive, std::ios::binary);
  archiveOutput << embeddedExceptionRuntime().str();
  archiveOutput.close();
  if (!archiveOutput) {
    sun::support::logAndThrowError("Cannot write exception runtime archive");
  }
}

/** Links the generated storage and existing runtime into one isolated native
 * library. */
void linkRuntimeLibrary(const std::filesystem::path& storage,
                        const std::filesystem::path& archive,
                        const std::filesystem::path& library) {
  std::string diagnostic;
  auto compiler = llvm::sys::findProgramByName("clang++");
  if (!compiler) {
    compiler = llvm::sys::findProgramByName("c++");
  }
  if (!compiler) {
    sun::support::logAndThrowError(
        "Nondefault JIT trace depth requires a native link driver");
  }
  std::vector<std::string> arguments{*compiler, "-pthread"};
#ifdef __APPLE__
  arguments.push_back("-dynamiclib");
  arguments.push_back("-Wl,-force_load," + archive.string());
#else
  arguments.push_back("-shared");
  arguments.push_back("-Wl,-Bsymbolic");
  arguments.insert(arguments.end(), {"-Wl,--whole-archive", archive.string(),
                                     "-Wl,--no-whole-archive"});
#endif
  arguments.insert(arguments.end(), {storage.string(), "-o", library.string()});
  std::vector<llvm::StringRef> views(arguments.begin(), arguments.end());
  if (llvm::sys::ExecuteAndWait(*compiler, views, std::nullopt, {}, 0, 0,
                                &diagnostic)) {
    sun::support::logAndThrowError("Cannot link exception runtime: " +
                                   diagnostic);
  }
}
}  // namespace

/** Links program-specific storage against the shared runtime archive before
 * execution. Each driver owns its library so different JIT layouts never share
 * TLS records.
 */
std::shared_ptr<void> loadExceptionRuntime(unsigned depth) {
  if (depth == sun_exception_trace_depth) {
    return {};
  }
  std::error_code directoryError;
  std::filesystem::create_directories("tmp", directoryError);
  if (directoryError) {
    sun::support::logAndThrowError(
        "Cannot create runtime temporary directory: " +
        directoryError.message());
  }
  llvm::SmallString<256> directory;
  auto error = llvm::sys::fs::createUniqueDirectory(
      (std::filesystem::current_path() / "tmp/sun-exceptions").string(),
      directory);
  if (error) {
    sun::support::logAndThrowError(
        "Cannot create exception runtime directory: " + error.message());
  }
  const std::filesystem::path root(directory.str().str());

  /** Removes temporary object and archive files unless the loaded library takes
   * ownership. */
  struct Cleanup {
    std::filesystem::path root;
    bool release = false;

    /** Releases files unless their ownership passed to the loaded library. */
    ~Cleanup() {
      if (!release) {
        std::error_code error;
        std::filesystem::remove_all(root, error);
      }
    }
  } cleanup{root};

  const auto storage = root / "storage.o";
  const auto library = root / "runtime.so";
  const auto archive = root / "runtime.a";
  emitJITStorageObject(depth, storage);
  extractRuntimeArchive(archive);
  linkRuntimeLibrary(storage, archive, library);
  auto* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    sun::support::logAndThrowError("Cannot load exception runtime: " +
                                   std::string(dlerror()));
  }
  cleanup.release = true;
  /** Unloads the runtime only after its generated code has been destroyed. */
  return std::shared_ptr<void>(handle, [root](void* library) {
    dlclose(library);
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });
}

}  // namespace sun::driver
