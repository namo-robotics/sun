// doc_comments.h — Comments that document declarations

#pragma once

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
class BlockExprAST;
}

#include <string>

/** Turns source text into syntax trees and provides source formatting. */
namespace sun::parsing {}

/** Turns source text into syntax trees and provides source formatting. */
namespace sun::parsing {

// The comment block written directly above a line (1-based): consecutive
// `//` lines or one `/* */` block, with the delimiters removed. A blank line
// between the comment and the line breaks the attachment.
/** Returns the documentation comment preceding a source line. */
std::string docCommentAbove(const std::string& source, int line);

/**
 * Store the doc comment of every declaration in the program on its node —
 * functions, classes and their fields and methods, interfaces, enums and
 * their variants, module-level variables — so it travels with the tree into
 * .moon bundles and reaches editor tooling without the source at hand.
 */
void attachDocComments(sun::ast::BlockExprAST& program,
                       const std::string& source);

}  // namespace sun::parsing
