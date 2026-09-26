// option_parser.cpp — turn `sun` command-line arguments into option structs.

#include "cli/option_parser.h"

#include "cli/command_support.h"
#include "cli/usage.h"
#include "driver/compiler.h"
#include "moon_bundling/moon_import.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {

/** Keeps the implementation helpers in this file private to this translation
 * unit. */
namespace {

/**
 * An early exit that reports a mistake on stderr.
 */
EarlyExit makeFailure(const std::string& text, int exitCode = 1) {
  return EarlyExit{exitCode, EarlyExit::Stream::Err, text};
}

/**
 * Walks the argument list one argument at a time.
 */
class ArgumentCursor {
 public:
  /** Borrows the argument list and starts reading at its first element. */
  explicit ArgumentCursor(const std::vector<std::string>& args) : args_(args) {}

  /** Reports whether all command-line arguments have been consumed. */
  bool atEnd() const { return index_ >= args_.size(); }
  /** Returns the current command-line argument without advancing. */
  const std::string& current() const { return args_[index_]; }
  /** Moves the cursor to the next command-line argument. */
  void advance() { ++index_; }

  /**
   * True when the current argument is `flag` and another argument follows.
   * That argument becomes the value, whatever it looks like, and the cursor
   * moves onto it. A flag with nothing after it is left alone, so the caller
   * goes on to report it as an unknown option.
   */
  bool takeValueOf(const std::string& flag, std::string& value) {
    if (current() != flag || index_ + 1 >= args_.size()) return false;
    value = args_[++index_];
    return true;
  }

  /**
   * Every argument after the current one.
   */
  std::vector<std::string> getRemaining() const {
    return std::vector<std::string>(args_.begin() + index_ + 1, args_.end());
  }

 private:
  const std::vector<std::string>& args_;
  size_t index_ = 0;
};

/**
 * Handle the current argument if it is one of the flags every command
 * accepts. Returns true when it was; a malformed value is reported through
 * `failure`.
 */
bool parseSharedOption(ArgumentCursor& cursor, SharedOptions& shared,
                       std::optional<EarlyExit>& failure) {
  const std::string& arg = cursor.current();
  std::string value;
  if (arg == "--debug") {
    shared.debugMode = true;
  } else if (arg == "-O0") {
    shared.optimize = false;
  } else if (arg == "-g") {
    shared.debugInfo = true;
  } else if (arg == "--emit-ir") {
    shared.emitIR = true;
  } else if (cursor.takeValueOf("--lib-path", value)) {
    shared.libPaths.push_back(value);
  } else if (cursor.takeValueOf("--moon", value)) {
    auto moonImport = sun::moon_bundling::parseMoonImportSpec(value);
    if (!moonImport) {
      failure = makeFailure("Invalid --moon format: " + value +
                            "\nExpected: path.moon or "
                            "path.moon:module=alias\n");
      return true;
    }
    shared.moonImports.push_back(std::move(*moonImport));
  } else if (cursor.takeValueOf("--path-var", value)) {
    auto eq = value.find('=');
    if (eq == std::string::npos || eq == 0) {
      failure = makeFailure("Invalid --path-var format: " + value +
                            "\nExpected: NAME=<dir>\n");
      return true;
    }
    shared.pathVariables.emplace_back(value.substr(0, eq),
                                      value.substr(eq + 1));
  } else {
    return false;
  }
  return true;
}

/** Reports whether an argument begins with an option prefix. */
bool looksLikeOption(const std::string& arg) {
  return !arg.empty() && arg[0] == '-';
}

}  // namespace

/** Parses command-line arguments into build run options and reports early
 * exits. */
