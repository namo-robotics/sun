// Declaration registration and validation within a semantic scope.

#include "semantic_analysis/semantic_scope.h"
#include "semantic_analysis/symbol_names.h"
#include "support/error.h"

using sun::semantic_analysis::QualifiedName;
using sun::semantic_analysis::TypePtr;

using sun::support::logAndThrowError;
using sun::support::Position;

namespace sun::semantic_analysis {

void SemanticScopeBase::declareVariable(
    const std::string& name, TypePtr type, bool isParam, bool isConst,
    sun::semantic_analysis::DeclarationId declarationId) {
  // Block user-defined identifiers starting with underscore
  if (sun::semantic_analysis::isReservedIdentifier(name)) {
    logAndThrowError(
        "Identifier '" + name +
        "' is invalid: names starting with '_' are reserved for builtins");
  }
  // Check for shadowing of global/module variables
  for (auto* s = this; s != nullptr; s = s->parent) {
    if (s->getType() == ScopeType::Global ||
        s->getType() == ScopeType::Module) {
      if (s->variables.contains(name)) {
        logAndThrowError("Cannot shadow " +
                         std::string(s->getType() == ScopeType::Global
                                         ? "global"
                                         : "module") +
                         " variable '" + name + "'");
      }
    }
  }
  VariableInfo info{type, isAtModuleLevel(), isParam, false};
  info.declarationId = declarationId;
  info.isConst = isConst;
  variables[name] = info;
}

ModuleScope& SemanticScopeBase::declareModule(const std::string& name) {
  auto& child = childModules[name];
  if (!child) {
    auto module = std::make_shared<ModuleScope>();
    module->scopeName = name;
    module->parent = this;
    module->scopePath = scopePath;
    module->scopePath.push_back(name);
    module->qualifiedName = QualifiedName(scopePath, name);
    child = module;
  }
  auto* root = this;
  while (root->parent) root = root->parent;
  root->canonicalModules[QualifiedName(child->scopePath, "")
                             .scopePathString()] = child.get();
  return static_cast<ModuleScope&>(*child);
}

ModuleScope& SemanticScopeBase::declareModule(
    const sun::ast::ModuleAST& declaration) {
  auto& module = declareModule(declaration.getName());
  module.declarationId = declaration.getDeclarationId();
  if (module.visibilityDeclared &&
      module.visibility != declaration.getVisibility())
    sun::support::logSemanticError(
        "module '" + declaration.getName() + "' was previously declared " +
            sun::semantic_analysis::visibilityKeyword(module.visibility) +
            "; all declarations of a module must agree on its visibility",
        declaration.getLocation());
  if (declaration.hasQualifiedName()) {
    module.qualifiedName = declaration.getQualifiedName();
    module.scopePath = module.qualifiedName.scopePath;
    module.scopePath.push_back(module.qualifiedName.baseName);
  }
  auto* root = this;
  while (root->parent) root = root->parent;
  root->canonicalModules[QualifiedName(module.scopePath, "")
                             .scopePathString()] = &module;
  module.visibility = declaration.getVisibility();
  module.visibilityDeclared = true;
  return module;
}

void SemanticScopeBase::declareClassDefinition(
    const std::string& name, sun::ast::ClassDefinitionAST& definition) {
  classDefinitions[name] = &definition;
}

void SemanticScopeBase::declareTypeAlias(const std::string& name, TypePtr type,
                                         std::optional<Position> loc) {
  if (typeAliases.contains(name))
    logAndThrowError(
        "Type alias '" + name + "' is already defined in this scope", loc);
  if (type) typeAliases[name] = std::move(type);
}

void SemanticScopeBase::declareFunction(const std::string& name,
                                        const FunctionInfo& info,
                                        std::optional<Position> loc) {
  // Functions are registered in their enclosing scope. For nested functions,
  // this is the parent function's scope - the scope hierarchy naturally
  // disambiguates between different generic instantiations.
  sun::semantic_analysis::CallableSignature sig{name, info.paramTypes};
  auto existing = functions.find(sig);
  if (existing != functions.end() && info.isForwardDeclaration &&
      !existing->second.isForwardDeclaration)
    return;
  if (existing != functions.end() && info.declarationId &&
      existing->second.declarationId &&
      existing->second.declarationId != info.declarationId &&
      !info.isForwardDeclaration && !existing->second.isForwardDeclaration &&
      !(info.isCExtern && existing->second.isCExtern))
    logAndThrowError("Function '" + name + "' is already defined in this scope",
                     loc);
  // Body checking replaces the collected signature with complete information.
  functions[sig] = info;
}

void SemanticScopeBase::declareGenericFunction(sun::ast::FunctionAST& func) {
  const sun::ast::PrototypeAST& proto = func.getProto();
  assert(proto.hasQualifiedName() && "Generic declaration must be named first");
  const QualifiedName& qname = proto.getQualifiedName();
  auto existing = genericFunctions.find(proto.getName());
  // Repeated declaration collection may visit the same template again.
  // A different declaration must not silently replace it.
  if (existing != genericFunctions.end() && existing->second.AST != &func) {
    logAndThrowError("Generic function '" + proto.getName() +
                         "' is already declared in this scope; generic "
                         "function overloads are not supported",
                     func.getLocation());
  }

  GenericFunctionInfo genInfo;
  genInfo.AST = &func;
  genInfo.typeParameters = proto.getTypeParameters();
  if (proto.hasReturnType()) {
    genInfo.returnType = *proto.getReturnType();
  }
  genInfo.params = proto.getArgs();
  genInfo.qualifiedName = qname;
  genInfo.definitionScope = shared_from_this();
  genericFunctions[proto.getName()] = genInfo;
}

void SemanticScopeBase::declareModuleVariable(
    const QualifiedName& qualifiedName, TypePtr type,
    sun::semantic_analysis::Visibility visibility, bool isConst, bool isCExtern,
    sun::semantic_analysis::DeclarationId declarationId) {
  VariableInfo info{type, true, false};
  info.declarationId = declarationId;
  info.visibility = visibility;
  info.isConst = isConst;
  info.isCExtern = isCExtern;
  info.qualifiedName = qualifiedName;
  const std::string& baseName = qualifiedName.baseName;
  namespacedVariables[baseName] = info;
  // The plain-name entry created by declareVariable during body analysis
  if (auto it = variables.find(baseName); it != variables.end()) {
    it->second.visibility = visibility;
    it->second.isConst = isConst;
    it->second.isCExtern = isCExtern;
    if (it->second.qualifiedName.empty())
      it->second.qualifiedName = info.qualifiedName;
  }
}

void SemanticScopeBase::declareClass(
    const std::string& name,
    std::shared_ptr<sun::semantic_analysis::ClassType> classType,
    std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (classes.contains(name)) {
    return;
  }
  // Register in current scope
  classes[name] = classType;
}

void SemanticScopeBase::declareGenericClass(const std::string& name,
                                            const GenericClassInfo& info,
                                            std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (genericClasses.contains(name)) {
    return;
  }
  // Register in current scope
  auto& slot = genericClasses[name];
  slot = info;
  slot.definitionScope = shared_from_this();
}

void SemanticScopeBase::declareInterface(
    const std::string& name,
    std::shared_ptr<sun::semantic_analysis::InterfaceType> interfaceType,
    std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (interfaces.contains(name)) {
    return;
  }
  // Register in current scope
  interfaces[name] = interfaceType;
}

void SemanticScopeBase::declareGenericInterface(
    const std::string& name, const GenericInterfaceInfo& info,
    std::optional<Position> loc) {
  // Skip if already registered (diamond import re-registration)
  if (genericInterfaces.contains(name)) {
    return;
  }
  // Register in current scope
  auto& slot = genericInterfaces[name];
  slot = info;
  slot.definitionScope = shared_from_this();
}

void SemanticScopeBase::declareEnum(
    const std::string& name,
    std::shared_ptr<sun::semantic_analysis::EnumType> enumType) {
  // Register in current scope
  enums[name] = enumType;
}

void SemanticScopeBase::declareGenericEnum(const std::string& name,
                                           GenericEnumInfo info) {
  info.definitionScope = shared_from_this();
  genericEnums[name] = std::move(info);
}

void SemanticScopeBase::declareTypeParameters(
    const std::vector<std::string>& params, const std::vector<TypePtr>& args) {
  auto& scope = *this;
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    // Lifetime names are relative to the signature that wrote the type
    // argument; the specialization the binding builds is shared by every
    // caller, so the names must not leak into it
    scope.typeParameters[params[i]] =
        sun::semantic_analysis::eraseLifetimeNames(args[i]);
  }
}

}  // namespace sun::semantic_analysis
