#include "abi/boundary_requirements.h"

#include <cstdint>
#include <string_view>

#include "support/error.h"

/** Validates the emitted boundaries retained in bundle contracts. */
namespace sun::abi {
/** Decodes bounded canonical fields without allocating from supplied lengths.
 */
namespace {
/** Holds a borrowed canonical field after its frame has been checked. */
struct Field {
  uint64_t tag;
  std::string_view value;
};

/** Rejects unsupported or malformed required boundary descriptions. */
[[noreturn]] void malformed() {
  sun::support::logAndThrowError(
      "Malformed or unsupported bundle boundary record");
}

/** Consumes one fixed-width little-endian integer from a bounded view. */
uint64_t integer(std::string_view& bytes) {
  if (bytes.size() < 8) malformed();
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= uint64_t(static_cast<unsigned char>(bytes[i])) << (i * 8);
  bytes.remove_prefix(8);
  return value;
}

/** Consumes a complete tagged field, checking lengths before slicing. */
Field field(std::string_view& bytes) {
  const auto tag = integer(bytes);
  const auto length = integer(bytes);
  if (length > bytes.size()) malformed();
  const auto value = bytes.substr(0, static_cast<size_t>(length));
  bytes.remove_prefix(static_cast<size_t>(length));
  return {tag, value};
}

/** Requires the next ordered field to have the specified tag. */
std::string_view required(std::string_view& bytes, uint64_t tag) {
  const auto next = field(bytes);
  if (next.tag != tag) malformed();
  return next.value;
}

/** Requires an integer-valued field to have exactly the fixed canonical width.
 */
uint64_t number(std::string_view& bytes, uint64_t tag) {
  auto value = required(bytes, tag);
  const auto result = integer(value);
  if (!value.empty()) malformed();
  return result;
}

/** Requires a named identity or printed LLVM type to be nonempty. */
std::string_view text(std::string_view& bytes, uint64_t tag) {
  const auto value = required(bytes, tag);
  if (value.empty()) malformed();
  return value;
}

/** Checks framing for the ordered attribute entries emitted by LLVM lowering.
 */
void attributes(std::string_view bytes) {
  while (!bytes.empty()) {
    const auto attribute = field(bytes);
    if (attribute.value.empty()) malformed();
  }
}
}  // namespace

void BoundaryRequirements::add(const std::string& record) {
  std::string_view bytes(record);
  const auto outer = field(bytes);
  if (!bytes.empty()) malformed();
  bytes = outer.value;
  std::string key;
  std::string name;
  if (outer.tag == 1) {
    name = text(bytes, 1);
    key = "layout:" + name;
    const auto size = number(bytes, 2);
    const auto alignment = number(bytes, 3);
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) malformed();
    uint64_t previousOffset = 0;
    while (!bytes.empty()) {
      auto member = required(bytes, 4);
      const auto offset = integer(member);
      if (offset < previousOffset || offset > size) malformed();
      previousOffset = offset;
      text(member, 1);
      if (!member.empty()) malformed();
    }
  } else if (outer.tag == 2) {
    name = text(bytes, 1);
    key = "symbol:" + name;
    text(bytes, 2);
    number(bytes, 3);
    attributes(required(bytes, 4));
  } else if (outer.tag == 3) {
    text(bytes, 1);
    number(bytes, 2);
    attributes(required(bytes, 3));
  } else if (outer.tag == 4) {
    name = text(bytes, 1);
    // LLVM combines these lists across modules; different lengths can coexist.
    // Their exact interpretation remains covered by the experimental LLVM rule.
    if (name != "llvm.global_ctors" && name != "llvm.global_dtors" &&
        name != "llvm.used" && name != "llvm.compiler.used")
      key = "symbol:" + name;
    text(bytes, 2);
    const auto alignment = number(bytes, 3);
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) malformed();
    number(bytes, 4);
  } else {
    malformed();
  }
  if (!bytes.empty()) malformed();
  if (!key.empty()) {
    const auto found = named_.find(key);
    if (found != named_.end() && found->second != record)
      sun::support::logAndThrowError("Conflicting bundle boundary for '" +
                                     name + "'");
    named_.emplace(std::move(key), record);
  }
  records_.insert(record);
}
}  // namespace sun::abi
