#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "support/terminal.h"

/** Renders structured compiler diagnostics for the destination terminal. */
namespace sun::support {

/** One tree label with plain detail lines and ordered child branches.
 * Labels may contain formatting returned by terminalFileLabel. Labels and
 * details must each occupy one line; rendering colors only the connectors.
 */
struct TerminalTreeNode {
  std::string label;
  std::vector<std::string> details;
  std::vector<TerminalTreeNode> children;
};

/** Links an existing file with a solid yellow underline and unchanged text color.
 * VS Code receives a visible absolute path; other terminals receive an OSC link.
 * Missing files, filesystem errors, and disabled formatting use a plain basename.
 */
std::string terminalFileLabel(const std::filesystem::path& path,
                              llvm::raw_ostream& stream);

/** Renders a tree with solid connectors, styling them with the given severity.
 * The root has no connector. Every line receives indent; no trailing newline
 * is added. Detail lines follow their label before child branches.
 */
std::string terminalTree(const TerminalTreeNode& root, llvm::raw_ostream& stream,
                         const std::string& level = "warning",
                         const std::string& indent = "");

}  // namespace sun::support
