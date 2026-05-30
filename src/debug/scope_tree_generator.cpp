// scope_tree_generator.cpp — Generate interactive HTML visualization of scope
// tree

#include "debug/scope_tree_generator.h"

#include <sstream>

std::string ScopeTreeGenerator::generateHtml(const SemanticScope& root) {
  std::string json = generateJson(root);
  std::string html = getHtmlTemplate();

  // Replace placeholder with actual JSON data
  const std::string placeholder = "/* SCOPE_JSON_DATA */";
  size_t pos = html.find(placeholder);
  if (pos != std::string::npos) {
    html.replace(pos, placeholder.length(), json);
  }

  return html;
}

std::string ScopeTreeGenerator::scopeTypeToString(ScopeType type) {
  switch (type) {
    case ScopeType::Global:
      return "Global";
    case ScopeType::Module:
      return "Module";
    case ScopeType::Import:
      return "Import";
    case ScopeType::Class:
      return "Class";
    case ScopeType::Interface:
      return "Interface";
    case ScopeType::Function:
      return "Function";
    case ScopeType::Block:
      return "Block";
    default:
      return "Unknown";
  }
}

std::string ScopeTreeGenerator::escapeJson(const std::string& s) {
  std::string result;
  result.reserve(s.size() + 10);
  for (char c : s) {
    switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 32) {
          // Control character - escape as unicode
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

std::string ScopeTreeGenerator::generateJson(const SemanticScope& scope,
                                             int indent) {
  std::ostringstream out;
  std::string pad(indent, ' ');
  std::string pad2(indent + 2, ' ');
  std::string pad4(indent + 4, ' ');

  out << "{\n";

  // Scope type
  out << pad2 << "\"type\": \"" << scopeTypeToString(scope.type) << "\"";

  // Compute headerTitle and headerSignature for display
  std::string headerTitle;
  std::string headerSignature;

  switch (scope.type) {
    case ScopeType::Function: {
      // Function: show base name, signature is the full mangled signature
      headerTitle = scope.functionName.baseName;
      headerSignature = scope.functionSignature;
      // Fallback: extract base name from signature if functionName is empty
      // This handles scopes loaded from .moon files where functionName isn't
      // set
      if (headerTitle.empty() && !headerSignature.empty()) {
        // Signature format: "name(params)" or "$hash$_mod_Class_name(params)"
        auto parenPos = headerSignature.find('(');
        if (parenPos != std::string::npos) {
          std::string namePart = headerSignature.substr(0, parenPos);
          // Find last underscore to get the actual function name
          auto lastUnderscore = namePart.rfind('_');
          if (lastUnderscore != std::string::npos &&
              lastUnderscore < namePart.size() - 1) {
            headerTitle = namePart.substr(lastUnderscore + 1);
          } else {
            headerTitle = namePart;
          }
        }
      }
      break;
    }
    case ScopeType::Module:
      // Module: show module name, signature is full path if different
      headerTitle = scope.moduleName;
      if (!scope.modulePath.empty() && scope.modulePath != scope.moduleName) {
        headerSignature = scope.modulePath;
      }
      break;
    case ScopeType::Import:
      // Import: show the scope key
      headerTitle = scope.moduleName;
      break;
    case ScopeType::Class: {
      // Class: try to extract class name from first method signature
      headerTitle = scope.moduleName;
      if (headerTitle.empty() && !scope.children.empty()) {
        // Look for a Function child with a mangled signature
        for (const auto& child : scope.children) {
          if (child->type == ScopeType::Function ||
              child->type == ScopeType::Block) {
            // Check recursively for a Function scope
            std::function<std::string(const SemanticScope&)> findMethodSig =
                [&](const SemanticScope& s) -> std::string {
              if (s.type == ScopeType::Function &&
                  !s.functionSignature.empty()) {
                return s.functionSignature;
              }
              for (const auto& c : s.children) {
                auto sig = findMethodSig(*c);
                if (!sig.empty()) return sig;
              }
              return "";
            };
            std::string sig = findMethodSig(*child);
            if (!sig.empty()) {
              // Extract class name from "$hash$_mod_ClassName_method(params)"
              // Pattern: everything between last "$_" prefix and "_methodName"
              auto parenPos = sig.find('(');
              if (parenPos != std::string::npos) {
                std::string namePart = sig.substr(0, parenPos);
                // Find "$...$_" prefix end
                auto dollarEnd = namePart.find("$_");
                if (dollarEnd != std::string::npos) {
                  namePart = namePart.substr(dollarEnd + 2);
                }
                // Remove module prefix (e.g., "sun_")
                auto firstUnderscore = namePart.find('_');
                if (firstUnderscore != std::string::npos) {
                  namePart = namePart.substr(firstUnderscore + 1);
                }
                // Now namePart is like "MatrixView_u8_init"
                // Remove method name (last part after final underscore)
                auto lastUnderscore = namePart.rfind('_');
                if (lastUnderscore != std::string::npos) {
                  headerTitle = namePart.substr(0, lastUnderscore);
                  // Replace underscores with angle brackets for generic types
                  // e.g., "MatrixView_u8" -> "MatrixView<u8>"
                  auto typeUnderscore = headerTitle.find('_');
                  if (typeUnderscore != std::string::npos) {
                    headerTitle = headerTitle.substr(0, typeUnderscore) + "<" +
                                  headerTitle.substr(typeUnderscore + 1) + ">";
                  }
                }
              }
              break;
            }
          }
        }
      }
      break;
    } break;
    default:
      // Global, Block, etc.
      headerTitle = scope.moduleName;
      break;
  }

  if (!headerTitle.empty()) {
    out << ",\n"
        << pad2 << "\"headerTitle\": \"" << escapeJson(headerTitle) << "\"";
  }
  if (!headerSignature.empty()) {
    out << ",\n"
        << pad2 << "\"headerSignature\": \"" << escapeJson(headerSignature)
        << "\"";
  }

  // Function can throw
  if (scope.functionCanThrow) {
    out << ",\n" << pad2 << "\"canThrow\": true";
  }

  // Unsafe context
  if (scope.inUnsafeContext) {
    out << ",\n" << pad2 << "\"unsafe\": true";
  }

  // External flag
  if (scope.isExternal) {
    out << ",\n" << pad2 << "\"external\": true";
  }

  // Variables
  if (!scope.variables.empty()) {
    out << ",\n" << pad2 << "\"variables\": {";
    bool first = true;
    for (const auto& [name, info] : scope.variables) {
      if (!first) out << ",";
      out << "\n"
          << pad4 << "\"" << escapeJson(name) << "\": {"
          << "\"type\": \""
          << (info.type ? escapeJson(info.type->toString()) : "unknown") << "\""
          << ", \"isGlobal\": " << (info.isGlobal ? "true" : "false")
          << ", \"isParam\": " << (info.isFunctionParam ? "true" : "false")
          << "}";
      first = false;
    }
    out << "\n" << pad2 << "}";
  }

  // Namespaced variables
  if (!scope.namespacedVariables.empty()) {
    out << ",\n" << pad2 << "\"namespacedVariables\": {";
    bool first = true;
    for (const auto& [name, info] : scope.namespacedVariables) {
      if (!first) out << ",";
      out << "\n"
          << pad4 << "\"" << escapeJson(name) << "\": {"
          << "\"type\": \""
          << (info.type ? escapeJson(info.type->toString()) : "unknown") << "\""
          << "}";
      first = false;
    }
    out << "\n" << pad2 << "}";
  }

  // Note: Functions, classes, interfaces, enums are not listed here
  // as they appear as child scope nodes in the tree. The info is shown
  // on the scope node header itself.

  // Type parameters
  if (!scope.typeParameters.empty()) {
    out << ",\n" << pad2 << "\"typeParameters\": {";
    bool first = true;
    for (const auto& [name, type] : scope.typeParameters) {
      if (!first) out << ",";
      out << "\n"
          << pad4 << "\"" << escapeJson(name) << "\": \""
          << (type ? escapeJson(type->toString()) : "?") << "\"";
      first = false;
    }
    out << "\n" << pad2 << "}";
  }

  // Type aliases
  if (!scope.typeAliases.empty()) {
    out << ",\n" << pad2 << "\"typeAliases\": {";
    bool first = true;
    for (const auto& [name, type] : scope.typeAliases) {
      if (!first) out << ",";
      out << "\n"
          << pad4 << "\"" << escapeJson(name) << "\": \""
          << (type ? escapeJson(type->toString()) : "?") << "\"";
      first = false;
    }
    out << "\n" << pad2 << "}";
  }

  // Child module scopes
  if (!scope.childModules.empty()) {
    out << ",\n" << pad2 << "\"childModules\": {";
    bool first = true;
    for (const auto& [name, child] : scope.childModules) {
      if (!first) out << ",";
      out << "\n" << pad4 << "\"" << escapeJson(name) << "\": ";
      out << generateJson(*child, indent + 4);
      first = false;
    }
    out << "\n" << pad2 << "}";
  }

  // Non-module children (block/function scopes)
  if (!scope.children.empty()) {
    out << ",\n" << pad2 << "\"children\": [";
    bool first = true;
    for (const auto& child : scope.children) {
      if (!first) out << ",";
      out << "\n" << pad4;
      out << generateJson(*child, indent + 4);
      first = false;
    }
    out << "\n" << pad2 << "]";
  }

  out << "\n" << pad << "}";
  return out.str();
}

std::string ScopeTreeGenerator::getHtmlTemplate() {
  return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Semantic Scope Tree</title>
  <style>
    :root {
      --bg-color: #1e1e1e;
      --text-color: #d4d4d4;
      --border-color: #444;
      --scope-global: #264f36;
      --scope-module: #3d3d6b;
      --scope-import: #4a4a4a;
      --scope-function: #6b3d3d;
      --scope-class: #6b5a3d;
      --scope-block: #3d5a6b;
    }
    * { box-sizing: border-box; }
    body {
      font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
      background: var(--bg-color);
      color: var(--text-color);
      margin: 0;
      padding: 20px;
      line-height: 1.4;
    }
    h1 {
      margin: 0 0 20px 0;
      font-size: 1.5em;
      color: #569cd6;
    }
    .scope {
      margin-left: 20px;
      border-left: 2px solid var(--border-color);
      padding-left: 12px;
      margin-top: 4px;
    }
    .scope-header {
      cursor: pointer;
      padding: 6px 12px;
      border-radius: 4px;
      margin: 2px 0;
      display: inline-flex;
      align-items: center;
      gap: 8px;
      user-select: none;
      transition: filter 0.15s;
    }
    .scope-header:hover { filter: brightness(1.2); }
    .scope-header::before {
      content: '▶';
      font-size: 10px;
      transition: transform 0.15s;
    }
    .scope.expanded > .scope-header::before { transform: rotate(90deg); }
    .scope-global .scope-header { background: var(--scope-global); }
    .scope-module .scope-header { background: var(--scope-module); }
    .scope-import .scope-header { background: var(--scope-import); }
    .scope-function .scope-header { background: var(--scope-function); }
    .scope-class .scope-header { background: var(--scope-class); }
    .scope-block .scope-header { background: var(--scope-block); }
    .scope-content {
      display: none;
      margin-top: 8px;
    }
    .scope.expanded > .scope-content { display: block; }
    .details {
      background: rgba(0,0,0,0.3);
      border-radius: 4px;
      padding: 10px 14px;
      margin: 8px 0;
      font-size: 12px;
    }
    .details-section {
      margin-bottom: 10px;
    }
    .details-section:last-child { margin-bottom: 0; }
    .details-title {
      color: #569cd6;
      font-weight: bold;
      margin-bottom: 4px;
    }
    .detail-row {
      display: flex;
      gap: 10px;
      padding: 2px 0;
    }
    .detail-name { color: #9cdcfe; }
    .detail-type { color: #4ec9b0; }
    .detail-meta { color: #808080; font-style: italic; }
    .scope-qualified {
      color: #808080;
      font-size: 11px;
      margin-left: 8px;
    }
    .badge {
      display: inline-block;
      padding: 1px 6px;
      border-radius: 3px;
      font-size: 10px;
      margin-left: 6px;
    }
    .badge-throws { background: #6b3d3d; }
    .badge-unsafe { background: #8b4513; }
    .badge-external { background: #4a4a6a; }
    .children-container {
      margin-top: 4px;
    }
    .empty { color: #666; font-style: italic; }
    .toggle-all {
      background: #333;
      border: 1px solid #555;
      color: #d4d4d4;
      padding: 6px 12px;
      border-radius: 4px;
      cursor: pointer;
      margin-bottom: 15px;
    }
    .toggle-all:hover { background: #444; }
  </style>
</head>
<body>
  <h1>Semantic Scope Tree</h1>
  <button class="toggle-all" onclick="toggleAll()">Expand All</button>
  <div id="tree"></div>

  <script>
    const scopeData = /* SCOPE_JSON_DATA */;

    function renderScope(scope, container, depth = 0) {
      const div = document.createElement('div');
      div.className = `scope scope-${scope.type.toLowerCase()}`;
      if (depth === 0) div.classList.add('expanded');

      // Header
      const header = document.createElement('div');
      header.className = 'scope-header';
      
      // Build title from pre-computed fields
      let title = scope.type;
      if (scope.headerTitle) {
        title += `: ${escapeHtml(scope.headerTitle)}`;
      }
      if (scope.headerSignature) {
        title += `<span class="scope-qualified">${escapeHtml(scope.headerSignature)}</span>`;
      }
      header.innerHTML = title;

      // Badges
      if (scope.canThrow) header.innerHTML += '<span class="badge badge-throws">throws</span>';
      if (scope.unsafe) header.innerHTML += '<span class="badge badge-unsafe">unsafe</span>';
      if (scope.external) header.innerHTML += '<span class="badge badge-external">external</span>';

      header.onclick = (e) => {
        e.stopPropagation();
        div.classList.toggle('expanded');
      };

      const content = document.createElement('div');
      content.className = 'scope-content';

      // Details panel
      const details = document.createElement('div');
      details.className = 'details';
      details.innerHTML = renderDetails(scope);
      if (details.innerHTML.trim()) content.appendChild(details);

      // Children container
      const childrenContainer = document.createElement('div');
      childrenContainer.className = 'children-container';

      // Render child modules
      if (scope.childModules) {
        for (const [name, child] of Object.entries(scope.childModules)) {
          renderScope(child, childrenContainer, depth + 1);
        }
      }

      // Render other children
      if (scope.children) {
        for (const child of scope.children) {
          renderScope(child, childrenContainer, depth + 1);
        }
      }

      if (childrenContainer.children.length > 0) {
        content.appendChild(childrenContainer);
      }

      div.appendChild(header);
      div.appendChild(content);
      container.appendChild(div);
    }

    function renderDetails(scope) {
      let html = '';

      // Variables
      if (scope.variables && Object.keys(scope.variables).length > 0) {
        html += '<div class="details-section"><div class="details-title">Variables</div>';
        for (const [name, info] of Object.entries(scope.variables)) {
          const meta = [];
          if (info.isGlobal) meta.push('global');
          if (info.isParam) meta.push('param');
          html += `<div class="detail-row">
            <span class="detail-name">${escapeHtml(name)}</span>
            <span class="detail-type">${escapeHtml(info.type)}</span>
            ${meta.length ? `<span class="detail-meta">(${meta.join(', ')})</span>` : ''}
          </div>`;
        }
        html += '</div>';
      }

      // Namespaced variables
      if (scope.namespacedVariables && Object.keys(scope.namespacedVariables).length > 0) {
        html += '<div class="details-section"><div class="details-title">Namespaced Variables</div>';
        for (const [name, info] of Object.entries(scope.namespacedVariables)) {
          html += `<div class="detail-row">
            <span class="detail-name">${escapeHtml(name)}</span>
            <span class="detail-type">${escapeHtml(info.type)}</span>
          </div>`;
        }
        html += '</div>';
      }

      // Type parameters
      if (scope.typeParameters && Object.keys(scope.typeParameters).length > 0) {
        html += '<div class="details-section"><div class="details-title">Type Parameters</div>';
        for (const [name, type] of Object.entries(scope.typeParameters)) {
          html += `<div class="detail-row">
            <span class="detail-name">${escapeHtml(name)}</span>
            <span class="detail-type">= ${escapeHtml(type)}</span>
          </div>`;
        }
        html += '</div>';
      }

      // Type aliases
      if (scope.typeAliases && Object.keys(scope.typeAliases).length > 0) {
        html += '<div class="details-section"><div class="details-title">Type Aliases</div>';
        for (const [name, type] of Object.entries(scope.typeAliases)) {
          html += `<div class="detail-row">
            <span class="detail-name">${escapeHtml(name)}</span>
            <span class="detail-type">= ${escapeHtml(type)}</span>
          </div>`;
        }
        html += '</div>';
      }

      return html;
    }

    function escapeHtml(str) {
      if (!str) return '';
      return String(str)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
    }

    let allExpanded = false;
    function toggleAll() {
      allExpanded = !allExpanded;
      const scopes = document.querySelectorAll('.scope');
      scopes.forEach(s => {
        if (allExpanded) s.classList.add('expanded');
        else s.classList.remove('expanded');
      });
      // Keep root expanded
      const root = document.querySelector('#tree > .scope');
      if (root) root.classList.add('expanded');
      document.querySelector('.toggle-all').textContent = allExpanded ? 'Collapse All' : 'Expand All';
    }

    // Render the tree
    renderScope(scopeData, document.getElementById('tree'));
  </script>
</body>
</html>
)HTML";
}
