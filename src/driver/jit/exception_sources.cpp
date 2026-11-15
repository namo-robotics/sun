/** Packs source locations into relocatable program data before code is
 * published. */
#include "driver/exception_sources.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Object/SymbolSize.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "runtime/exception_sources.h"

/** Builds indexes using compiler memory, outside allocation-free execution. */
namespace sun::driver {
/** Keeps serialized layout and LLVM object handling local to index
 * construction. */
namespace {
constexpr llvm::StringLiteral mapName = "__sun_exception_source_map";

/** Indexes physical functions, including adapters that may contain inlined
 * source calls. */
std::vector<llvm::Function*> sourceFunctions(llvm::Module& module) {
  std::vector<llvm::Function*> functions;
  for (auto& function : module) {
    const auto* debug = function.getSubprogram();
    if (function.isDeclaration() || !debug) {
      continue;
    }
    functions.push_back(&function);
  }
  return functions;
}

/** Appends aligned records and deduplicated strings within the reserved index.
 */
class IndexWriter {
 public:
  char* bytes;
  std::size_t capacity;
  std::size_t used;
  bool failed = false;
  std::map<std::string, std::uint32_t> strings;

  /** Begins after the relocated function records, which must remain intact. */
  IndexWriter(char* data, std::size_t size, std::size_t start)
      : bytes(data), capacity(size), used(start) {}

  /** Reserves aligned bytes, rejecting an index that would exceed its object
   * storage. */
  std::uint32_t reserve(std::size_t count) {
    used = (used + 7) & ~std::size_t(7);
    if (used > capacity || count > capacity - used) {
      failed = true;
      return 0;
    }
    const auto offset = used;
    used += count;
    return static_cast<std::uint32_t>(offset);
  }

