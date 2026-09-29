// module_path.h — Module paths and the library hashes hidden inside them
//
// A module path is a list of names such as {"std", "io"}. The contents of an
// imported .moon live under an extra `$hash$` segment, so "$ab12$.std.io" is
// the path the compiler uses while "std.io" is what the program wrote. These
// helpers keep that hidden segment out of diagnostics and in one format.

#pragma once

#include <string>
#include <vector>

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {

/** Module path segments used for source lookup and diagnostic display. */
using ModulePath = std::vector<std::string>;

/** Reports whether a module-path segment is an internal library hash. */
inline bool isLibraryHashSegment(const std::string& seg) {
  return seg.size() >= 2 && seg.front() == '$' && seg.back() == '$';
}

/**
 * The hash inside a `$hash$` segment ("$ab12$" gives "ab12"), or an empty
 * string when the segment is not a library hash.
 */
inline std::string libraryHashOf(const std::string& seg) {
  if (!isLibraryHashSegment(seg)) return "";
  return seg.substr(1, seg.size() - 2);
}

/** The path as the program wrote it, without `$hash$` or empty segments. */
inline ModulePath visibleModulePath(const ModulePath& path) {
  ModulePath out;
  for (const auto& seg : path) {
    if (!seg.empty() && !isLibraryHashSegment(seg)) out.push_back(seg);
  }
  return out;
}

/**
 * "a.b.c" — drops `$hash$` segments; "" for the root.
 */
inline std::string displayModulePath(const ModulePath& path) {
  std::string out;
  for (const auto& seg : visibleModulePath(path)) {
    if (!out.empty()) out += '.';
    out += seg;
  }
  return out;
}

/** Splits a dotted module path into its individual names. */
inline ModulePath splitModulePath(const std::string& dotted) {
  ModulePath out;
  std::string cur;
  for (char c : dotted) {
    if (c == '.') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

/**
 * The dotted path as source code spells it: "$hash$.std.io" reads "std.io".
 * Diagnostics use this so a library's bundle hash never reaches the user.
 */
inline std::string displayModulePath(const std::string& dotted) {
  return displayModulePath(splitModulePath(dotted));
}

}  // namespace sun::semantic_analysis
