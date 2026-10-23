// fmt_command.cpp — `sun fmt`, the source formatter command.

#include "cli/fmt_command.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "cli/command_support.h"
#include "cli/option_parser.h"
#include "driver/sun_config.h"
#include "llvm/Support/GlobPattern.h"
#include "llvm/Support/raw_ostream.h"
#include "parsing/formatter.h"
#include "support/error.h"
#include "support/terminal.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/**
 * What happened to one file.
 */
enum class FormatOutcome { Unchanged, Changed, Failed };

/** Splits a slash-separated pattern or relative path into components. */
std::vector<std::string> components(const std::string& path) {
  std::vector<std::string> result;
  std::istringstream input(path);
  for (std::string part; std::getline(input, part, '/');) {
    result.push_back(std::move(part));
  }
  return result;
}

/** Matches path components, with ** spanning zero or more directories. */
bool matchesPattern(const std::string& pattern, const std::string& path) {
  const auto patterns = components(pattern);
  const auto parts = components(path);
  std::vector<bool> matched(parts.size() + 1, false);
  matched[0] = true;
  for (size_t p = 0; p < patterns.size(); ++p) {
    const auto& part = patterns[p];
    if (part == "**") {
      if (p > 0 && p + 1 == patterns.size()) {
        for (size_t i = parts.size(); i > 0; --i) {
          matched[i] = matched[i - 1];
        }
        matched[0] = false;
      }
      for (size_t i = 1; i <= parts.size(); ++i) {
        matched[i] = matched[i] || matched[i - 1];
      }
      continue;
    }
    auto glob = llvm::GlobPattern::create(part);
    if (!glob) {
      sun::support::logAndThrowError("sun_fmt.ignore: invalid pattern '" +
                                     pattern +
                                     "': " + llvm::toString(glob.takeError()));
    }
    for (size_t i = parts.size(); i > 0; --i) {
      matched[i] = matched[i - 1] && glob->match(parts[i - 1]);
    }
    matched[0] = false;
  }
  return matched.back();
}

/** Caches inherited project settings and evaluates ordered ignore patterns. */
class FormatIgnores {
 public:
  /** Enables configured exclusions unless the caller explicitly bypasses them.
   */
  explicit FormatIgnores(bool disabled) : disabled_(disabled) {}

  /** Checks a path and its ancestors; excluded directories cannot be
   * re-entered. */
  bool ignores(const std::filesystem::path& path, bool directory) {
    if (disabled_) {
      return false;
    }
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto configDir = directory ? absolute : absolute.parent_path();
    auto found = configs_.find(configDir);
    if (found == configs_.end()) {
      found =
          configs_
              .emplace(configDir, sun::driver::SunConfig::findFrom(configDir))
              .first;
    }
    if (!found->second) {
      return false;
    }
    const auto& rules = found->second->fmtIgnore;
    for (auto candidate = absolute; candidate != candidate.parent_path();
         candidate = candidate.parent_path(), directory = true) {
      bool ignored = false;
      for (const auto& rule : rules) {
        auto relative = candidate.lexically_relative(rule.baseDir);
        if (relative.empty() || relative == "." || *relative.begin() == "..") {
          continue;
        }
        std::string pattern = rule.pattern;
        if (pattern.front() == '#') {
          continue;
        }
        const bool negate = pattern.front() == '!';
        if (negate) {
          pattern.erase(0, 1);
        }
        if (pattern.empty()) {
          continue;
        }
        const bool directoryOnly = pattern.back() == '/';
        if (directoryOnly) {
          pattern.pop_back();
        }
        if (directoryOnly && !directory) {
          continue;
        }
        const bool anchored = !pattern.empty() && pattern.front() == '/';
        if (anchored) {
          pattern.erase(0, 1);
        }
        const auto target = !anchored && pattern.find('/') == std::string::npos
                                ? candidate.filename().generic_string()
                                : relative.generic_string();
        if (matchesPattern(pattern, target)) {
          ignored = !negate;
        }
      }
      if (ignored) {
        return true;
      }
    }
    return false;
  }

 private:
  bool disabled_;
  std::map<std::filesystem::path, std::optional<sun::driver::SunConfig>>
      configs_;
};

/**
 * Collect .sun files under a directory, skipping hidden directories
 * (.git, .cache, ...). Sorted so output order is deterministic.
 */
