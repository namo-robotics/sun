/** Collects native archives for linking and JIT execution. */
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "driver/driver.h"
#include "moon_bundling/archive_symbols.h"
#include "moon_bundling/library_cache.h"
#include "support/display_path.h"
#include "support/error.h"
#include "support/terminal.h"

using sun::moon_bundling::LibraryCache;
using sun::support::logAndThrowError;

/** Coordinates compilation, dependency loading, linking, and execution. */
namespace sun::driver {

/**
 * What the linker will not say. Two archives with one file name under
 * different hashes mean two versions of a library go into the program,
 * each bound to the code that came with it. A plain extern naming a symbol
 * that a bundle carries only in prefixed form binds to whatever the final
 * link provides under the bare name, which is rarely what was meant.
 */
static void warnAboutArchiveSet(const std::vector<std::string>& archives,
                                const std::vector<std::string>& bareExterns) {
  std::map<std::string, std::vector<std::string>> byName;
  for (const auto& path : archives) {
    byName[std::filesystem::path(path).filename().string()].push_back(path);
  }
  for (const auto& [name, paths] : byName) {
    if (paths.size() < 2) {
      continue;
    }
    sun::support::messageStream("warning")
        << paths.size() << " versions of " << name
        << " will be linked, each bound to the code that came with "
           "it:\n";
    for (const auto& path : paths) {
      sun::support::messageStream("warning")
          << "  " << sun::support::displayPath(path) << "\n";
    }
  }

  if (bareExterns.empty()) {
    return;
  }
  std::set<std::string> wanted(bareExterns.begin(), bareExterns.end());
  std::set<std::string> reported;
  for (const auto& path : archives) {
    auto buffer = llvm::MemoryBuffer::getFile(path);
    if (!buffer) {
      continue;
    }
    for (const auto& [bare, recorded] :
         sun::moon_bundling::listArchiveDefinitions(
             (*buffer)->getMemBufferRef())) {
      if (bare == recorded || !wanted.count(bare)) {
        continue;
      }
      if (!reported.insert(bare).second) {
        continue;
      }
      sun::support::messageStream("warning")
          << "extern \"C\" '" << bare << "' names a symbol that "
          << std::filesystem::path(path).filename().string() << " carries as '"
          << recorded
          << "'. The declaration binds to whatever the final link "
             "provides as '"
          << bare
          << "', not to that bundle's copy. Call through the "
             "bundle's API, or carry the archive under `archives:` "
             "to bind your own copy.\n";
    }
  }
}

void Driver::collectNativeArchives(const std::set<std::string>& linkedModules) {
  nativeArchivePaths_ = manifestArchivePaths_;
  if (linkedModules.empty()) {
    return;
  }

  std::error_code ec;
  if (archiveTempDir_.empty()) {
    archiveTempDir_ = std::filesystem::temp_directory_path(ec) /
                      ("sun-archives-" + std::to_string(::getpid()));
    if (ec) {
      archiveTempDir_.clear();
      return;
    }
    std::filesystem::create_directories(archiveTempDir_, ec);
    if (ec) {
      archiveTempDir_.clear();
      return;
    }
  }

  const auto bundled = LibraryCache::instance().extractNativeArchives(
      linkedModules, archiveTempDir_);
  nativeArchivePaths_.insert(nativeArchivePaths_.end(), bundled.begin(),
                             bundled.end());
  warnAboutArchiveSet(nativeArchivePaths_, unmappedExternLinkNames_);
}

void Driver::registerArchivesWithJIT() {
  if (!ctx->jit) {
    return;
  }

  // The same archives the AOT link would use, so a program behaves alike
  // run directly and compiled. Their symbols carry the prefix of the bundle
  // that brought them, so a shared library of the same code could not stand
  // in even if one were installed. What an archive member needs from outside
  // resolves from the running process; the glibc entry points dlsym cannot
  // see are provided by createForJIT.
  for (const auto& archive : nativeArchivePaths_) {
    if (auto err = ctx->jit->addStaticLibrary(archive)) {
      sun::support::messageStream("warning")
          << "could not load bundled library '" << archive
          << "' into the JIT: " << llvm::toString(std::move(err))
          << "\n  Compiling with -c links it with the system linker "
             "instead.\n";
    }
  }
}

void Driver::addJITStaticLibrary(const std::string& path) {
  if (!ctx->jit) {
    return;
  }
  if (auto err = ctx->jit->addStaticLibrary(path)) {
    logAndThrowError("cannot load static archive '" + path +
                     "': " + llvm::toString(std::move(err)));
  }
}

}  // namespace sun::driver
