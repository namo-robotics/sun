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

// Module path as segments, e.g. {"$hash$", "sun"}; root = {}. This is the same
// representation as QualifiedName::scopePath — for symbols that carry a
// QualifiedName, `AccessInfo::owner == qualifiedName.scopePath`. It is stored
// separately only so records without a QualifiedName (class/interface
// members, interface and enum types, inherited interface fields that keep the
// interface's owner) fit the same predicate.
using ModulePath = std::vector<std::string>;

// Visibility plus the module that owns the item. The default (Private, root)
// is reachable from every context in a compilation — root is a prefix of every
// path — so internal/synthesized symbols fail open, while anything registered
// under a real module or a bundle fails closed unless marked public.
struct AccessInfo {
  Visibility visibility = Visibility::Private;
  ModulePath owner;

  bool isPublic() const { return visibility == Visibility::Public; }
};

inline bool isModulePrefix(const ModulePath& prefix, const ModulePath& path) {
  if (prefix.size() > path.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (prefix[i] != path[i]) return false;
  }
  return true;
}

// The single access predicate: public, or `from` is inside the owner.
inline bool isAccessibleFrom(const ModulePath& from, const AccessInfo& item) {
  return item.isPublic() || isModulePrefix(item.owner, from);
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
