// input_hash.cpp — see input_hash.h

#include "driver/input_hash.h"

#include <llvm/ADT/StringExtras.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/BLAKE3.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <map>

#include "generated/sun_version.h"
#include "moon_bundling/moon.h"
#include "moon_bundling/proto_importer.h"
#include "support/error.h"

namespace sun::driver {

namespace {

[[noreturn]] void fail(const std::string& message) {
  throw sun::support::SunError(sun::support::SunError::Kind::Compile, message);
}

/** Frame each input so arbitrary names cannot become field separators. */
std::string hashField(const std::string& tag, const std::string& value) {
  std::string result;
  for (const auto* field : {&tag, &value}) {
    const uint64_t size = field->size();
    for (int shift = 56; shift >= 0; shift -= 8)
      result.push_back(static_cast<char>((size >> shift) & 255));
    result += *field;
  }
  return result;
}

// Digest of a whole file's bytes. Files can be large (the compiler itself,
// native archives) and are read on every run, so this uses BLAKE3, which is
// several times faster than SHA-256 and just as fit for telling files apart.
std::string digestBytes(llvm::StringRef bytes) {
  llvm::BLAKE3 hasher;
  hasher.update(bytes);
  return llvm::toHex(hasher.final(), /*LowerCase=*/true);
}

// One imported bundle's part of the hash: the hash it was built from, which
// already covers everything inside it, and how its modules are renamed here.
std::string hashMoonImport(const sun::moon_bundling::MoonImport& import) {
  auto reader = sun::moon_bundling::MoonReader::open(import.path);
  if (!reader) fail("Cannot open imported moon: " + import.path);
  const auto modules = reader->listModules();
  const auto* first =
      modules.empty() ? nullptr : reader->getMetadata(modules[0]);
  if (!first) fail("Imported moon has no modules: " + import.path);
  std::string dependency = hashField("digest", first->content_hash());
  // An alias changes which symbols the importing code refers to
  for (const auto& [from, to] : std::map<std::string, std::string>(
           import.moduleRemap.begin(), import.moduleRemap.end())) {
    dependency +=
        hashField("alias", hashField("from", from) + hashField("to", to));
  }
  return dependency;
}

}  // namespace

const std::string& getCompilerDigest() {
  static const std::string digest = [] {
    const std::string exe = llvm::sys::fs::getMainExecutable(
        "sun", reinterpret_cast<void*>(&getCompilerDigest));
    if (!exe.empty()) {
      if (auto buffer = llvm::MemoryBuffer::getFile(exe, /*IsText=*/false,
                                                    /*RequiresNullTerminator=*/
                                                    false)) {
        return digestBytes((*buffer)->getBuffer());
      }
    }
    // The executable cannot be found or read; the release it came from is
    // the best identity left.
    return digestBytes(std::string(SUN_VERSION) + "-" + SUN_GIT_HASH);
  }();
  return digest;
}

std::string computeFileDigest(const std::string& path, const char* what) {
  auto buffer = llvm::MemoryBuffer::getFile(path, /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!buffer) fail(std::string("Cannot read ") + what + ": " + path);
  return digestBytes((*buffer)->getBuffer());
}

void addSourceDigests(BuildInputs& inputs,
                      const std::vector<std::string>& sourceFiles,
                      const std::vector<std::string>& protoFiles,
                      const std::string& baseDir) {
  for (const auto& path : sourceFiles) {
    inputs.sourceDigests.push_back(computeFileDigest(path, "source"));
  }
  for (const auto& source :
       sun::moon_bundling::ProtoImporter::importAll(protoFiles, baseDir)) {
    inputs.sourceDigests.push_back(digestBytes(source.sunSource));
  }
}

std::string computeInputHash(const BuildInputs& inputs) {
  std::string input = "sun.inputs.v1";
  input += hashField("kind", inputs.artifactKind);

  // Sorted, so the hash does not depend on manifest order
  std::vector<std::string> sources = inputs.sourceDigests;
  std::sort(sources.begin(), sources.end());
  for (const auto& digest : sources) input += hashField("source", digest);

  std::vector<std::string> archiveLines;
  for (const auto& [name, digest] : inputs.archives) {
    archiveLines.push_back(hashField("name", name) +
                           hashField("digest", digest));
  }
  std::sort(archiveLines.begin(), archiveLines.end());
  for (const auto& line : archiveLines) input += hashField("archive", line);

  std::vector<std::string> dependencies;
  for (const auto& import : inputs.moonImports) {
    dependencies.push_back(hashMoonImport(import));
  }
  std::sort(dependencies.begin(), dependencies.end());
  for (const auto& dependency : dependencies)
    input += hashField("moon", dependency);

  input += hashField("target", inputs.targetTriple.empty()
                                   ? llvm::sys::getDefaultTargetTriple()
                                   : inputs.targetTriple);
  input += hashField("debug", inputs.debugInfo ? "1" : "0");
  input += hashField("optimize", inputs.optimize ? "1" : "0");
  for (const auto& [name, value] : inputs.settings) {
    input += hashField("setting", hashField(name, value));
  }

  // The compiler is an input too. Its own bytes cover every change to it,
  // committed or not; LLVM is named separately because it may be a shared
  // library the executable's bytes say nothing about.
  input += hashField("compiler", getCompilerDigest());
  input += hashField("llvm", LLVM_VERSION_STRING);
  return sun::moon_bundling::computeSha256Hex(input);
}

}  // namespace sun::driver
