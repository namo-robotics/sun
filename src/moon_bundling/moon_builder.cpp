// moon_builder.cpp — see moon_builder.h

#include "moon_bundling/moon_builder.h"

#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <fstream>
#include <map>

#include "driver/driver.h"
#include "driver/manifest_processor.h"
#include "generated/sun_version.h"
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

// The bundle's content hash, decided before anything is compiled so the
// compiler can spell the bundle's own symbols with it. It has to change
// whenever the code image would: it covers every source, every bundle the
// code links against (and how those are aliased), the target, the debug
// setting and the compiler itself. Importers rely on distinct bundles
// carrying distinct hashes, and a symbol prefix must not collide.
std::string computeBundleHash(std::vector<std::string> sources,
                              const std::vector<MoonImport>& moonImports,
                              const MoonBuildOptions& options) {
  std::string input;
  // Sorted, so the hash does not depend on manifest order
  std::sort(sources.begin(), sources.end());
  for (const auto& h : sources) input += "source:" + h + "\n";

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

  // ---- Compile everything into one LLVM module, under the bundle's own
  // hash so its symbols are already the ones importers will look for ----
  const std::string bundleHash =
      computeBundleHash(fingerprints, report.moonImports, options);
  auto driver = Driver::createForAOT("moon_module", options.targetTriple,
                                     options.debugInfo, options.optimize);
  driver->setDumpProtoSun(options.dumpProtoSun);
  driver->setOwnBundleHash(bundleHash);
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
  // without naming -l flags. The manifest's own `archives:` come first; then
  // the archives of every imported bundle whose code was just linked into
  // this one (the driver put them on disk). That code keeps its calls into
  // those archives, and importers see only this bundle, so the archives must
  // follow the code the same way the bitcode already does. Identical archives
  // reached through several bundles are carried once; two different archives
  // under one file name cannot both be carried, since a consumer extracts
  // them by name.
  std::map<std::string, std::string> carriedDigestByName;
  auto carry = [&](const std::string& archivePath, bool inherited) {
    std::string data = readWholeFile(archivePath, "native archive");
    const std::string name = fs::path(archivePath).filename().string();
    const std::string digest = computeSha256Hex(data);
    auto [it, fresh] = carriedDigestByName.try_emplace(name, digest);
    if (!fresh) {
      if (it->second == digest) return;
      fail("moon bundle: cannot carry two different archives named '" + name +
           "' (one of them from " + archivePath + ")");
    }
    if (inherited) report.inheritedArchives.push_back(name);
    writer.addNativeArchive(name, std::move(data));
  };
  for (const auto& archivePath : report.archiveFiles) carry(archivePath, false);
  for (const auto& archivePath : driver->getNativeArchivePaths()) {
    carry(archivePath, true);
  }
  if (!writer.write(outputPath)) {
    fail("Error writing moon: " + writer.getError());
  }
  return report;
}

}  // namespace sun
