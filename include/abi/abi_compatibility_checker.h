#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "abi/compatibility.h"

/** Provides immutable container data to ABI validation. */
namespace sun::moon_bundling {
/** Reads Moon metadata and payload bytes without authorizing their use. */
class MoonReader;
}  // namespace sun::moon_bundling

/** Owns compatibility policy independently of bundle container decoding. */
namespace sun::abi {

/** Owns a verified contract, remaining valid after the reader or checker is reset. */
class ValidatedBundle {
 public:
  /** Returns the complete contract verified for this import. */
  const BundleContract& contract() const { return *contract_; }

 private:
  friend class AbiCompatibilityChecker;
  /** Only the checker can authorize a contract after checking its payload. */
  explicit ValidatedBundle(std::shared_ptr<const BundleContract> contract)
      : contract_(std::move(contract)) {}
  std::shared_ptr<const BundleContract> contract_;
};

/** Validates imports and owns compilation-scoped caches, warnings, and requirements. */
class AbiCompatibilityChecker {
 public:
  /** Creates an isolated checker for the host or an explicitly selected target. */
  explicit AbiCompatibilityChecker(std::string targetTriple = "");
  /** Returns the current thread's checker shared by compiler import paths. */
  static AbiCompatibilityChecker& current();
  /** Starts a compilation, discarding cached authorizations and warning state. */
  void beginCompilation(const std::string& targetTriple);
  /** Checks the complete contract and all payloads against this compilation. */
  ValidatedBundle validate(moon_bundling::MoonReader& reader);
  /** Checks an explicitly selected target without changing the compilation target. */
  ValidatedBundle validate(moon_bundling::MoonReader& reader,
                           const std::string& targetTriple);
  /** Hashes the immutable bundle snapshot for build and validation cache keys. */
  std::string bytesHash(const moon_bundling::MoonReader& reader) const;
  /** Checks contract requirements only; this does not authorize a compiled payload. */
  void validateContract(const BundleContract& contract, const std::string& bundle,
                        const std::string& targetTriple) const;
  /** Returns requirements inherited from imports accepted during this compilation. */
  const std::map<std::string, BundleContract>& importedContracts() const {
    return importedContracts_;
  }

 private:
  /** Checks emitted boundaries against the independently supported contract. */
  void validatePayload(const BundleContract& contract, const llvm::Module& module,
                       const std::string& bundle, const std::string& targetTriple) const;
  /** Retains requirements and emits at most one producer warning per exact bundle. */
  void recordValidatedImport(const BundleContract& contract,
                             const std::string& bytesHash, const std::string& bundle);

  // Weak ownership prevents reader-address reuse from reusing a stale digest,
  // without keeping complete bundle contents alive until the next compilation.
  mutable std::map<std::weak_ptr<const std::string>, std::string,
                   std::owner_less<std::weak_ptr<const std::string>>> byteHashes_;
  std::string targetTriple_;
  std::set<std::string> warnedBundles_;
  std::map<std::string, BundleContract> importedContracts_;
  std::map<std::pair<std::string, std::string>,
           std::shared_ptr<const BundleContract>> validatedBundles_;
};
}  // namespace sun::abi
