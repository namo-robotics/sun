// moon_builder.cpp — see moon_builder.h

#include "moon_bundling/moon_builder.h"

#include "driver/driver.h"
#include "support/error.h"
#include "driver/manifest_processor.h"
#include "moon_bundling/metadata_extractor.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/proto_importer.h"

namespace sun {

namespace {

[[noreturn]] void fail(const std::string& message) {
  throw SunError(SunError::Kind::Compile, message);
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
  if (auto manifest = ManifestProcessor::fromEntrypointFile(entrypoint)) {
    report.sunFiles = std::move(manifest->sunFiles);
    report.moonImports.insert(report.moonImports.end(),
                              manifest->moonImports.begin(),
                              manifest->moonImports.end());
    report.protoFiles = std::move(manifest->protoFiles);
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

  // ---- Compile everything into one LLVM module ----
  auto driver = Driver::createForAOT("moon_module", options.targetTriple,
                                     options.debugInfo);
  driver->setDumpProtoSun(options.dumpProtoSun);
  driver->compileFiles(report.sunFiles, report.moonImports, report.protoFiles);

  // ---- Write the bundle: each module's metadata + the shared code ----
  MoonWriter writer;
  for (const auto& metadata : allMetadata) {
    writer.addModule(driver->getModule(), metadata);
  }
  if (!writer.write(outputPath)) {
    fail("Error writing moon: " + writer.getError());
  }
  return report;
}

}  // namespace sun
