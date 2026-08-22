// doc_comments.cpp — Comments that document declarations

#include "doc_comments.h"

#include <vector>

#include "ast.h"

namespace sun {

namespace {

std::string trim(const std::string& text) {
  size_t start = text.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(start, end - start + 1);
}

bool startsWith(const std::string& text, const std::string& prefix) {
  return text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// One comment line with its delimiters removed
std::string stripCommentLine(std::string line) {
  line = trim(line);
  if (endsWith(line, "*/")) line = line.substr(0, line.size() - 2);
  if (startsWith(line, "///")) line = line.substr(3);
  else if (startsWith(line, "//")) line = line.substr(2);
  else if (startsWith(line, "/**")) line = line.substr(3);
  else if (startsWith(line, "/*")) line = line.substr(2);
  else if (startsWith(line, "*")) line = line.substr(1);
  return trim(line);
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

std::string joinCollected(const std::vector<std::string>& collected) {
  std::string out;
  for (auto it = collected.rbegin(); it != collected.rend(); ++it) {
    if (it->empty() && out.empty()) continue;  // leading blank comment line
    if (!out.empty()) out += "\n";
    out += *it;
  }
  return trim(out);
}

// Attaches docs throughout one parsed file
class DocAttacher {
 public:
  explicit DocAttacher(const std::string& source)
      : lines_(splitLines(source)) {}

  void visitBlock(BlockExprAST& block) {
    for (auto& stmt : block.mutableBody()) {
      if (stmt) visit(*stmt);
    }
  }

 private:
  std::string docAt(const Position& location) const {
    return commentAbove(lines_, location.line);
  }

  // A member written on the same line as its parent's header has no line
  // of its own above it; the comment there belongs to the parent
  std::string memberDocAt(const Position& member,
                          const Position& parent) const {
    return member.line == parent.line ? "" : docAt(member);
  }

  void visit(ExprAST& node) {
    switch (node.getType()) {
      case ASTNodeType::MODULE:
        visitBlock(const_cast<BlockExprAST&>(
            static_cast<ModuleAST&>(node).getBody()));
        break;
      case ASTNodeType::FUNCTION: {
        auto& fn = static_cast<FunctionAST&>(node);
        fn.getProtoMut().setDoc(docAt(fn.getLocation()));
        break;
      }
      case ASTNodeType::CLASS_DEFINITION: {
        auto& cls = static_cast<ClassDefinitionAST&>(node);
        cls.setDoc(docAt(cls.getLocation()));
        for (auto& field : cls.getMutableFields()) {
          field.doc = memberDocAt(field.location, cls.getLocation());
        }
        for (auto& method : cls.getMutableMethods()) {
          if (!method.function) continue;
          method.function->getProtoMut().setDoc(
              memberDocAt(method.function->getLocation(), cls.getLocation()));
        }
        break;
      }
      case ASTNodeType::INTERFACE_DEFINITION: {
        auto& iface = static_cast<InterfaceDefinitionAST&>(node);
        iface.setDoc(docAt(iface.getLocation()));
        for (auto& field : iface.getMutableFields()) {
          field.doc = memberDocAt(field.location, iface.getLocation());
        }
        for (auto& method : iface.getMutableMethods()) {
          if (!method.function) continue;
          method.function->getProtoMut().setDoc(memberDocAt(
              method.function->getLocation(), iface.getLocation()));
        }
        break;
      }
      case ASTNodeType::ENUM_DEFINITION: {
        auto& enumDef = static_cast<EnumDefinitionAST&>(node);
        enumDef.setDoc(docAt(enumDef.getLocation()));
        for (auto& variant : enumDef.getMutableVariants()) {
          variant.doc = memberDocAt(variant.location, enumDef.getLocation());
        }
        break;
      }
      case ASTNodeType::VARIABLE_CREATION: {
        auto& var = static_cast<VariableCreationAST&>(node);
        var.setDoc(docAt(var.getLocation()));
        break;
      }
      default:
        break;
    }
  }

  static std::string commentAbove(const std::vector<std::string>& lines,
                                  int line) {
    int index = line - 2;  // line above, as a 0-based index
    if (index < 0 || index >= static_cast<int>(lines.size())) return "";

    std::vector<std::string> collected;
    std::string above = trim(lines[index]);
    if (startsWith(above, "//")) {
      while (index >= 0 && startsWith(trim(lines[index]), "//")) {
        collected.push_back(stripCommentLine(lines[index]));
        --index;
      }
    } else if (endsWith(above, "*/")) {
      int start = index;
      while (start >= 0 &&
             trim(lines[start]).find("/*") == std::string::npos) {
        --start;
      }
      if (start < 0) return "";
      for (int i = index; i >= start; --i) {
        collected.push_back(stripCommentLine(lines[i]));
      }
    } else {
      return "";
    }
    return joinCollected(collected);
  }

  friend std::string sun::docCommentAbove(const std::string& source, int line);

  std::vector<std::string> lines_;
};

}  // namespace

std::string docCommentAbove(const std::string& source, int line) {
  return DocAttacher::commentAbove(splitLines(source), line);
}

void attachDocComments(BlockExprAST& program, const std::string& source) {
  DocAttacher attacher(source);
  attacher.visitBlock(program);
}

}  // namespace sun