  /** Stores a string once; index offsets are independent of load addresses. */
  std::uint32_t string(const std::string& value) {
    if (auto found = strings.find(value); found != strings.end()) {
      return found->second;
    }
    const auto offset = reserve(value.size() + 1);
    if (!offset) {
      return 0;
    }
    std::memcpy(bytes + offset, value.c_str(), value.size() + 1);
    strings.emplace(value, offset);
    return offset;
  }
};

/** Normalizes recorded source paths without resolving relative library paths;
 * rejects synthetic filenames. */
std::string sourcePath(llvm::StringRef path) {
  const std::filesystem::path source(path.str());
  if (path.empty() || source.filename().string().starts_with("<")) {
    return {};
  }
  return source.lexically_normal().generic_string();
}

/** Maps each source throw location to its readable label before indexing rows.
 */
std::map<std::pair<std::string, std::uint64_t>, std::string> throwLabels(
    const llvm::Module& module) {
  std::map<std::pair<std::string, std::uint64_t>, std::string> labels;
  if (const auto* sites = module.getNamedMetadata("sun.trace.throws")) {
    for (const auto* site : sites->operands()) {
      const auto path = sourcePath(
          llvm::cast<llvm::MDString>(site->getOperand(0))->getString());
      const auto line =
          llvm::mdconst::extract<llvm::ConstantInt>(site->getOperand(1))
              ->getZExtValue();
      labels.try_emplace(
          std::make_pair(path, line),
          llvm::cast<llvm::MDString>(site->getOperand(2))->getString().str());
    }
  }
  return labels;
}

}  // namespace

void prepareExceptionSources(llvm::Module& module, bool jit) {
  if (module.getNamedGlobal(mapName)) {
    return;
  }
  const auto functions = sourceFunctions(module);
  if (functions.empty()) {
    return;
  }
  // Capture occurs at calls, so synchronous tables also cover non-throwing
  // functions. Asynchronous tables can break cleanup resumption on Darwin at
  // O0.
  for (auto& function : module) {
    if (!function.isDeclaration()) {
      function.setUWTableKind(llvm::UWTableKind::Sync);
    }
  }
  auto& context = module.getContext();
  auto* ptr = llvm::PointerType::getUnqual(context);
  auto* i32 = llvm::Type::getInt32Ty(context);
  auto* i64 = llvm::Type::getInt64Ty(context);
  auto* record = llvm::StructType::get(ptr, i64, i32, i32);
  std::vector<llvm::Constant*> records;
  std::size_t instructionCount = 0;
  for (auto* function : functions) {
    instructionCount += function->getInstructionCount();
    records.push_back(llvm::ConstantStruct::get(
        record,
        {function, llvm::ConstantInt::get(i64, 0),
         llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, 0)}));
  }
  // Reserve enough rows for instruction locations and inlined call chains.
  // Packing checks this bound rather than silently discarding source
  // information.
  const auto payloadSize =
      4096 + instructionCount * 256 + functions.size() * 1024;
  auto* array = llvm::ArrayType::get(record, records.size());
  auto* payload =
      llvm::ArrayType::get(llvm::Type::getInt8Ty(context), payloadSize);
  auto* type = llvm::StructType::get(ptr, i32, i32, array, payload);
  const auto total = sizeof(runtime::ExceptionSourceMap) +
                     records.size() * sizeof(runtime::ExceptionSourceFunction) +
                     payloadSize;
  auto* map = new llvm::GlobalVariable(
      module, type, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantStruct::get(type,
                                {llvm::ConstantPointerNull::get(ptr),
                                 llvm::ConstantInt::get(i32, records.size()),
                                 llvm::ConstantInt::get(i32, total),
                                 llvm::ConstantArray::get(array, records),
                                 llvm::ConstantAggregateZero::get(payload)}),
      mapName);
  map->setAlignment(llvm::Align(8));
  llvm::appendToCompilerUsed(module, {map});
  if (!jit) {
    auto* init = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), false),
        llvm::GlobalValue::InternalLinkage, "__sun_register_exception_sources",
        module);
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", init));
    builder.CreateCall(module.getOrInsertFunction("sun_eh_register_source_map",
                                                  builder.getVoidTy(), ptr),
                       {map});
    builder.CreateRetVoid();
    llvm::appendToGlobalCtors(module, init, 0);
  }
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> indexExceptionSources(
    llvm::Module& module, llvm::MemoryBufferRef input) {
  auto output =
      llvm::WritableMemoryBuffer::getNewUninitMemBuffer(input.getBufferSize());
  std::memcpy(output->getBufferStart(), input.getBufferStart(),
              input.getBufferSize());
  if (!module.getNamedGlobal(mapName)) {
    return std::unique_ptr<llvm::MemoryBuffer>(std::move(output));
  }
  auto object =
      llvm::object::ObjectFile::createObjectFile(output->getMemBufferRef());
  if (!object) {
    return object.takeError();
  }
  auto& file = **object;
  if (!file.isLittleEndian() || file.getBytesInAddress() != 8) {
    return llvm::createStringError(
        "Exception source indexes require a little-endian 64-bit target");
  }
  auto dwarf = llvm::DWARFContext::create(file);
  const auto functions = sourceFunctions(module);

  /** Locates a symbol in both target address space and the relocatable object
   * buffer. */
  struct SymbolLocation {
    std::uint64_t address, size, section;
    std::size_t offset;
  };

  std::map<std::string, SymbolLocation> symbols;
  for (auto [symbol, size] : llvm::object::computeSymbolSizes(file)) {
    auto name = symbol.getName();
    auto address = symbol.getAddress();
    auto section = symbol.getSection();
    if (!name || !address || !section) {
      if (!name) {
        llvm::consumeError(name.takeError());
      }
      if (!address) {
        llvm::consumeError(address.takeError());
      }
      if (!section) {
        llvm::consumeError(section.takeError());
      }
      continue;
    }
    if (*section == file.section_end()) {
      continue;
    }
    auto content = (*section)->getContents();
    if (!content) {
      llvm::consumeError(content.takeError());
      continue;
    }
    if (content->empty()) {
      continue;
    }
    auto normalized = *name;
    if (file.isMachO()) {
      normalized.consume_front("_");
    }
    const auto within = *address - (*section)->getAddress();
    symbols[normalized.str()] = {
        *address, size, (*section)->getIndex(),
        std::size_t(content->data() - output->getBufferStart()) + within};
  }
  auto found = symbols.find(mapName.str());
  if (found == symbols.end()) {
    return llvm::createStringError("Missing reserved exception source index");
  }
  auto* map = reinterpret_cast<runtime::ExceptionSourceMap*>(
      output->getBufferStart() + found->second.offset);
  IndexWriter writer(
      reinterpret_cast<char*>(map), map->byteSize,
      sizeof(*map) +
          functions.size() * sizeof(runtime::ExceptionSourceFunction));
  const auto empty = writer.string("");
  const auto labels = throwLabels(module);
  auto* records = reinterpret_cast<runtime::ExceptionSourceFunction*>(map + 1);
  llvm::DILineInfoSpecifier spec(
      llvm::DILineInfoSpecifier::FileLineInfoKind::AbsoluteFilePath,
      llvm::DILineInfoSpecifier::FunctionNameKind::ShortName);
  for (std::size_t index = 0; index < functions.size(); ++index) {
    auto position = symbols.find(functions[index]->getName().str());
    if (position == symbols.end()) {
      continue;
    }
    const auto& symbol = position->second;
    auto lines = dwarf->getLineInfoForAddressRange(
        {symbol.address, symbol.section}, symbol.size, spec);
    std::vector<runtime::ExceptionSourceRow> rows;
    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
      const auto address = lines[lineIndex].first;
      const auto end = lineIndex + 1 < lines.size()
                           ? lines[lineIndex + 1].first
                           : symbol.address + symbol.size;
      if (end <= address || address < symbol.address) {
        continue;
      }
      auto inlining =
          dwarf->getInliningInfoForAddress({address, symbol.section}, spec);
      std::vector<runtime::ExceptionSourceFrame> frames;
      const auto count = std::max(1u, inlining.getNumberOfFrames());
      for (unsigned depth = count; depth; --depth) {
        const auto& info = inlining.getNumberOfFrames()
                               ? inlining.getFrame(depth - 1)
                               : lines[lineIndex].second;
        auto name = info.FunctionName;
        if (name.empty() || name == llvm::DILineInfo::BadString) {
          name = functions[index]->getSubprogram()->getName().str();
        }
        if (llvm::StringRef(name).starts_with("sun_main_exception")) {
          continue;
        }
        if (name.size() > runtime::reportNameLimit + 1) {
          name.resize(runtime::reportNameLimit + 1);
        }
        const auto path = sourcePath(info.FileName);
        auto throwLabel = empty;
        if (depth == 1) {
          if (const auto label = labels.find({path, info.Line});
              label != labels.end()) {
            throwLabel = writer.string(label->second);
          }
        }
        frames.push_back({writer.string(name), writer.string(path), info.Line,
                          info.Column, throwLabel});
      }
      const auto frameOffset =
          writer.reserve(frames.size() * sizeof(frames.front()));
      if (!frameOffset) {
        return llvm::createStringError(
            "Exception source index exceeded reserved storage");
      }
      if (!frames.empty()) {
        std::memcpy(writer.bytes + frameOffset, frames.data(),
                    frames.size() * sizeof(frames.front()));
      }
      rows.push_back({std::uint32_t(address - symbol.address),
                      std::uint32_t(end - symbol.address), frameOffset,
                      std::uint32_t(frames.size())});
    }
    const auto rowOffset =
        writer.reserve(rows.size() * sizeof(runtime::ExceptionSourceRow));
    if (!rowOffset) {
      return llvm::createStringError(
          "Exception source index exceeded reserved storage");
    }
    std::memcpy(writer.bytes + rowOffset, rows.data(),
                rows.size() * sizeof(runtime::ExceptionSourceRow));
    records[index].size = symbol.size;
    records[index].rows = rowOffset;
    records[index].rowCount = rows.size();
  }
  if (writer.failed) {
    return llvm::createStringError(
        "Exception source strings exceeded reserved storage");
  }
  return std::unique_ptr<llvm::MemoryBuffer>(std::move(output));
}