std::optional<EarlyExit> parseBuildRunArguments(
    const std::string& programName, const std::vector<std::string>& args,
    BuildRunOptions& options) {
  for (ArgumentCursor cursor(args); !cursor.atEnd(); cursor.advance()) {
    const std::string& arg = cursor.current();
    std::optional<EarlyExit> failure;
    std::string value;
    if (arg == "--") {
      // Everything after -- is passed to the Sun program
      options.programArgs = cursor.getRemaining();
      break;
    } else if (parseSharedOption(cursor, options.shared, failure)) {
      if (failure) return failure;
    } else if (arg == "-c" || arg == "--compile") {
      options.compileMode = true;
    } else if (cursor.takeValueOf("-o", value)) {
      options.outputFile = value;
    } else if (arg == "--emit-obj") {
      options.compileMode = true;
      options.emitObjOnly = true;
    } else if (cursor.takeValueOf("--target", value)) {
      options.targetTriple = value;
    } else if (cursor.takeValueOf("--sysroot", value)) {
      options.linkOptions.sysroot = value;
    } else if (arg == "--static") {
      options.staticRequested = true;
    } else if (arg == "--dynamic") {
      options.dynamicRequested = true;
    } else if (arg == "--emit-moon") {
      options.emitMoon = true;
    } else if (arg == "--dump-proto-sun") {
      options.dumpProtoSun = true;
    } else if (arg == "--no-test") {
      options.noTest = true;
    } else if (arg == "--refresh-sources") {
      options.refreshSources = true;
    } else if (arg == "--force-rebuild") {
      options.forceRebuild = true;
    } else if (cursor.takeValueOf("-l", value)) {
      options.linkOptions.libraries.push_back(value);
    } else if (arg.rfind("-l", 0) == 0 && arg.size() > 2) {
      options.linkOptions.libraries.push_back(arg.substr(2));
    } else if (cursor.takeValueOf("-L", value)) {
      options.linkOptions.searchPaths.push_back(value);
    } else if (arg.rfind("-L", 0) == 0 && arg.size() > 2) {
      options.linkOptions.searchPaths.push_back(arg.substr(2));
    } else if (cursor.takeValueOf("--gh-token", value)) {
      options.githubToken = value;
    } else if (arg == "-h" || arg == "--help") {
      return EarlyExit{0, EarlyExit::Stream::Err, renderUsage(programName)};
    } else if (arg == "--version") {
      return EarlyExit{0, EarlyExit::Stream::Out, renderVersionLine()};
    } else if (looksLikeOption(arg)) {
      return makeFailure("Unknown option: " + arg + "\n" +
                         renderUsage(programName));
    } else {
      options.inputFiles.push_back(arg);
    }
  }

  // Linking is static by default: one self-contained binary, the deployment
  // shape embedded targets want. --dynamic restores shared-library linking
  // (needed for .so-only vendor libraries). macOS is always dynamic.
  bool darwinTarget =
      sun::driver::effectiveLinkTriple(options.targetTriple).isOSDarwin();
  options.linkOptions.staticLink = !options.dynamicRequested && !darwinTarget;

  // A sun-config.json named as the input stands in for its declared
  // entrypoints: -c builds every product, plain run executes the single
  // binary entrypoint.
  options.configInput =
      options.inputFiles.size() == 1 && isConfigInput(options.inputFiles[0]);

  return validateBuildRunOptions(options, programName);
}

