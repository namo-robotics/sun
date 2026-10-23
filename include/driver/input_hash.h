// input_hash.h — one hash over everything a build reads.
//
// Before anything is compiled, sun hashes every input that can change the
// artifact: the text of each source, the archives and bundles it links
// against, the target and flags, and the compiler itself. That one hash does
// two jobs.
//
// It lets a build be skipped. Every artifact records the hash it was built
// from, so a later run that arrives at the same hash has nothing to do. A
// build tool can therefore run sun unconditionally and leave the question
// "did anything change?" to the only program that knows what a manifest
// pulls in.
//
// It names a .moon bundle. The bundle's symbols are spelled under `$hash$`,
// so two bundles that differ in any input never collide, and two that agree
// in every input are the same bundle.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "moon_bundling/moon_import.h"
#include "moon_bundling/proto_importer.h"

/** Coordinates compilation, dependency loading, linking, and program execution.
 */
namespace sun::driver {

/**
 * Everything one build reads, reduced to digests and settings. Sources and
 * archives are given by content, not by path, so moving a project or saving
 * a file unchanged leaves the hash alone.
 */
struct BuildInputs {
  // What is being built: "bundle", "executable", "object" or "tests". The
  // same sources make a different artifact of each kind.
  std::string artifactKind;
  // Diagnostic label only; excluded from the input hash.
  std::string entrypoint;
  // Digest of each source text, generated sources included, in any order
  std::vector<std::string> sourceDigests;
  // Native archives, each as its file name and a digest of its bytes
  std::vector<std::pair<std::string, std::string>> archives;
  // Imported bundles, with their paths already resolved
  std::vector<sun::moon_bundling::MoonImport> moonImports;
  std::string targetTriple;  // empty = host
  bool debugInfo = false;
  bool optimize = true;
  // Anything else that shapes the artifact, such as link flags, as
  // (name, value) pairs. Order is kept: link order is meaningful.
  std::vector<std::pair<std::string, std::string>> settings;
};

/**
 * The hash of `inputs` and of the running compiler, as lowercase hex. Throws
 * SunError when an imported bundle cannot be read.
 */
std::string computeInputHash(const BuildInputs& inputs);

/**
 * Digest of the running executable's bytes, so output from a rebuilt
 * compiler is never mistaken for output from the one before it. Read once
 * per process.
 */
const std::string& getCompilerDigest();

/**
 * Digest of a file's bytes, as lowercase hex. `what` names the file's role
 * in the error ("source", "native archive"). Throws SunError when it cannot
 * be read.
 */
std::string computeFileDigest(const std::string& path, const char* what);

/**
 * Append content hashes of source files and already synthesized protobuf
 * modules. Callers must generate source files before hashing them.
 * Throws SunError when a source file cannot be read.
 */
void addSourceDigests(
    BuildInputs& inputs, const std::vector<std::string>& sourceFiles,
    const std::vector<sun::moon_bundling::SynthesizedProtoModule>&
        protoModules);

}  // namespace sun::driver
