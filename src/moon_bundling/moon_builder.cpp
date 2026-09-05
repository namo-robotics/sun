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

// The bundle's content hash, decided before anything is compiled so the
// compiler can spell the bundle's own symbols with it. It has to change
// whenever the code image would: it covers every source, every bundle the
// code links against (and how those are aliased), the target, the debug
// setting and the compiler itself. Importers rely on distinct bundles
// carrying distinct hashes, and a symbol prefix must not collide.
std::string computeBundleHash(const std::vector<moon::ModuleMetadata>& metadata,
                              const std::vector<MoonImport>& moonImports,
                              const MoonBuildOptions& options) {
  std::string input;
  // Sorted, so the hash does not depend on manifest order
  std::vector<std::string> sources;
  for (const auto& m : metadata) sources.push_back(m.source_hash());
  std::sort(sources.begin(), sources.end());
  for (const auto& h : sources) input += "source:" + h + "\n";

  for (const auto& import : moonImports) {
    auto reader = MoonReader::open(import.path);
    if (!reader) fail("Cannot open imported moon: " + import.path);
    const auto modules = reader->listModules();
    const auto* first =
        modules.empty() ? nullptr : reader->getMetadata(modules[0]);
    if (!first) fail("Imported moon has no modules: " + import.path);
    input += "moon:" + first->content_hash() + "\n";
    // An alias changes which symbols this bundle's code refers to
    for (const auto& [from, to] : std::map<std::string, std::string>(
             import.moduleRemap.begin(), import.moduleRemap.end())) {
      input += "alias:" + from + "=" + to + "\n";
    }
  }

  input += "target:" +
           (options.targetTriple.empty() ? llvm::sys::getDefaultTargetTriple()
                                         : options.targetTriple) +
           "\n";
  input += std::string("debug:") + (options.debugInfo ? "1" : "0") + "\n";
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

  // ---- Exportable metadata: .sun files, then synthesized proto modules ----
  std::vector<moon::ModuleMetadata> allMetadata;
  auto collect = [&](std::optional<std::vector<moon::ModuleMetadata>> md,
                     const std::string& what) {
    if (!md) fail("Failed to parse " + what + " for metadata");
    for (auto& m : *md) {
      if (!m.module_name().empty()) report.modules.push_back(m.module_name());
      allMetadata.push_back(std::move(m));
    }
  };
  for (const auto& file : report.sunFiles) {
    collect(extractAllMetadataFromFile(file), file);
  }
  // Importers of this moon get the message classes without needing the
  // .proto (or libprotoc): the synthesized source is exported like any
  // other module and compiled into the bundle below
  for (const auto& synthesized :
       ProtoImporter::importAll(report.protoFiles, baseDir)) {
    collect(extractAllMetadataFromSource(synthesized.sunSource,
                                         synthesized.pseudoPath, baseDir),
            synthesized.pseudoPath);
  }

  // Store deterministic file identities local to this bundle.
  std::map<sun::SourceFileId, sun::SourceFileId> sourceFiles;
  for (auto& metadata : allMetadata) {
    serialization::remapSourceFiles(metadata, [&](sun::SourceFileId id) {
      auto [it, inserted] = sourceFiles.try_emplace(id, sourceFiles.size() + 1);
      return it->second;
    });
  }

  // ---- Root modules must be public: a bundle whose top-level module is
  // private would export nothing reachable ----
  for (const auto& m : allMetadata) {
    const std::string& name = m.module_name();
    if (name.empty() || name.find('.') != std::string::npos) continue;
    if (m.visibility() != ast::PUBLIC) {
      fail("moon bundle: top-level module '" + name +
           "' must be declared 'public' to be exported");
    }
  }

  // ---- Compile everything into one LLVM module, under the bundle's own
  // hash so its symbols are already the ones importers will look for ----
  const std::string bundleHash =
      computeBundleHash(allMetadata, report.moonImports, options);
  auto driver = Driver::createForAOT("moon_module", options.targetTriple,
                                     options.debugInfo);
  driver->setDumpProtoSun(options.dumpProtoSun);
  driver->setOwnBundleHash(bundleHash);
  driver->compileFiles(report.sunFiles, report.moonImports, report.protoFiles);

  // ---- Write the bundle: each module's metadata + the shared code ----
  MoonWriter writer(bundleHash);
  for (const auto& metadata : allMetadata) {
    writer.addModule(driver->getModule(), metadata);
  }
  // Native archives named by `archives:` travel inside the bundle, so
  // importers link against them without naming -l flags.
  for (const auto& archivePath : report.archiveFiles) {
    std::ifstream archiveIn(archivePath, std::ios::binary);
    if (!archiveIn) fail("Cannot read native archive: " + archivePath);
    std::string data((std::istreambuf_iterator<char>(archiveIn)),
                     std::istreambuf_iterator<char>());
    writer.addNativeArchive(fs::path(archivePath).filename().string(),
                            std::move(data));
  }
  if (!writer.write(outputPath)) {
    fail("Error writing moon: " + writer.getError());
  }
  return report;
}

}  // namespace sun
