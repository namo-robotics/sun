#pragma once

#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/BLAKE3.h>
#include <llvm/Support/SHA256.h>

/** Provides content digests for internal identities and external checksums. */
namespace sun::support {
/** Returns the full BLAKE3 digest of exact bytes as lowercase hexadecimal. */
inline std::string blake3Hex(llvm::StringRef bytes) {
  llvm::BLAKE3 hash;
  hash.update(bytes);
  return llvm::toHex(hash.final(), /*LowerCase=*/true);
}

/** Returns SHA-256 for checksums explicitly supplied in that algorithm. */
inline std::string sha256Hex(llvm::StringRef bytes) {
  llvm::SHA256 hash;
  hash.update(bytes);
  return llvm::toHex(hash.final(), /*LowerCase=*/true);
}
}  // namespace sun::support