/** Tracks source indexes alongside JIT resources, including unload and
 * transfer. */
class SourceRegistration final : public llvm::orc::ObjectLinkingLayer::Plugin {
  std::shared_ptr<ExceptionSourceCallbacks> callbacks_;
  std::mutex mutex_;
  std::map<llvm::orc::ResourceKey, std::vector<void*>> maps_;

 public:
  /** Retains callbacks for the particular JIT runtime instance. */
  explicit SourceRegistration(
      std::shared_ptr<ExceptionSourceCallbacks> callbacks)
      : callbacks_(std::move(callbacks)) {}

  /** Waits for relocations before registering function addresses and source
   * data. */
  void modifyPassConfig(
      llvm::orc::MaterializationResponsibility& responsibility,
      llvm::jitlink::LinkGraph& graph,
      llvm::jitlink::PassConfiguration& config) override {
    llvm::orc::ResourceKey key;
    llvm::cantFail(responsibility.withResourceKeyDo(
        [&](auto resource) { key = resource; }));
    config.PrePrunePasses.push_back([](llvm::jitlink::LinkGraph& linked) {
      for (auto* symbol : linked.defined_symbols()) {
        if (symbol->hasName() && (*symbol->getName()).contains(mapName)) {
          symbol->setLive(true);
        }
      }
      return llvm::Error::success();
    });
    config.PostFixupPasses.push_back(
        [this, key](llvm::jitlink::LinkGraph& linked) {
          for (auto* symbol : linked.defined_symbols()) {
            if (!symbol->hasName() || !(*symbol->getName()).contains(mapName)) {
              continue;
            }
            auto* map = symbol->getAddress().toPtr<void*>();
            if (callbacks_->add) {
              std::lock_guard lock(mutex_);
              maps_[key].push_back(map);
            }
          }
          return llvm::Error::success();
        });
  }

