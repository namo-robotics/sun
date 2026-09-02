// depfile.cpp — see depfile.h

#include "driver/depfile.h"

#include <fstream>
#include <set>

#include "support/error.h"

namespace sun {

namespace {

/*
 * A path as Make and Ninja read it back: spaces and '#' escaped with a
 * backslash, '$' doubled, and an absolute form so the build directory the
 * consumer runs in does not matter.
 */
std::string escapePath(const std::string& path) {
  std::error_code ec;
  std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  std::string text = ec ? path : absolute.lexically_normal().string();
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c == ' ' || c == '#') {
      out += '\\';
    } else if (c == '$') {
      out += '$';
    }
    out += c;
  }
  return out;
}

}  // namespace

void Depfile::addOutput(const std::string& output,
                        const std::vector<std::string>& inputs) {
  std::vector<std::string> unique;
  std::set<std::string> seen;
  for (const auto& input : inputs) {
    if (input.empty() || !seen.insert(input).second) continue;
    unique.push_back(input);
  }
  rules_.emplace_back(output, std::move(unique));
}

void Depfile::addSharedInput(const std::string& input) {
  if (!input.empty()) sharedInputs_.push_back(input);
}

std::string Depfile::render() const {
  // An artifact this same run produced is not an input of the run: one
  // build command makes every entrypoint of a config, so tls.moon reading
  // stdlib.moon is ordering inside the command, and naming it here would
  // make the command depend on its own output (a cycle to Ninja).
  std::set<std::string> produced;
  for (const auto& [output, inputs] : rules_) {
    produced.insert(escapePath(output));
  }

  std::string text;
  for (const auto& [output, inputs] : rules_) {
    text += escapePath(output) + ":";
    for (const auto& input : inputs) {
      const std::string escaped = escapePath(input);
      if (produced.count(escaped)) continue;
      text += " \\\n  " + escaped;
    }
    for (const auto& input : sharedInputs_) {
      text += " \\\n  " + escapePath(input);
    }
    text += "\n";
  }
  return text;
}

void Depfile::write(const std::filesystem::path& path) const {
  std::error_code ec;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream out(path);
  if (!out) {
    throw SunError(SunError::Kind::Compile,
                   "cannot write depfile '" + path.string() + "'");
  }
  out << render();
}

}  // namespace sun
