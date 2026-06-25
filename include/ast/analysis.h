// analysis.h — Analysis structures populated by semantic analyzer

#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast_fwd.h"
#include "qualified_name.h"
#include "types.h"

// ============================================================================
// Analysis structures - populated by semantic analyzer, borrow checker, etc.
// These encapsulate all metadata added to AST nodes during analysis passes.
// ============================================================================

/// Base analysis data for all expression nodes
struct ExprAnalysis {
  sun::TypePtr resolvedType;  // Type determined by semantic analyzer
  bool moved = false;         // Set by borrow checker when ownership transfers

  ExprAnalysis() = default;
  ExprAnalysis(const ExprAnalysis&) = default;
  ExprAnalysis& operator=(const ExprAnalysis&) = default;
  ExprAnalysis(ExprAnalysis&&) = default;
  ExprAnalysis& operator=(ExprAnalysis&&) = default;
};

/// Analysis data for nodes with qualified names (variables, classes, etc.)
struct QualifiedNameAnalysis {
  sun::QualifiedName qualifiedName;

  QualifiedNameAnalysis() = default;
  QualifiedNameAnalysis(const QualifiedNameAnalysis&) = default;
  QualifiedNameAnalysis& operator=(const QualifiedNameAnalysis&) = default;
};

/// Analysis data for PrototypeAST (function signatures)
struct PrototypeAnalysis {
  sun::QualifiedName qualifiedName;
  std::vector<sun::TypePtr> resolvedParamTypes;
  bool resolvedParamTypesSet = false;
  sun::TypePtr resolvedReturnType;
  std::vector<sun::TypePtr> resolvedVariadicTypes;
  std::vector<std::pair<std::string, sun::TypePtr>> typeBindings;

  PrototypeAnalysis() = default;
  PrototypeAnalysis(const PrototypeAnalysis&) = default;
  PrototypeAnalysis& operator=(const PrototypeAnalysis&) = default;
};

/// Analysis data for FunctionAST (includes specializations)
struct FunctionAnalysis : public ExprAnalysis {
  std::map<std::string, std::shared_ptr<FunctionAST>> specializations;

  FunctionAnalysis() = default;
  FunctionAnalysis(const FunctionAnalysis&) = default;
  FunctionAnalysis& operator=(const FunctionAnalysis&) = default;
};

/// Analysis data for ClassDefinitionAST
struct ClassAnalysis : public ExprAnalysis {
  sun::QualifiedName qualifiedName;
  std::map<std::string, std::shared_ptr<ClassDefinitionAST>> specializations;

  ClassAnalysis() = default;
  ClassAnalysis(const ClassAnalysis&) = default;
  ClassAnalysis& operator=(const ClassAnalysis&) = default;
};

/// Analysis data for InterfaceDefinitionAST
struct InterfaceAnalysis : public ExprAnalysis {
  sun::QualifiedName qualifiedName;

  InterfaceAnalysis() = default;
  InterfaceAnalysis(const InterfaceAnalysis&) = default;
  InterfaceAnalysis& operator=(const InterfaceAnalysis&) = default;
};

/// Analysis data for ForInExprAST
struct ForInAnalysis : public ExprAnalysis {
  sun::TypePtr resolvedLoopVarType;

  ForInAnalysis() = default;
  ForInAnalysis(const ForInAnalysis&) = default;
  ForInAnalysis& operator=(const ForInAnalysis&) = default;
};

/// Analysis data for QualifiedNameAST
struct QualifiedNameExprAnalysis : public ExprAnalysis {
  std::string resolvedMangledName;

  QualifiedNameExprAnalysis() = default;
  QualifiedNameExprAnalysis(const QualifiedNameExprAnalysis&) = default;
  QualifiedNameExprAnalysis& operator=(const QualifiedNameExprAnalysis&) =
      default;
};

/// Analysis data for MemberAccessAST
struct MemberAccessAnalysis : public ExprAnalysis {
  std::vector<sun::TypePtr> resolvedTypeArgs;
  // For a generic method call whose param is a variadic pack (_init_args<T>),
  // the resolved types of the actual variadic arguments. Used to key the
  // specialization (mangled name) so different call arities/types get distinct
  // specializations.
  std::vector<sun::TypePtr> resolvedVariadicArgTypes;
  std::string resolvedQualifiedName;

  MemberAccessAnalysis() = default;
  MemberAccessAnalysis(const MemberAccessAnalysis&) = default;
  MemberAccessAnalysis& operator=(const MemberAccessAnalysis&) = default;
};

/// Analysis data for GenericCallAST
struct GenericCallAnalysis : public ExprAnalysis {
  std::vector<sun::TypePtr> resolvedTypeArgs;
  const FunctionAST* genericFunctionAST = nullptr;

  GenericCallAnalysis() = default;
  GenericCallAnalysis(const GenericCallAnalysis&) = default;
  GenericCallAnalysis& operator=(const GenericCallAnalysis&) = default;
};

/// Analysis data for DeclareTypeAST
struct DeclareTypeAnalysis : public ExprAnalysis {
  sun::TypePtr resolvedDeclaredType;

  DeclareTypeAnalysis() = default;
  DeclareTypeAnalysis(const DeclareTypeAnalysis&) = default;
  DeclareTypeAnalysis& operator=(const DeclareTypeAnalysis&) = default;
};

/// Analysis data for variable nodes (VarRef, VarCreate, RefCreate)
struct VariableAnalysis : public ExprAnalysis {
  sun::QualifiedName qualifiedName;

  VariableAnalysis() = default;
  VariableAnalysis(const VariableAnalysis&) = default;
  VariableAnalysis& operator=(const VariableAnalysis&) = default;
};