  /** Publishes indexes only after target memory has been finalized. */
  llvm::Error notifyEmitted(
      llvm::orc::MaterializationResponsibility& responsibility) override {
    llvm::orc::ResourceKey key;
    llvm::cantFail(responsibility.withResourceKeyDo(
        [&](auto resource) { key = resource; }));
    std::lock_guard lock(mutex_);
    for (auto* map : maps_[key]) {
      callbacks_->add(map);
    }
    return llvm::Error::success();
  }

  /** Removes any indexes whose materialization failed. */
  llvm::Error notifyFailed(
      llvm::orc::MaterializationResponsibility& responsibility) override {
    llvm::orc::ResourceKey key;
    llvm::cantFail(responsibility.withResourceKeyDo(
        [&](auto resource) { key = resource; }));
    return notifyRemovingResources(responsibility.getTargetJITDylib(), key);
  }

  /** Unregisters indexes before their backing JIT allocations disappear. */
  llvm::Error notifyRemovingResources(llvm::orc::JITDylib&,
                                      llvm::orc::ResourceKey key) override {
    std::lock_guard lock(mutex_);
    for (auto* map : maps_[key]) {
      callbacks_->remove(map);
    }
    maps_.erase(key);
    return llvm::Error::success();
  }

  /** Preserves ownership when LLVM transfers code between resource trackers. */
  void notifyTransferringResources(llvm::orc::JITDylib&,
                                   llvm::orc::ResourceKey destination,
                                   llvm::orc::ResourceKey source) override {
    std::lock_guard lock(mutex_);
    auto& target = maps_[destination];
    auto& previous = maps_[source];
    target.insert(target.end(), previous.begin(), previous.end());
    maps_.erase(source);
  }
};

void addExceptionSourceRegistration(
    llvm::orc::ObjectLinkingLayer& layer,
    std::shared_ptr<ExceptionSourceCallbacks> callbacks) {
  layer.addPlugin(std::make_shared<SourceRegistration>(std::move(callbacks)));
}
}  // namespace sun::driver
