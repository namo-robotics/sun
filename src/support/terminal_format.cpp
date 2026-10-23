/** Implements reusable file links and tree layouts for terminal diagnostics. */
#include "support/terminal_format.h"

/** Renders structured compiler diagnostics for the destination terminal. */
namespace sun::support {
std::string terminalFileLabel(const std::filesystem::path& path,
                              llvm::raw_ostream& stream) {
  const auto label = path.filename().string();
  if (!terminalColors(stream)) return label;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) return label;
  const auto absolute = std::filesystem::absolute(path, error);
  if (error) return label;
  // VS Code resolves visible file paths in its editor, including remote workspaces.
  const char* terminal = std::getenv("TERM_PROGRAM");
  if (terminal && std::string_view(terminal) == "vscode")
    return "\033[4:1;58;5;3m" + absolute.lexically_normal().string() +
           "\033[24;59m";
  std::string uri = "file://";
  constexpr char hex[] = "0123456789ABCDEF";
  for (const unsigned char byte : absolute.lexically_normal().generic_string()) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '/' || byte == '-' ||
        byte == '_' || byte == '.' || byte == '~') {
      uri += static_cast<char>(byte);
    } else {
      uri += '%';
      uri += hex[byte >> 4];
      uri += hex[byte & 15];
    }
  }
  // Set underline color independently of the filename's foreground color.
  return "\033]8;;" + uri + "\033\\\033[4:1;58;5;3m" + label +
         "\033[24;59m\033]8;;\033\\";
}

/** Keeps recursive tree layout private to the renderer. */
namespace {
/** Appends a node's details and descendants, retaining ancestor branch guides. */
void appendTree(std::string& output, const TerminalTreeNode& node,
                const std::string& guides, const std::string& indent,
                const std::string& level, llvm::raw_ostream& stream) {
  for (const auto& detail : node.details)
    output += "\n" + indent + terminalStyledText(guides, level, stream) + detail;
  for (size_t i = 0; i < node.children.size(); ++i) {
    const auto& child = node.children[i];
    const bool last = i + 1 == node.children.size();
    output += "\n" + indent +
              terminalStyledText(guides + (last ? "└── " : "├── "), level, stream) +
              child.label;
    appendTree(output, child, guides + (last ? "    " : "│   "), indent,
               level, stream);
  }
}
}  // namespace

std::string terminalTree(const TerminalTreeNode& root, llvm::raw_ostream& stream,
                         const std::string& level, const std::string& indent) {
  std::string output = indent + root.label;
  appendTree(output, root, "", indent, level, stream);
  return output;
}
}  // namespace sun::support
