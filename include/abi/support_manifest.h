#pragma once

#include "compatibility.pb.h"

/** Validates compiler capabilities against concrete implementation bindings. */
namespace sun::abi {
/** Rejects unsupported schemas, bindings, duplicate entries, and invalid
 * defaults. */
void validateCompilerSupport(const proto::CompilerAbiSupport& support);
}  // namespace sun::abi
