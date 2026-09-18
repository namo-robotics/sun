// formatter.h — Canonical source formatter (sun fmt)

#pragma once

#include <map>
#include <string>

#include "parsing/parser.h"

namespace sun::parsing {

// Parse source and reprint it in the canonical style (2-space indent,
// K&R braces, comments preserved). Throws SunError on parse failure;
// the input is never partially formatted.
std::string formatSource(const std::string& source,
                         const std::string& filePath = "<fmt>");

// Format an already-parsed lossless (pre-lowering) program. `comments` must
// come from the same parse (Parser::getComments()) and `source` must be the
// exact text that was parsed — literals and types are sliced from it.
std::string formatProgram(const sun::ast::BlockExprAST& program,
                          const std::map<int, sun::parsing::Comment>& comments,
                          const std::string& source);

}  // namespace sun::parsing
