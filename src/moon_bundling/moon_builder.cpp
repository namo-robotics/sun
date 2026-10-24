// moon_builder.cpp — see moon_builder.h

#include "moon_bundling/moon_builder.h"

#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <map>
#include <set>

#include "abi/abi_compatibility_checker.h"
#include "abi/abi_validation_scope.h"
#include "driver/build_record.h"
#include "driver/c_bindings.h"
#include "driver/driver.h"
#include "driver/git_source.h"
#include "driver/input_hash.h"
#include "driver/manifest_processor.h"
#include "moon_bundling/archive_symbols.h"
#include "moon_bundling/library_cache.h"
#include "moon_bundling/metadata_extractor.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/proto_importer.h"
#include "serialization/source_file_ids.h"
#include "support/error.h"
#include "support/terminal.h"

using sun::support::SourceFileId;

/** Builds and loads compiled Moon libraries and their declaration metadata. */
namespace sun::moon_bundling {

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {
/** Provides filesystem operations for loading and packaging build inputs. */
namespace fs = std::filesystem;

/** Reports the operation's failure as a compiler error and stops processing. */
[[noreturn]] void fail(const std::string& message) {
  throw sun::support::SunError(sun::support::SunError::Kind::Compile, message);
}

/** Reads all bytes from an input file or reports a compiler error. */
std::string readWholeFile(const std::string& path, const char* what) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    fail(std::string("Cannot read ") + what + ": " + path);
  }
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

/**
 * A native archive named by the manifest's `archives:`, read once: its
 * digest joins the input hash and the archive set hash, its symbols decide
 * which externs bind to it, and its bytes are rewritten under the set hash
 * when the bundle is written. Borrows its description from the report; that
 * report's nativeArchives must remain unchanged while loaded archives exist.
 */
struct LoadedNativeArchive {
  const NativeArchive& description;
  std::string name;  // file name carried in the bundle
  std::string data;
  std::string digest;
};

}  // namespace

/** Prepared sources and hashes; archive descriptions stay owned by the report.
 */
struct MoonBuilder::LoadedInputs {
  MoonBuildReport report;
  std::optional<std::string> libraryId;
  std::vector<SynthesizedProtoModule> protoModules;
  std::vector<LoadedNativeArchive> loadedArchives;
  std::string archiveSetHash;
  std::string inputHash;

  /** Creates an empty result for input preparation. */
  LoadedInputs() = default;
  /** Transfers archive storage without invalidating borrowed descriptions. */
  LoadedInputs(LoadedInputs&&) = default;
  /** Prevents copies from retaining references into another result's report. */
  LoadedInputs(const LoadedInputs&) = delete;
};

/** Native symbol names and unresolved references computed from loaded archives.
 */
struct MoonBuilder::NativeSymbols {
  std::map<std::string, std::string> renames;
  std::map<std::string, std::string> undefinedBy;
};

/** Validated exports and their metadata, with bundle-local source identifiers.
 */
struct MoonBuilder::ExportMetadata {
  std::vector<moon::ModuleMetadata> records;
  std::vector<std::string> modules;
};

/** Compilation output; the driver is destroyed before metadata it references.
 */
struct MoonBuilder::CompiledBundle {
  ExportMetadata metadata;
  std::unique_ptr<sun::driver::Driver> driver;
};

