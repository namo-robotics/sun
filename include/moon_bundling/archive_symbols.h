// archive_symbols.h — Symbol isolation for native archives carried in bundles
//
// A bundle's Sun symbols are spelled `$hash$_name`, so two bundles never
// collide. The C archives a bundle carries (`archives:` in its manifest) get
// the same treatment: every symbol they define is renamed to
// `$sethash$_symbol` when the bundle is built, where the hash is taken from
// the bytes of all the archives listed together, and the bundle's
// `extern "C"` declarations bind to those names. Two bundles carrying
// different versions of one library then each link against their own copy,
// while two bundles carrying the same set of bytes produce the same names
// and share one copy. Hashing the set rather than each archive keeps the
// references between siblings (libssl into libcrypto) inside the name: the
// same libssl next to a different libcrypto is a different set.

#pragma once

#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBufferRef.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sun {

/// The global symbols of an archive, by bare name (a Mach-O leading `_`
/// stripped), gathered from every member.
struct ArchiveSymbolScan {
  std::set<std::string> defined;    // provided by some member
  std::set<std::string> undefined;  // expected from outside the archive
};

/// Read every member of `archive` and classify its global symbols. Fails
/// when a member is not a native object (an LLVM bitcode member from an LTO
/// build cannot be renamed) or when members mix object formats.
llvm::Expected<ArchiveSymbolScan> scanArchiveSymbols(
    llvm::MemoryBufferRef archive);

/// Rewrite `archive` so that every symbol named in `renames` (bare old name
/// to bare new name) is renamed in every member, definitions and references
/// alike, and return the new archive bytes with a fresh symbol index.
llvm::Expected<std::string> renameArchiveSymbols(
    llvm::MemoryBufferRef archive,
    const std::map<std::string, std::string>& renames);

/// The names in an archive's symbol index as C code spells them (Mach-O's
/// leading underscore removed). Cheap: the index and one member header.
std::vector<std::string> listArchiveIndex(llvm::MemoryBufferRef archive);

/// The symbols an archive's index says it defines, keyed by the name with
/// any `$hash$_` prefix removed, mapped to the name as recorded. Used to
/// tell when a plain extern names something a bundle carries only in
/// prefixed form.
std::map<std::string, std::string> listArchiveDefinitions(
    llvm::MemoryBufferRef archive);

/// The hash a bundle's own archives have their symbols renamed under: over
/// every archive's file name and the SHA-256 of its bytes as shipped by the
/// vendor, independent of manifest order. Longer than a bundle hash because
/// a collision here would merge two libraries' symbols.
std::string computeArchiveSetHash(
    const std::vector<std::pair<std::string, std::string>>& namesAndDigests);

}  // namespace sun
