// symbol_location.h — A name's range in a source file, in byte offsets and
// in protocol coordinates

#pragma once

#include <string>

#include "lsp/text_positions.h"
#include "support/position.h"

namespace sun::lsp {

struct SymbolLocation {
  std::string filePath;  // Absolute path of the file holding the name
  Position range;        // Byte offsets of the name in that file
  LspPosition start;     // The same range in protocol coordinates
  LspPosition end;
};

// Protocol form of a byte range inside `text`
SymbolLocation makeSymbolLocation(const std::string& filePath,
                                  const Position& range,
                                  const std::string& text);

}  // namespace sun::lsp
