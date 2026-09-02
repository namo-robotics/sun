// depfile.h — Make-format dependency files (`sun --depfile <file>`).
//
// A build system only reruns a compile when a file it knows about changes,
// and it cannot know which files a Sun manifest pulls in: source_files,
// test_files, imported bundles, proto schemas and native archives are all
// resolved by the compiler. A depfile closes that gap the way C compilers
// do with -MD: after building, the compiler writes
//
//   <output>: <input> <input> ...
//
// for every artifact it produced, and Ninja or Make read that back as the
// artifact's dependencies. Absolute paths throughout; spaces, '#' and '$'
// are escaped the way Make and Ninja expect.

#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace sun {

/*
 * Collects (output -> inputs) rules while a build runs, and writes them out
 * as one depfile at the end.
 */
class Depfile {
 public:
  // Record that `output` was built from `inputs`. Duplicates within one
  // rule are dropped; order is otherwise kept.
  void addOutput(const std::string& output,
                 const std::vector<std::string>& inputs);

  // A file every rule depends on, such as the sun-config.json that named
  // the outputs. Appended to each rule when rendering.
  void addSharedInput(const std::string& input);

  // The depfile text. An input that is itself one of the recorded outputs
  // is left out: it was made by this same run, so it is ordering inside the
  // command rather than a dependency of it.
  std::string render() const;

  // Write render() to `path`, creating parent folders. Throws SunError when
  // the file cannot be written.
  void write(const std::filesystem::path& path) const;

  // How many outputs were recorded.
  size_t size() const { return rules_.size(); }

 private:
  std::vector<std::pair<std::string, std::vector<std::string>>> rules_;
  std::vector<std::string> sharedInputs_;
};

}  // namespace sun
