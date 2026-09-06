// moon_builder.cpp — see moon_builder.h

#include "moon_bundling/moon_builder.h"

#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

#include "driver/driver.h"
#include "driver/manifest_processor.h"
#include "generated/sun_version.h"
#include "moon_bundling/archive_symbols.h"
#include "moon_bundling/metadata_extractor.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/proto_importer.h"
#include "serialization/source_file_ids.h"
#include "support/error.h"

namespace sun {

namespace {

[[noreturn]] void fail(const std::string& message) {
  throw SunError(SunError::Kind::Compile, message);
}

// Fingerprints preserve all source bytes before computing the bundle identity.
std::string sourceFingerprint(const std::string& source) {
  return computeSha256Hex(source);
}

std::string readWholeFile(const std::string& path, const char* what) {
  std::ifstream in(path, std::ios::binary);
  if (!in) fail(std::string("Cannot read ") + what + ": " + path);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// A native archive named by the manifest's `archives:`, read once: its
// digest joins the bundle hash and the archive set hash, its symbols decide
// which externs bind to it, and its bytes are rewritten under the set hash
// when the bundle is written.
struct OwnArchive {
  std::string path;
  std::string name;  // file name carried in the bundle
  std::string data;
  std::string digest;
};

// The bundle's content hash, decided before anything is compiled so the
// compiler can spell the bundle's own symbols with it. It has to change
// whenever the code image would: it covers every source, every bundle the
// code links against (and how those are aliased), every archive it carries
// (two bundles alike in source but for the C library they wrap are
// different bundles, and importers drop a second bundle with a known
// hash), the target, the debug setting and the compiler itself. Importers
// rely on distinct bundles carrying distinct hashes, and a symbol prefix
// must not collide.
std::string computeBundleHash(std::vector<std::string> sources,
                              const std::vector<MoonImport>& moonImports,
                              const std::vector<OwnArchive>& archives,
                              const MoonBuildOptions& options) {
  std::string input;
  // Sorted, so the hash does not depend on manifest order
  std::sort(sources.begin(), sources.end());
  for (const auto& h : sources) input += "source:" + h + "\n";

  std::vector<std::string> archiveLines;
  for (const auto& archive : archives) {
    archiveLines.push_back("archive:" + archive.name + ":" + archive.digest +
                           "\n");
  }
  std::sort(archiveLines.begin(), archiveLines.end());
  for (const auto& line : archiveLines) input += line;

  std::vector<std::string> dependencies;
  for (const auto& import : moonImports) {
    auto reader = MoonReader::open(import.path);
    if (!reader) fail("Cannot open imported moon: " + import.path);
    const auto modules = reader->listModules();
    const auto* first =
        modules.empty() ? nullptr : reader->getMetadata(modules[0]);
    if (!first) fail("Imported moon has no modules: " + import.path);
    std::string dependency = "moon:" + first->content_hash() + "\n";
    // An alias changes which symbols this bundle's code refers to
    for (const auto& [from, to] : std::map<std::string, std::string>(
             import.moduleRemap.begin(), import.moduleRemap.end())) {
      dependency += "alias:" + from + "=" + to + "\n";
    }
    dependencies.push_back(std::move(dependency));
  }
  std::sort(dependencies.begin(), dependencies.end());
  for (const auto& dependency : dependencies) input += dependency;
  input += "format:" + std::to_string(MoonHeader::VERSION) + "\n";

  input += "target:" +
           (options.targetTriple.empty() ? llvm::sys::getDefaultTargetTriple()
                                         : options.targetTriple) +
           "\n";
  input += std::string("debug:") + (options.debugInfo ? "1" : "0") + "\n";
  input += std::string("optimize:") + (options.optimize ? "1" : "0") + "\n";
  input += std::string("compiler:") + SUN_VERSION + "-" + SUN_GIT_HASH + "\n";
  return computeContentHash(input);
}

}  // namespace

std::filesystem::path MoonBuilder::defaultOutputPath(
    const std::string& entrypoint) {
  // The name does not encode the target — the bundle metadata records it,
  // and cross bundles conventionally live in per-target directories.
  std::string out = entrypoint;
  size_t dotPos = out.rfind(".sun");
  if (dotPos != std::string::npos) out = out.substr(0, dotPos);
  return out + ".moon";
}

MoonBuildReport MoonBuilder::build(const std::string& entrypoint,
                                   const std::filesystem::path& outputPath,
                                   const MoonBuildOptions& options) {
  namespace fs = std::filesystem;
  fs::path entrypointPath = fs::absolute(entrypoint);
  std::string baseDir = entrypointPath.parent_path().string();

  // ---- Inputs: manifest (if any) + entrypoint itself ----
  MoonBuildReport report;
  report.moonImports = options.extraMoons;
  if (auto manifest = ManifestProcessor::fromEntrypointFile(
          entrypoint, options.targetTriple)) {
    report.sunFiles = std::move(manifest->sunFiles);
    report.moonImports.insert(report.moonImports.end(),
                              manifest->moonImports.begin(),
                              manifest->moonImports.end());
    report.protoFiles = std::move(manifest->protoFiles);
    report.archiveFiles = std::move(manifest->archiveFiles);
  }
  report.sunFiles.insert(report.sunFiles.begin(), entrypointPath.string());

  std::vector<std::string> fingerprints;
  for (const auto& path : report.sunFiles) {
    std::ifstream input(path, std::ios::binary);
    if (!input) fail("Cannot read source: " + path);
    fingerprints.push_back(sourceFingerprint(
        std::string(std::istreambuf_iterator<char>(input), {})));
  }
  for (const auto& source :
       ProtoImporter::importAll(report.protoFiles, baseDir))
    fingerprints.push_back(sourceFingerprint(source.sunSource));
  std::vector<moon::ModuleMetadata> allMetadata;

  std::vector<OwnArchive> ownArchives;
  for (const auto& archivePath : report.archiveFiles) {
    OwnArchive archive;
    archive.path = archivePath;
    archive.name = fs::path(archivePath).filename().string();
    archive.data = readWholeFile(archivePath, "native archive");
    archive.digest = computeSha256Hex(archive.data);
    ownArchives.push_back(std::move(archive));
  }
  std::vector<std::pair<std::string, std::string>> archiveIdentities;
  for (const auto& archive : ownArchives) {
    archiveIdentities.emplace_back(archive.name, archive.digest);
  }
  const std::string archiveSetHash = computeArchiveSetHash(archiveIdentities);

  // ---- Compile everything into one LLVM module, under the bundle's own
  // hash so its symbols are already the ones importers will look for ----
  const std::string bundleHash =
      computeBundleHash(fingerprints, report.moonImports, ownArchives, options);
  auto driver = Driver::createForAOT("moon_module", options.targetTriple,
                                     options.debugInfo, options.optimize);
  driver->setDumpProtoSun(options.dumpProtoSun);
  driver->setOwnBundleHash(bundleHash);

  // The C symbols the own archives define get a prefix made from the bytes
  // of all of them together, the way Sun symbols get the bundle's: another
  // bundle carrying another version of the library never collides with this
  // one, and one carrying the very same archives spells the symbols the same
  // way, so a program importing both links one copy. Within the set the
  // archives keep the single namespace a plain link would give them, so
  // their references to each other (libssl into libcrypto) stay consistent
  // and two of them defining one symbol is the same situation, and the same
  // warning-worthy one, as it would be for any C program linking both. The
  // program's externs naming these symbols are emitted under the prefixed
  // name; the archives are rewritten to match once compilation is done.
  std::map<std::string, std::string> renames;  // symbol -> prefixed name
  std::map<std::string, std::string> definedBy;  // symbol -> archive name
  std::map<std::string, std::string> ownUndefinedBy;  // symbol -> archive
  for (const auto& archive : ownArchives) {
    auto scan = scanArchiveSymbols(
        llvm::MemoryBufferRef(archive.data, archive.name));
    if (!scan) {
      fail("moon bundle: cannot isolate the symbols of native archive " +
           archive.path + ": " + llvm::toString(scan.takeError()));
    }
    for (const auto& symbol : scan->defined) {
      auto [it, fresh] = definedBy.emplace(symbol, archive.name);
      if (!fresh) {
        llvm::errs() << "Warning: native archives " << it->second << " and "
                     << archive.name << " both define '" << symbol
                     << "'; the link takes whichever it meets first, as it "
                        "would for any program linking both.\n";
        continue;
      }
      renames.emplace(symbol, "$" + archiveSetHash + "$_" + symbol);
    }
    for (const auto& symbol : scan->undefined) {
      ownUndefinedBy.emplace(symbol, archive.name);
    }
  }
  for (const auto& [symbol, renamed] : renames) ownUndefinedBy.erase(symbol);
  driver->setExternSymbolRenames(renames);
  driver->setMetadataCallback(
      [&](const BlockExprAST& program, SemanticAnalyzer& analyzer) {
        allMetadata = extractAnalyzedMetadata(program, analyzer, bundleHash);
        std::map<SourceFileId, SourceFileId> sourceFiles;
        for (auto& metadata : allMetadata) {
          serialization::remapSourceFiles(metadata, [&](SourceFileId id) {
            return sourceFiles.try_emplace(id, sourceFiles.size() + 1)
                .first->second;
          });
          const auto& name = metadata.module_name();
          if (!name.empty()) report.modules.push_back(name);
          if (!name.empty() && name.find('.') == std::string::npos &&
              metadata.visibility() != ast::PUBLIC)
            fail("moon bundle: top-level module '" + name +
                 "' must be declared 'public' to be exported");
        }
      });
  driver->compileFiles(report.sunFiles, report.moonImports, report.protoFiles);

  // ---- Write the bundle: each module's metadata + the shared code ----
  MoonWriter writer(bundleHash);
  for (const auto& metadata : allMetadata) {
    writer.addModule(driver->getModule(), metadata);
  }
  // Native archives travel inside the bundle, so importers link against them
  // without naming -l flags. The manifest's own `archives:` come first, with
  // their symbols renamed; then the archives of every imported bundle whose
  // code was just linked into this one (the driver put them on disk, as
  // `<set hash>/<name>`), carried unchanged since their symbols were renamed
  // when their bundle was built. That code keeps its calls into those
  // archives, and importers see only this bundle, so the archives must
  // follow the code the same way the bitcode already does. An archive is
  // identified by its set hash and name, which determine its renamed bytes:
  // the same library reached through several bundles is carried once, and
  // two versions of it are both carried.
  std::set<std::pair<std::string, std::string>> carried;
  std::map<std::string, std::vector<std::string>> hashesByName;
  auto carry = [&](const std::string& setHash, const std::string& name,
                   std::string data, bool inherited) {
    if (!carried.insert({setHash, name}).second) return;
    hashesByName[name].push_back(setHash);
    if (inherited) report.inheritedArchives.push_back(name);
    writer.addNativeArchive(setHash, name, std::move(data));
  };
  for (const auto& archive : ownArchives) {
    auto renamed = renameArchiveSymbols(
        llvm::MemoryBufferRef(archive.data, archive.name), renames);
    if (!renamed) {
      fail("moon bundle: cannot isolate the symbols of native archive " +
           archive.path + ": " + llvm::toString(renamed.takeError()));
    }
    carry(archiveSetHash, archive.name, std::move(*renamed), false);
  }
  std::map<std::string, std::string> inheritedDefinitions;  // bare -> carried
  for (const auto& archivePath : driver->getNativeArchivePaths()) {
    std::string data = readWholeFile(archivePath, "native archive");
    const fs::path path(archivePath);
    const auto definitions =
        listArchiveDefinitions(llvm::MemoryBufferRef(data, archivePath));
    inheritedDefinitions.insert(definitions.begin(), definitions.end());
    carry(path.parent_path().filename().string(), path.filename().string(),
          std::move(data), true);
  }

  // What would go wrong only at the final link, said now. An own archive
  // expecting a symbol that an imported bundle carries under a prefix cannot
  // reach that copy; the renamed name belongs to that archive alone.
  for (const auto& [symbol, archiveName] : ownUndefinedBy) {
    auto carried = inheritedDefinitions.find(symbol);
    if (carried == inheritedDefinitions.end() || carried->second == symbol) {
      continue;
    }
    llvm::errs() << "Warning: native archive " << archiveName
                 << " references '" << symbol
                 << "', which an imported bundle carries only as '"
                 << carried->second
                 << "'. The reference will not resolve against that copy; "
                    "carry the library it comes from under `archives:` or "
                    "link it into the program.\n";
  }
  for (const auto& [name, hashes] : hashesByName) {
    if (hashes.size() < 2) continue;
    llvm::errs() << "Warning: this bundle carries " << hashes.size()
                 << " versions of " << name
                 << ", each bound to the code that came with it:";
    for (const auto& hash : hashes) llvm::errs() << " " << hash;
    llvm::errs() << "\n";
  }
  if (!writer.write(outputPath)) {
    fail("Error writing moon: " + writer.getError());
  }
  return report;
}

}  // namespace sun
