#include "cli/bindgen_command.h"

#include <iostream>

#include "cli/command_support.h"
#include "driver/c_bindings.h"

/** Parses command-line options and runs the selected compiler command. */
namespace sun::cli {
int runBindgenCommand(const std::vector<std::string>& arguments) {
  sun::driver::CBindingOptions options;
  for (size_t i = 0; i < arguments.size(); ++i) {
    const auto& argument = arguments[i];
    if (argument == "--") {
      options.clangArgs.assign(arguments.begin() + i + 1, arguments.end());
      break;
    }
    if (argument == "--help" || argument == "-h") {
      std::cout
          << "usage: sun bindgen HEADER --target TRIPLE --module NAME "
             "--allow DECL --output FILE [-- CLANG_ARGS...]\n\n"
             "Generate Sun C bindings and target layout checks.\n"
             "Repeat --allow to select declarations and their dependencies.\n"
             "Use --output - to write to standard output.\n";
      return 0;
    }
    if (argument.starts_with("--")) {
      const auto equal = argument.find('=');
      const auto flag = argument.substr(0, equal);
      if (flag != "--target" && flag != "--module" && flag != "--allow" &&
          flag != "--output") {
        sun::support::logErrorNoThrow("sun bindgen: unknown option " + flag);
        return 2;
      }
      std::string value;
      if (equal != std::string::npos) {
        value = argument.substr(equal + 1);
      } else if (i + 1 < arguments.size() &&
                 !arguments[i + 1].starts_with("--")) {
        value = arguments[++i];
      }
      if (value.empty()) {
        sun::support::logErrorNoThrow("sun bindgen: " + flag +
                                      " requires a value");
        return 2;
      }
      if (flag == "--target") {
        options.target = value;
      }
      if (flag == "--module") {
        options.module = value;
      }
      if (flag == "--output") {
        options.output = value;
      }
      if (flag == "--allow") {
        options.allow.push_back(value);
      }
    } else if (options.header.empty() && !argument.starts_with("-")) {
      options.header = argument;
    } else {
      sun::support::logErrorNoThrow("sun bindgen: unexpected argument " +
                                    argument);
      return 2;
    }
  }
  if (options.header.empty() || options.target.empty() ||
      options.module.empty() || options.output.empty() ||
      options.allow.empty()) {
    sun::support::logErrorNoThrow(
        "sun bindgen: header, --target, --module, --allow and --output are "
        "required");
    return 2;
  }
  try {
    sun::driver::generateCBinding(options);
    return 0;
  } catch (const sun::support::SunError& error) {
    return reportSunError(error);
  } catch (const std::exception& error) {
    return reportUnexpectedError(error);
  }
}
}  // namespace sun::cli