MoonBuilder::LoadedInputs MoonBuilder::loadInputs(
    const std::string& entrypoint, const MoonBuildOptions& options) {
  LoadedInputs result;
  auto& report = result.report;
  fs::path entrypointPath = fs::absolute(entrypoint);
  std::string baseDir = entrypointPath.parent_path().string();

  report.moonImports = options.extraMoons;
  // Resolve command-line imports before hashing the bundles they name.
  for (auto& moon : report.moonImports) {
    moon.path = sun::driver::ManifestProcessor::resolvePath(
        moon.path, fs::current_path().string(), nullptr, options.targetTriple);
  }
  if (auto manifest = sun::driver::ManifestProcessor::fromEntrypointFile(
          entrypoint, options.targetTriple)) {
    result.libraryId = manifest->libraryId;
    report.sunFiles = std::move(manifest->sunFiles);
    report.moonImports.insert(report.moonImports.end(),
                              manifest->moonImports.begin(),
                              manifest->moonImports.end());
    report.protoFiles = std::move(manifest->protoFiles);
    report.nativeArchives = std::move(manifest->nativeArchives);
  }
  report.sunFiles.insert(report.sunFiles.begin(), entrypointPath.string());

  // Everything the bundle is built from, reduced to one hash before any of it
  // is compiled (see input_hash.h)
  sun::driver::BuildInputs inputs;
  inputs.artifactKind = "bundle";
  if (!options.versionName.empty()) {
    inputs.settings.emplace_back("version_name", options.versionName);
  }
  inputs.moonImports = report.moonImports;
  inputs.targetTriple = options.targetTriple;
  inputs.debugInfo = options.debugInfo;
  inputs.optimize = options.optimize;
  inputs.settings.emplace_back("format", std::to_string(MoonHeader::VERSION));
  sun::driver::generateCBindings(baseDir, options.targetTriple);
  result.protoModules = ProtoImporter::importAll(report.protoFiles, baseDir);
  sun::driver::addSourceDigests(inputs, report.sunFiles, result.protoModules);

  for (const auto& nativeArchive : report.nativeArchives) {
    LoadedNativeArchive archive{
        nativeArchive,
        fs::path(nativeArchive.path).filename().string(),
        readWholeFile(nativeArchive.path, "native archive"),
        {}};
    archive.digest = computeSha256Hex(archive.data);
    result.loadedArchives.push_back(std::move(archive));
  }
  std::vector<std::pair<std::string, std::string>> archiveIdentities;
  for (const auto& archive : result.loadedArchives) {
    archiveIdentities.emplace_back(archive.name, archive.digest);
  }
  result.archiveSetHash = computeArchiveSetHash(archiveIdentities);

  inputs.archives = archiveIdentities;
  result.inputHash = sun::driver::computeInputHash(inputs);
  return result;
}

bool MoonBuilder::isUpToDate(const std::string& inputHash,
                             const std::filesystem::path& outputPath,
                             const MoonBuildOptions& options) {
  // Debug artifacts and generated-source output require compilation even
  // when the bundle inputs are unchanged.
  return !options.forceRebuild && !options.dumpProtoSun && !options.debugMode &&
         sun::driver::readMoonInputHash(outputPath.string()) == inputHash;
}

MoonBuildReport MoonBuilder::readExistingReport(
    const std::filesystem::path& bundlePath,
    const std::string& ownArchiveSetHash, MoonBuildReport report) {
  auto reader = MoonReader::open(bundlePath);
  report.upToDate = true;
  if (!reader) {
    return report;
  }
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
  return report;
}

MoonBuilder::NativeSymbols MoonBuilder::prepareNativeSymbols(
    const LoadedInputs& inputs) {
  NativeSymbols result;
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
  std::map<std::string, std::string> definedBy;  // symbol -> archive name
  for (const auto& archive : inputs.loadedArchives) {
    auto scan =
        scanArchiveSymbols(llvm::MemoryBufferRef(archive.data, archive.name));
    if (!scan) {
      fail("moon bundle: cannot isolate the symbols of native archive " +
           archive.description.path + ": " + llvm::toString(scan.takeError()));
    }
    for (const auto& symbol : scan->defined) {
      auto [it, fresh] = definedBy.emplace(symbol, archive.name);
      if (!fresh) {
        sun::support::messageStream("warning")
            << "native archives " << it->second << " and " << archive.name
            << " both define '" << symbol
            << "'; the link takes whichever it meets first, as it "
               "would for any program linking both.\n";
        continue;
      }
      result.renames.emplace(symbol,
                             "$" + inputs.archiveSetHash + "$_" + symbol);
    }
    for (const auto& symbol : scan->undefined) {
      result.undefinedBy.emplace(symbol, archive.name);
    }
  }
  for (const auto& [symbol, renamed] : result.renames) {
    result.undefinedBy.erase(symbol);
  }
  return result;
}

