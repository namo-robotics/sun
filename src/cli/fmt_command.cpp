// fmt_command.cpp — `sun fmt`, the source formatter command.

#include "cli/fmt_command.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cli/command_support.h"
#include "cli/option_parser.h"
#include "llvm/Support/raw_ostream.h"
#include "parsing/formatter.h"
#include "support/error.h"

namespace sun::cli {

namespace {

// What happened to one file.
enum class FormatOutcome { Unchanged, Changed, Failed };

// Collect .sun files under a directory, skipping hidden directories
// (.git, .cache, ...). Sorted so output order is deterministic.
bool collectSunFiles(const std::string& dir, std::vector<std::string>& out) {
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(dir, ec), end;
  if (ec) {
    llvm::errs() << dir << ": cannot read directory: " << ec.message() << "\n";
    return false;
  }
  size_t firstNew = out.size();
  for (; it != end; it.increment(ec)) {
    if (ec) {
      llvm::errs() << dir << ": error while scanning: " << ec.message() << "\n";
      return false;
    }
    const std::filesystem::path& path = it->path();
    std::string name = path.filename().string();
    if (it->is_directory(ec)) {
      if (name.size() > 1 && name[0] == '.') it.disable_recursion_pending();
      continue;
    }
    // *.inja.sun files are templates, not Sun programs: their splices
    // cannot parse, so the formatter leaves them alone.
    if (path.extension() == ".sun" &&
        std::filesystem::path(path.stem()).extension() != ".inja") {
      out.push_back(path.string());
    }
  }
  std::sort(out.begin() + firstNew, out.end());
  return true;
}

// Turn the inputs into the list of files to format. Directories are
// expanded; explicitly named non-.sun files are reported (a directory walk
// filters them silently instead). Returns false when any input could not be
// read.
bool expandInputs(const std::vector<std::string>& inputs,
                  std::vector<std::string>& files) {
  bool ok = true;
  for (const auto& input : inputs) {
    std::error_code ec;
    if (std::filesystem::is_directory(input, ec)) {
      if (!collectSunFiles(input, files)) ok = false;
    } else if (!std::filesystem::exists(input, ec)) {
      llvm::errs() << input << ": no such file or directory\n";
      ok = false;
    } else if (std::filesystem::path(input).extension() != ".sun") {
      llvm::errs() << input << ": skipped (not a .sun file)\n";
    } else {
      files.push_back(input);
    }
  }
  return ok;
}

// Replace a file's contents in one step: write a temp file in the same
// directory, then rename it over the original.
bool rewriteFile(const std::string& file, const std::string& contents) {
  std::string tmpPath = file + ".fmt-tmp";
  {
    std::ofstream out(tmpPath, std::ios::trunc);
    if (!out) {
      llvm::errs() << file << ": cannot write " << tmpPath << "\n";
      return false;
    }
    out << contents;
  }
  std::error_code ec;
  std::filesystem::rename(tmpPath, file, ec);
  if (ec) {
    llvm::errs() << file << ": rename failed: " << ec.message() << "\n";
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  return true;
}

// Format one file. In check mode a file that would change is only reported.
FormatOutcome formatFile(const std::string& file, bool checkMode) {
  std::ifstream in(file);
  if (!in) {
    llvm::errs() << file << ": cannot open file\n";
    return FormatOutcome::Failed;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string source = buffer.str();
  in.close();

  std::string formatted;
  try {
    formatted = sun::formatSource(source, file);
  } catch (const SunError& e) {
    llvm::errs() << file << ": " << e.what() << "\n";
    return FormatOutcome::Failed;
  }

  if (formatted == source) return FormatOutcome::Unchanged;
  if (checkMode) {
    llvm::outs() << file << ": needs formatting\n";
    return FormatOutcome::Changed;
  }
  return rewriteFile(file, formatted) ? FormatOutcome::Changed
                                      : FormatOutcome::Failed;
}

}  // namespace

int runFmtCommand(const std::vector<std::string>& args) {
  FmtOptions options;
  if (auto earlyExit = parseFmtArguments(args, options)) {
    return reportEarlyExit(*earlyExit);
  }

  std::vector<std::string> files;
  bool hadError = !expandInputs(options.inputs, files);
  bool hadDiff = false;
  for (const auto& file : files) {
    switch (formatFile(file, options.checkMode)) {
      case FormatOutcome::Unchanged:
        break;
      case FormatOutcome::Changed:
        hadDiff = true;
        break;
      case FormatOutcome::Failed:
        hadError = true;
        break;
    }
  }

  if (hadError) return 2;
  if (options.checkMode && hadDiff) return 1;
  return 0;
}

}  // namespace sun::cli