/** Checks for incompatible or incomplete build and execution options. */
std::optional<EarlyExit> validateBuildRunOptions(
    const BuildRunOptions& options, const std::string& programName) {
  bool linksExecutable = options.compileMode && !options.emitObjOnly;

  // Cross-compilation produces object files and .moon artifacts; the JIT can
  // only run host code.
  if (!options.targetTriple.empty() && !options.emitObjOnly &&
      !options.emitMoon && !options.compileMode) {
    return makeFailure(
        "Error: --target requires --emit-obj, -c or --emit-moon "
        "(JIT execution is host-only)\n");
  }
  if (options.staticRequested && options.dynamicRequested) {
    return makeFailure(
        "Error: --static and --dynamic are mutually exclusive\n");
  }
  if (options.noTest && !linksExecutable) {
    return makeFailure(
        "Error: --no-test only applies when linking an "
        "executable; use it with -c\n");
  }
  if (options.staticRequested && !linksExecutable) {
    return makeFailure(
        "Error: --static only applies when linking; use it "
        "with -c\n");
  }
  if (options.refreshSources && (!options.compileMode || !options.configInput)) {
    return makeFailure("Error: --refresh-sources requires -c sun-config.json\n");
  }
  if (options.forceRebuild && !options.compileMode && !options.emitMoon) {
    return makeFailure(
        "Error: --force-rebuild is about built artifacts; use it with -c "
        "or --emit-moon\n");
  }
  // macOS has no fully static binaries: Apple ships no static libSystem or
  // startup objects, and its linker rejects -static for executables.
  if (options.staticRequested &&
      sun::driver::effectiveLinkTriple(options.targetTriple).isOSDarwin()) {
    return makeFailure("Error: --static is not supported for macOS targets\n");
  }
  if (options.configInput && (options.emitMoon || options.emitObjOnly)) {
    return makeFailure(
        "Error: a sun-config.json input works with -c or plain "
        "run; libraries in its entrypoints already build their "
        ".moon bundles under -c\n");
  }
  if (options.configInput && !options.outputFile.empty()) {
    return makeFailure(
        "Error: -o does not combine with a sun-config.json "
        "input; the config's output_name fields name the "
        "artifacts\n");
  }
  if (options.inputFiles.empty()) {
    if (options.emitMoon) {
      return makeFailure("Error: --emit-moon requires an entrypoint file\n");
    }
    return makeFailure("Error: No input file specified.\n" +
                       renderUsage(programName));
  }
  return std::nullopt;
}

/** Parses command-line arguments into test options and reports early exits. */
std::optional<EarlyExit> parseTestArguments(
    const std::vector<std::string>& args, TestOptions& options) {
  for (ArgumentCursor cursor(args); !cursor.atEnd(); cursor.advance()) {
    const std::string& arg = cursor.current();
    std::optional<EarlyExit> failure;
    std::string value;
    if (arg == "--") {
      std::vector<std::string> rest = cursor.getRemaining();
      options.forwardedArgs.insert(options.forwardedArgs.end(), rest.begin(),
                                   rest.end());
      break;
    } else if (arg == "--test-sequential") {
      options.forwardedArgs.push_back(arg);
    } else if (cursor.takeValueOf("--test-filter", value)) {
      options.forwardedArgs.push_back("--test-filter");
      options.forwardedArgs.push_back(value);
    } else if (parseSharedOption(cursor, options.shared, failure)) {
      if (failure) return failure;
    } else if (looksLikeOption(arg)) {
      return makeFailure("Unknown option for 'sun test': " + arg + "\n" +
                         kTestUsageFull);
    } else if (options.inputFile.empty()) {
      options.inputFile = arg;
    } else {
      return makeFailure("'sun test' takes one entrypoint file\n");
    }
  }

  if (options.inputFile.empty()) {
    return makeFailure(kTestUsageBrief);
  }
  return std::nullopt;
}

/** Parses command-line arguments into fmt options and reports early exits. */
std::optional<EarlyExit> parseFmtArguments(const std::vector<std::string>& args,
                                           FmtOptions& options) {
  for (const std::string& arg : args) {
    if (arg == "--check") {
      options.checkMode = true;
    } else if (arg == "-h" || arg == "--help") {
      return EarlyExit{0, EarlyExit::Stream::Err, kFmtUsage};
    } else if (looksLikeOption(arg)) {
      return makeFailure("Unknown fmt option: " + arg + "\n", /*exitCode=*/2);
    } else {
      options.inputs.push_back(arg);
    }
  }
  if (options.inputs.empty()) {
    return makeFailure(kFmtUsage, /*exitCode=*/2);
  }
  return std::nullopt;
}

}  // namespace sun::cli
