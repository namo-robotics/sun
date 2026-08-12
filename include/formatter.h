// formatter.h — Canonical source formatter (sun fmt)

#pragma once

#include <map>
#include <string>

#include "parser.h"

namespace sun {

// Parse source and reprint it in the canonical style (2-space indent,
// K&R braces, comments preserved). Throws SunError on parse failure;
// the input is never partially formatted.
std::string formatSource(const std::string& source,
                         const std::string& filePath = "<fmt>");

// Format an already-parsed lossless (pre-lowering) program. `comments` must
// come from the same parse (Parser::getComments()) and `source` must be the
// exact text that was parsed — literals and types are sliced from it.
std::string formatProgram(const BlockExprAST& program,
                          const std::map<int, Comment>& comments,
                          const std::string& source);

}  // namespace sun
