// doc_comments.h — Comments that document declarations

#pragma once

#include <string>

class BlockExprAST;

namespace sun {

// The comment block written directly above a line (1-based): consecutive
// `//` lines or one `/* */` block, with the delimiters removed. A blank line
// between the comment and the line breaks the attachment.
std::string docCommentAbove(const std::string& source, int line);

// Store the doc comment of every declaration in the program on its node —
// functions, classes and their fields and methods, interfaces, enums and
// their variants, module-level variables — so it travels with the tree into
// .moon bundles and reaches editor tooling without the source at hand.
void attachDocComments(BlockExprAST& program, const std::string& source);

}  // namespace sun
