// symbol_location.h — A name's range in a source file, in byte offsets and
// in protocol coordinates

#pragma once

#include <string>

#include "lsp/text_positions.h"
#include "support/position.h"

/** Provides compiler-backed editor features through the language server
 * protocol. */
namespace sun::lsp {

/** A declaration location returned by editor navigation requests. */
struct SymbolLocation {
  std::string filePath;          // Absolute path of the file holding the name
  sun::support::Position range;  // Byte offsets of the name in that file
  LspPosition start;             // The same range in protocol coordinates
  LspPosition end;
};

/**
 * Protocol form of a byte range inside `text`
 */
SymbolLocation makeSymbolLocation(const std::string& filePath,
                                  const sun::support::Position& range,
                                  const std::string& text);

}  // namespace sun::lsp
