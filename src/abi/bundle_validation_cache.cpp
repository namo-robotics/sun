#include "abi/bundle_validation_cache.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>

#include <cstdlib>
#include <set>
#include <utility>

#include "abi/compatibility.h"
#include "driver/input_hash.h"
#include "support/content_hash.h"
#include "support/terminal.h"

/** Reuses successful checks of immutable Moon contents across compilations. */
namespace sun::abi {
/** Keeps policy and bounded memory receipts local to the executing thread. */
namespace {
thread_local std::filesystem::path cacheRoot;
thread_local bool forceValidation = false;
thread_local std::set<std::string> receipts;
thread_local size_t hits = 0, misses = 0;

/** Reports whether caching was explicitly disabled for this process. */
bool disabled() {
  const auto* value = std::getenv("SUN_DISABLE_VALIDATION_CACHE");
  return value && std::string(value) == "1";
}

/** Length-prefixes fields so paths and ordered options cannot alias tuples. */
void append(std::string& bytes, const std::string& field) {
  bytes += std::to_string(field.size()) + ":" + field;
}

/** Encodes the exact expected success record; any differing bytes are a miss.
 */
std::string receipt(const std::string& key) {
  return "sun.validation.blake3.v1\n" + key + "\nsuccess\n";
}

/** Retains a bounded set of successes without keeping bundle payloads alive. */
void retain(const std::string& key) {
  if (receipts.size() >= 4096) {
    receipts.clear();
  }
  receipts.insert(key);
}
}  // namespace

ValidationCacheScope::ValidationCacheScope(std::filesystem::path root,
                                           bool force)
    : previousRoot_(cacheRoot), previousForce_(forceValidation) {
  if (!root.empty()) {
    cacheRoot = std::filesystem::absolute(root);
  }
  forceValidation = forceValidation || force;
}

ValidationCacheScope::~ValidationCacheScope() {
  cacheRoot = std::move(previousRoot_);
  forceValidation = previousForce_;
}

std::filesystem::path BundleValidationCache::directory() {
  if (const auto* override = std::getenv("SUN_VALIDATION_CACHE");
      override && *override) {
    return std::filesystem::absolute(override);
  }
  return cacheRoot.empty()
             ? std::filesystem::current_path() / ".sun/cache/validation/v1"
             : cacheRoot;
}

std::string BundleValidationCache::key(const std::string& digest,
                                       const CompilationContext& context,
                                       const std::string& target) {
  std::string bytes = "sun.validation.blake3.v1";
  for (const auto& field :
       {digest, driver::getCompilerDigest(), compatibilityImplementation(),
        runtimeFingerprint(),
        target.empty() ? llvm::sys::getDefaultTargetTriple() : target}) {
    append(bytes, field);
  }
  append(bytes, std::to_string(static_cast<int>(context.execution)));
  append(bytes, context.debugInfo ? "debug" : "release");
  append(bytes, context.optimize ? "optimized" : "unoptimized");
  append(bytes, std::to_string(context.exceptionTraceDepth));
  for (const auto& [name, value] : context.linkOptions) {
    append(bytes, name);
    append(bytes, value);
  }
  return sun::support::blake3Hex(bytes);
}

bool BundleValidationCache::contains(const std::string& key) {
  bool found = false;
  if (!disabled() && !forceValidation) {
    std::error_code error;
    const auto root = directory();
    const auto memoryKey = root.string() + "\n" + key;
    found = receipts.count(memoryKey);
    if (!found) {
      // Bound reads before opening a receipt; malformed files are just misses.
      const auto path = root / (key + ".record");
      const auto expected = receipt(key);
      if (std::filesystem::file_size(path, error) == expected.size() &&
          !error) {
        auto file = llvm::MemoryBuffer::getFile(path.string());
        found = file && (*file)->getBuffer() == expected;
        if (found) {
          retain(memoryKey);
        }
      }
    }
  }
  found ? ++hits : ++misses;
  support::messageStream("debug")
      << "Validation cache " << (found ? "hit" : "miss") << " (hits=" << hits
      << ", full validations=" << misses << ")\n";
  return found;
}

void BundleValidationCache::remember(const std::string& key) {
  if (disabled()) {
    return;
  }
  const auto root = directory();
  retain(root.string() + "\n" + key);
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) {
    return;
  }
  llvm::SmallString<256> temporary;
  int fd = -1;
  if (llvm::sys::fs::createUniqueFile((root / ".receipt-%%%%%%").string(), fd,
                                      temporary)) {
    return;
  }
  {
    llvm::raw_fd_ostream output(fd, true);
    output << receipt(key);
    output.close();
    error = output.error();
    output.clear_error();
  }
  if (!error) {
    std::filesystem::rename(temporary.str().str(), root / (key + ".record"),
                            error);
  }
  std::filesystem::remove(temporary.str().str(), error);
}

void BundleValidationCache::clearMemory() {
  receipts.clear();
  hits = misses = 0;
}
}  // namespace sun::abi
