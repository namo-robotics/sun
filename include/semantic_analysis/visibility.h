// visibility.h — Access-modifier model shared by the parser, semantic analyzer,
// moon builder and tooling.
//
// Sun is private by default: `public` is the only modifier. Privacy is
// module-scoped: an item owned by module M is reachable from code whose
// enclosing module is M or a descendant of M. Class members are owned by the
// module that defines the class. Contents of an imported `.moon` live under a
// `$hash$` scope segment, so importer code is never a descendant of a bundle
// module and bundle-private items are hidden by the same prefix rule.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sun {

enum class Visibility : uint8_t { Private = 0, Public = 1 };

// Module path as segments, e.g. {"$hash$", "std"}; root = {}. An item's owner
// is its QualifiedName::owner() (the scope path it was declared in); a
// private item owned by root is reachable from every context in a
// compilation — root is a prefix of every path — so internal/synthesized
// symbols fail open, while anything declared under a real module or a bundle
// fails closed unless marked public.
using ModulePath = std::vector<std::string>;

inline bool isModulePrefix(const ModulePath& prefix, const ModulePath& path) {
  if (prefix.size() > path.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (prefix[i] != path[i]) return false;
  }
  return true;
}

// The single access predicate: public, or `from` is inside the owner.
inline bool isAccessibleFrom(const ModulePath& from, Visibility visibility,
                             const ModulePath& owner) {
  return visibility == Visibility::Public || isModulePrefix(owner, from);
}

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

inline const char* visibilityKeyword(Visibility v) {
  return v == Visibility::Public ? "public" : "private";
}

}  // namespace sun
