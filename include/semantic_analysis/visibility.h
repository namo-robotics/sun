// visibility.h — Access-modifier model shared by the parser, semantic analyzer,
// moon builder and tooling.
//
// Sun is private by default: `public` is the only modifier. Privacy is
// module-scoped: an item owned by module M is reachable from code whose
// enclosing module is M or a descendant of M. Class members are owned by the
// module that defines the class. Contents of an imported `.moon` live under a
// `$hash$` scope segment, so importer code is never a descendant of a bundle
// module and bundle-private items remain hidden through their declaration
// ownership.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sun::semantic_analysis {

enum class Visibility : uint8_t { Private = 0, Public = 1 };

/** Module path segments used for source lookup and diagnostic display. */
using ModulePath = std::vector<std::string>;

inline bool isLibraryHashSegment(const std::string& seg) {
  return seg.size() >= 2 && seg.front() == '$' && seg.back() == '$';
}

// "a.b.c" — drops `$hash$` segments; "" for the root.
inline std::string displayModulePath(const ModulePath& path) {
  std::string out;
  for (const auto& seg : path) {
    if (isLibraryHashSegment(seg)) continue;
    if (!out.empty()) out += '.';
    out += seg;
  }
  return out;
}

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

// The dotted path as source code spells it: "$hash$.std.io" reads "std.io".
// Diagnostics use this so a library's bundle hash never reaches the user.
inline std::string displayModulePath(const std::string& dotted) {
  return displayModulePath(splitModulePath(dotted));
}

inline const char* visibilityKeyword(Visibility v) {
  return v == Visibility::Public ? "public" : "private";
}

}  // namespace sun::semantic_analysis
