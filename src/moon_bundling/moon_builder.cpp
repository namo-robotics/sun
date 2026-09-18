// moon_builder.cpp — see moon_builder.h

#include "moon_bundling/moon_builder.h"

#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <map>
#include <set>

#include "driver/build_record.h"
#include "driver/driver.h"
#include "driver/input_hash.h"
#include "driver/manifest_processor.h"
#include "moon_bundling/archive_symbols.h"
#include "moon_bundling/metadata_extractor.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/proto_importer.h"
#include "serialization/source_file_ids.h"
#include "support/error.h"

using sun::support::SourceFileId;

namespace sun::moon_bundling {

namespace {

[[noreturn]] void fail(const std::string& message) {
  throw sun::support::SunError(sun::support::SunError::Kind::Compile, message);
}

std::string readWholeFile(const std::string& path, const char* what) {
  std::ifstream in(path, std::ios::binary);
  if (!in) fail(std::string("Cannot read ") + what + ": " + path);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// A native archive named by the manifest's `archives:`, read once: its
// digest joins the input hash and the archive set hash, its symbols decide
// which externs bind to it, and its bytes are rewritten under the set hash
// when the bundle is written.
struct OwnArchive {
  std::string path;
  std::string name;  // file name carried in the bundle
  std::string data;
  std::string digest;
};

// Fill in what a build would have reported about the bundle already on disk:
// the modules it exports and the archives it took over from its imports.
// Archives under `ownArchiveSetHash` are the bundle's own.
void describeExistingBundle(const std::filesystem::path& bundlePath,
                            const std::string& ownArchiveSetHash,
                            MoonBuildReport& report) {
  auto reader = MoonReader::open(bundlePath);
  if (!reader) return;
  for (const auto& key : reader->listModules()) {
    const auto* metadata = reader->getMetadata(key);
    if (metadata && !metadata->module_name().empty()) {
      report.modules.push_back(metadata->module_name());
    }
  }
  for (const auto& archive : reader->getNativeArchives()) {
    if (archive.archiveSetHash != ownArchiveSetHash) {
      report.inheritedArchives.push_back(archive.name);
    }
  }
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
  // Resolve command-line imports before hashing the bundles they name.
  for (auto& moon : report.moonImports) {
    moon.path = sun::driver::ManifestProcessor::resolvePath(
        moon.path, fs::current_path().string(), nullptr, options.targetTriple);
  }
  if (auto manifest = sun::driver::ManifestProcessor::fromEntrypointFile(
          entrypoint, options.targetTriple)) {
    report.sunFiles = std::move(manifest->sunFiles);
    report.moonImports.insert(report.moonImports.end(),
                              manifest->moonImports.begin(),
                              manifest->moonImports.end());
    report.protoFiles = std::move(manifest->protoFiles);
    report.archiveFiles = std::move(manifest->archiveFiles);
  }
  report.sunFiles.insert(report.sunFiles.begin(), entrypointPath.string());

  // Everything the bundle is built from, reduced to one hash before any of it
  // is compiled (see input_hash.h)
  sun::driver::BuildInputs inputs;
  inputs.artifactKind = "bundle";
  inputs.moonImports = report.moonImports;
  inputs.targetTriple = options.targetTriple;
  inputs.debugInfo = options.debugInfo;
  inputs.optimize = options.optimize;
  inputs.settings.emplace_back("format", std::to_string(MoonHeader::VERSION));
  sun::driver::addSourceDigests(inputs, report.sunFiles, report.protoFiles,
                                baseDir);

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

  inputs.archives = archiveIdentities;
  const std::string inputHash = sun::driver::computeInputHash(inputs);

  // ---- When asked to, do nothing if the bundle on disk was built from
  // these very inputs. Printing the generated proto source is a reason to
  // run anyway ----
  if (options.skipIfUnchanged && !options.dumpProtoSun &&
      sun::driver::readMoonInputHash(outputPath.string()) == inputHash) {
    describeExistingBundle(outputPath, archiveSetHash, report);
    report.upToDate = true;
    return report;
  }
  if (options.onBuildStart) options.onBuildStart();

  // ---- Compile everything into one LLVM module, under the input hash so
  // its symbols are already the ones importers will look for ----
  std::vector<moon::ModuleMetadata> allMetadata;
  auto driver = sun::driver::Driver::createForAOT(
      "moon_module", options.targetTriple, options.debugInfo, options.optimize);
  driver->setDumpProtoSun(options.dumpProtoSun);
  driver->setOwnBundleHash(inputHash);

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
      [&](const sun::ast::BlockExprAST& program,
          sun::semantic_analysis::SemanticAnalyzer& analyzer) {
        allMetadata = extractAnalyzedMetadata(program, analyzer, inputHash);
        std::map<SourceFileId, SourceFileId> sourceFiles;
        for (auto& metadata : allMetadata) {
          sun::serialization::remapSourceFiles(metadata, [&](SourceFileId id) {
            return sourceFiles.try_emplace(id, sourceFiles.size() + 1)
                .first->second;
          });
          const auto& name = metadata.module_name();
          if (!name.empty()) report.modules.push_back(name);
          if (!name.empty() && name.find('.') == std::string::npos &&
              metadata.visibility() != sun::proto::ast::PUBLIC)
            fail("moon bundle: top-level module '" + name +
                 "' must be declared 'public' to be exported");
        }
      });
  driver->compileFiles(report.sunFiles, report.moonImports, report.protoFiles);

  // ---- Write the bundle: each module's metadata + the shared code ----
  MoonWriter writer(inputHash);
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

}  // namespace sun::moon_bundling
