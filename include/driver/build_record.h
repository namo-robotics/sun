// build_record.h — what an executable remembers about the build that made it.
//
// A .moon bundle already carries the input hash it was built from. An
// executable or object file gets the same through a small section of its
// own, holding a build record. The next run reads the record back, and when
// the input hash it would build from is the one recorded, skips the build.
//
// The record lives inside the artifact rather than beside it, so it cannot
// outlast the artifact or describe a file that has since been replaced.

#pragma once

#include <llvm/IR/Module.h>

#include <optional>
#include <string>

/** Coordinates compilation, dependency loading, linking, and program execution.
 */
namespace sun::driver {

/**
 * The facts a later run needs to decide there is nothing to do. The two
 * flags say which artifacts the program yields, which is otherwise known
 * only after compiling it: a program without tests has no test binary to
 * look for, and a program of only tests has no executable.
 */
struct BuildRecord {
  std::string inputHash;      // see computeInputHash
  bool hasTests = false;      // a test binary is built alongside
  bool hasExecutable = true;  // an executable is built alongside
};

/**
 * Add `record` to `module` so that it ends up in the object file, and in any
 * executable linked from it. Call once per module, just before emitting it.
 */
void embedBuildRecord(llvm::Module& module, const BuildRecord& record);

/**
 * The record inside the executable or object file at `path`. Empty when the
 * file is missing, is not an object file, or carries no record.
 */
std::optional<BuildRecord> readBuildRecord(const std::string& path);

/**
 * The input hash a .moon bundle was built from. Empty when the file is
 * missing or is not a readable bundle.
 */
std::optional<std::string> readMoonInputHash(const std::string& path);

}  // namespace sun::driver
