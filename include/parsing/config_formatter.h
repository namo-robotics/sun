#pragma once

#include <string>

/** Formats configuration text without resolving project settings. */
namespace sun::parsing {
/** Returns canonical JSON with schema fields ordered and arrays preserved.
 * Throws SunError for malformed JSON, duplicate keys, or a non-object root. */
std::string formatConfig(const std::string& source);
}  // namespace sun::parsing