bool collectSunFiles(const std::string& dir, std::vector<std::string>& out,
                     FormatIgnores& ignores) {
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(dir, ec), end;
  if (ec) {
    sun::support::messageStream("error")
        << dir << ": cannot read directory: " << ec.message() << "\n";
    return false;
  }
  size_t firstNew = out.size();
  for (; it != end; it.increment(ec)) {
    if (ec) {
      sun::support::messageStream("error")
          << dir << ": error while scanning: " << ec.message() << "\n";
      return false;
    }
    const std::filesystem::path& path = it->path();
    std::string name = path.filename().string();
    if (it->is_directory(ec)) {
      if ((name.size() > 1 && name[0] == '.') || ignores.ignores(path, true)) {
        it.disable_recursion_pending();
      }
      continue;
    }
    // *.inja.sun files are templates, not Sun programs: their splices
    // cannot parse, so the formatter leaves them alone.
    if (path.extension() == ".sun" &&
        std::filesystem::path(path.stem()).extension() != ".inja" &&
        !ignores.ignores(path, false)) {
      out.push_back(path.string());
    }
  }
  std::sort(out.begin() + firstNew, out.end());
  return true;
}

/**
 * Turn the inputs into the list of files to format. Directories are
 * expanded; explicitly named non-.sun files are reported (a directory walk
 * filters them silently instead). Returns false when any input could not be
 * read.
 */
bool expandInputs(const std::vector<std::string>& inputs,
                  std::vector<std::string>& files, FormatIgnores& ignores) {
  bool ok = true;
  for (const auto& input : inputs) {
    std::error_code ec;
    const bool directory = std::filesystem::is_directory(input, ec);
    if (std::filesystem::exists(input, ec) &&
        ignores.ignores(input, directory)) {
      sun::support::messageStream("info")
          << input << ": skipped (sun_fmt.ignore)\n";
    } else if (directory) {
      if (!collectSunFiles(input, files, ignores)) {
        ok = false;
      }
    } else if (!std::filesystem::exists(input, ec)) {
      sun::support::messageStream("error")
          << input << ": no such file or directory\n";
      ok = false;
    } else if (std::filesystem::path(input).extension() != ".sun") {
      sun::support::messageStream("warning")
          << input << ": skipped (not a .sun file)\n";
    } else {
      files.push_back(input);
    }
  }
  return ok;
}

/**
 * Replace a file's contents in one step: write a temp file in the same
 * directory, then rename it over the original.
 */
bool rewriteFile(const std::string& file, const std::string& contents) {
  std::string tmpPath = file + ".fmt-tmp";
  {
    std::ofstream out(tmpPath, std::ios::trunc);
    if (!out) {
      sun::support::messageStream("error")
          << file << ": cannot write " << tmpPath << "\n";
      return false;
    }
    out << contents;
  }
  std::error_code ec;
  std::filesystem::rename(tmpPath, file, ec);
  if (ec) {
    sun::support::messageStream("error")
        << file << ": rename failed: " << ec.message() << "\n";
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  return true;
}

/**
 * Format one file. In check mode a file that would change is only reported.
 */
FormatOutcome formatFile(const std::string& file, bool checkMode) {
  std::ifstream in(file);
  if (!in) {
    sun::support::messageStream("error") << file << ": cannot open file\n";
    return FormatOutcome::Failed;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string source = buffer.str();
  in.close();

  std::string formatted;
  try {
    formatted = sun::parsing::formatSource(source, file);
  } catch (const sun::support::SunError& e) {
    sun::support::messageStream("error") << file << ": " << e.what() << "\n";
    return FormatOutcome::Failed;
  }

  if (formatted == source) {
    return FormatOutcome::Unchanged;
  }
  if (checkMode) {
    llvm::outs() << sun::support::messagePrefix("info", llvm::outs()) << file
                 << ": needs formatting\n";
    return FormatOutcome::Changed;
  }
  return rewriteFile(file, formatted) ? FormatOutcome::Changed
                                      : FormatOutcome::Failed;
}

}  // namespace

/** Runs the fmt command and returns its process exit status. */
int runFmtCommand(const std::vector<std::string>& args) {
  FmtOptions options;
  if (auto earlyExit = parseFmtArguments(args, options)) {
    return reportEarlyExit(*earlyExit);
  }

  std::vector<std::string> files;
  FormatIgnores ignores(options.noIgnore);
  bool hadError;
  try {
    hadError = !expandInputs(options.inputs, files, ignores);
  } catch (const sun::support::SunError& error) {
    sun::support::messageStream("error") << error.what() << "\n";
    return 2;
  }
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

  if (hadError) {
    return 2;
  }
  if (options.checkMode && hadDiff) {
    return 1;
  }
  return 0;
}

}  // namespace sun::cli