MoonBuilder::ExportMetadata MoonBuilder::collectMetadata(
    const sun::ast::BlockExprAST& program,
    sun::semantic_analysis::SemanticAnalyzer& analyzer,
    const std::string& inputHash, uint32_t staticInitOrder) {
  ExportMetadata result;
  result.records = extractAnalyzedMetadata(program, analyzer, inputHash);
  std::map<SourceFileId, SourceFileId> sourceFiles;
  for (auto& metadata : result.records) {
    /** Assigns bundle-local identifiers to source files. */
    sun::serialization::remapSourceFiles(metadata, [&](SourceFileId id) {
      return sourceFiles.try_emplace(id, sourceFiles.size() + 1).first->second;
    });
    metadata.set_static_init_order(staticInitOrder);
    const auto& name = metadata.module_name();
    // A bundle exports through its modules, so a global outside any
    // module could never be reached by an importer. That includes a C
    // extern global, which declares a name like any other.
    if (name.empty()) {
      if (metadata.globals_size() > 0) {
        fail("moon bundle: global '" + metadata.globals(0).name() +
             "' is declared outside any module; a bundle's globals "
             "must be declared inside a module");
      }
      continue;
    }
    result.modules.push_back(name);
    if (name.find('.') == std::string::npos &&
        metadata.visibility() != sun::proto::ast::PUBLIC) {
      fail("moon bundle: top-level module '" + name +
           "' must be declared 'public' to be exported");
    }
  }
  return result;
}

MoonBuilder::CompiledBundle MoonBuilder::compile(
    const LoadedInputs& inputs, const NativeSymbols& symbols,
    const std::string& entrypoint, const MoonBuildOptions& options) {
  CompiledBundle result;
  auto& driver = result.driver;
  driver = sun::driver::Driver::createForAOT(
      "moon_module", options.targetTriple, options.debugInfo, options.optimize);
  if (options.debugMode) {
    driver->setDebugMode(true, entrypoint);
  }
  driver->setDumpProtoSun(options.dumpProtoSun);
  driver->setOwnBundleHash(inputs.inputHash);
  driver->setExternSymbolRenames(symbols.renames);
  /** Captures validated export metadata after semantic analysis. */
  driver->setMetadataCallback(
      [&](const sun::ast::BlockExprAST& program,
          sun::semantic_analysis::SemanticAnalyzer& analyzer) {
        result.metadata = collectMetadata(program, analyzer, inputs.inputHash,
                                          driver->getStaticInitOrder());
      });
  driver->compileFiles(inputs.report.sunFiles, inputs.report.moonImports, {},
                       {}, inputs.protoModules);
  // The returned driver must not retain references to this function's locals.
  driver->setMetadataCallback({});
  return result;
}

std::vector<std::string> MoonBuilder::packageNativeArchives(
    MoonWriter& writer, const LoadedInputs& inputs,
    const NativeSymbols& symbols,
    const std::vector<std::string>& inheritedPaths) {
  std::vector<std::string> inheritedArchives;
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
  /** Adds each archive identity once and records inherited inputs. */
  auto carry = [&](const std::string& setHash, const std::string& name,
                   std::string data, bool inherited) {
    if (!carried.insert({setHash, name}).second) {
      return;
    }
    hashesByName[name].push_back(setHash);
    if (inherited) {
      inheritedArchives.push_back(name);
    }
    writer.addNativeArchive(setHash, name, std::move(data));
  };
  for (const auto& archive : inputs.loadedArchives) {
    auto renamed = renameArchiveSymbols(
        llvm::MemoryBufferRef(archive.data, archive.name), symbols.renames);
    if (!renamed) {
      fail("moon bundle: cannot isolate the symbols of native archive " +
           archive.description.path + ": " +
           llvm::toString(renamed.takeError()));
    }
    carry(inputs.archiveSetHash, archive.name, std::move(*renamed), false);
  }
  std::map<std::string, std::string> inheritedDefinitions;  // bare -> carried
  for (const auto& archivePath : inheritedPaths) {
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
  for (const auto& [symbol, archiveName] : symbols.undefinedBy) {
    auto carried = inheritedDefinitions.find(symbol);
    if (carried == inheritedDefinitions.end() || carried->second == symbol) {
      continue;
    }
    sun::support::messageStream("warning")
        << "native archive " << archiveName << " references '" << symbol
        << "', which an imported bundle carries only as '" << carried->second
        << "'. The reference will not resolve against that copy; "
           "carry the library it comes from under `archives:` or "
           "link it into the program.\n";
  }
  for (const auto& [name, hashes] : hashesByName) {
    if (hashes.size() < 2) {
      continue;
    }
    sun::support::messageStream("warning")
        << "this bundle carries " << hashes.size() << " versions of " << name
        << ", each bound to the code that came with it:";
    for (const auto& hash : hashes) {
      llvm::errs() << " " << hash;
    }
    llvm::errs() << "\n";
  }
  return inheritedArchives;
}

void MoonBuilder::writeDebugJson(const std::filesystem::path& outputPath,
                                 const std::filesystem::path& debugFolder) {
  auto reader = MoonReader::open(outputPath);
  const auto jsonPath = debugFolder / "moon.json";
  if (!reader) {
    sun::support::logAndThrowError("Cannot reopen moon for debug JSON: " +
                                   outputPath.string());
  }
  if (!reader->writeDebugJson(jsonPath)) {
    sun::support::logAndThrowError(reader->getError());
  }
  sun::support::messageStream("info")
      << "  Generated: " << jsonPath.string() << "\n";
}

std::vector<std::string> MoonBuilder::writeBundle(
    const LoadedInputs& inputs, const NativeSymbols& symbols,
    CompiledBundle& compiled, const std::filesystem::path& outputPath,
    const MoonBuildOptions& options) {
  auto& driver = compiled.driver;
  MoonWriter writer(inputs.inputHash);
  writer.setVersionName(options.versionName);
  for (const auto& metadata : compiled.metadata.records) {
    writer.addModule(driver->getModule(), metadata);
  }
  if (compiled.metadata.records.empty()) {
    moon::ModuleMetadata metadata;
    metadata.set_source_hash(inputs.inputHash);
    writer.addModule(driver->getModule(), metadata);
  }
  if (inputs.libraryId) {
    writer.setLibraryId(*inputs.libraryId);
  }
  for (const auto& archive : inputs.report.nativeArchives) {
    writer.declareNativeRequirements(archive.cpuFeatures,
                                     archive.runtimeServices);
  }
  for (const auto& [identity, contract] :
       abi::AbiCompatibilityChecker::current().importedContracts()) {
    writer.inheritContract(contract);
  }
  auto inheritedArchives = packageNativeArchives(
      writer, inputs, symbols, driver->getNativeArchivePaths());
  if (!writer.write(outputPath)) {
    fail("Error writing moon: " + writer.getError());
  }
  if (options.debugMode) {
    writeDebugJson(outputPath, driver->getDebugFolder());
  }
  return inheritedArchives;
}

std::filesystem::path MoonBuilder::defaultOutputPath(
    const std::string& entrypoint) {
  // The name does not encode the target — the bundle metadata records it,
  // and cross bundles conventionally live in per-target directories.
  std::string out = entrypoint;
  size_t dotPos = out.rfind(".sun");
  if (dotPos != std::string::npos) {
    out = out.substr(0, dotPos);
  }
  return out + ".moon";
}

MoonBuildReport MoonBuilder::build(const std::string& entrypoint,
                                   const std::filesystem::path& outputPath,
                                   const MoonBuildOptions& options) {
  abi::AbiValidationScope compatibility({options.targetTriple,
                                         abi::NativeExecution::NativeLink,
                                         options.debugInfo, options.optimize});
  sun::driver::GitDependencyBuildScope dependencyScope(
      options.debugInfo, options.optimize, options.forceRebuild);
  sun::support::messageStream("info") << "Building library: " << entrypoint
                                      << " -> " << outputPath.string() << "\n";
  llvm::outs().flush();
  auto inputs = loadInputs(entrypoint, options);
  if (isUpToDate(inputs.inputHash, outputPath, options)) {
    return readExistingReport(outputPath, inputs.archiveSetHash,
                              std::move(inputs.report));
  }

  if (options.onBuildStart) {
    options.onBuildStart();
  }
  const auto symbols = prepareNativeSymbols(inputs);
  auto compiled = compile(inputs, symbols, entrypoint, options);
  auto inheritedArchives =
      writeBundle(inputs, symbols, compiled, outputPath, options);
  // Release the compiler before invalidating bundles or transferring its
  // report.
  compiled.driver.reset();
  LibraryCache::instance().invalidateBundle(outputPath);

  auto report = std::move(inputs.report);
  report.modules = std::move(compiled.metadata.modules);
  report.inheritedArchives = std::move(inheritedArchives);
  return report;
}

}  // namespace sun::moon_bundling
