// analysis.h — Analysis structures populated by semantic analyzer

#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast_common.h"
#include "ast/ast_fwd.h"
#include "semantic_analysis/argument_conversion.h"
#include "semantic_analysis/declaration_id.h"
#include "semantic_analysis/qualified_name.h"
#include "semantic_analysis/types.h"

// ============================================================================
// Analysis structures - populated by semantic analyzer, borrow checker, etc.
// These encapsulate all metadata added to AST nodes during analysis passes.
// ============================================================================

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
using sun::semantic_analysis::DeclarationId;
using sun::semantic_analysis::QualifiedName;
using sun::semantic_analysis::TypePtr;

/**
 * What a write to a field does to whatever the field held before it. The
 * field-initialization walk proves which one holds for every write — there is
 * no run-time case.
 */
enum class FieldWriteKind {
  // The field holds a value: drop it before storing the new one
  ReplacesValue,
  // It cannot hold one yet — this write starts its life, so nothing is
  // dropped
  StartsLife,
};

}  // namespace sun::ast

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {

/**
 * Base analysis data for all expression nodes
 */
struct ExprAnalysis {
  /** Destroys this object and releases its owned members. */
  virtual ~ExprAnalysis() = default;
  sun::semantic_analysis::DeclarationIdentity declaration;
  DeclarationId targetDeclaration;
  TypePtr resolvedType;       // Type determined by semantic analyzer
  bool moved = false;         // Set by borrow checker when ownership transfers

  /** Creates an instance with its default state. */
  ExprAnalysis() = default;
  /** Creates an instance with its default state. */
  ExprAnalysis(const ExprAnalysis&) = default;
  /** Replaces the stored state with a copy of another instance. */
  ExprAnalysis& operator=(const ExprAnalysis&) = default;
  /** Creates an instance with its default state. */
  ExprAnalysis(ExprAnalysis&&) = default;
  /** Transfers the stored state from another instance during move assignment. */
  ExprAnalysis& operator=(ExprAnalysis&&) = default;
};

/** Selected fields in source order for a struct literal. */
struct StructLiteralAnalysis : public ExprAnalysis {
  std::vector<DeclarationId> fields;
};

/**
 * Analysis data for PrototypeAST (function signatures)
 */
struct PrototypeAnalysis {
  std::vector<Capture> captures;
  sun::semantic_analysis::DeclarationIdentity declaration;
  QualifiedName qualifiedName;
  std::vector<TypePtr> resolvedParamTypes;
  bool resolvedParamTypesSet = false;
  TypePtr resolvedReturnType;
  std::vector<TypePtr> resolvedVariadicTypes;
  // Distinguishes a specialization whose pack turned out to be empty from a
  // template whose pack is not resolved yet — both hold no types.
  bool resolvedVariadicTypesSet = false;
  std::vector<std::pair<std::string, TypePtr>> typeBindings;

  /** Creates an instance with its default state. */
  PrototypeAnalysis() = default;
  /** Creates an instance with its default state. */
  PrototypeAnalysis(const PrototypeAnalysis&) = default;
  /** Replaces the stored state with a copy of another instance. */
  PrototypeAnalysis& operator=(const PrototypeAnalysis&) = default;
};

/**
 * Analysis data for FunctionAST (includes specializations)
 */
struct FunctionAnalysis : public ExprAnalysis {
  std::map<DeclarationId, std::shared_ptr<FunctionAST>> specializations;

  /** Creates an instance with its default state. */
  FunctionAnalysis() = default;
  /** Creates an instance with its default state. */
  FunctionAnalysis(const FunctionAnalysis&) = default;
  /** Replaces the stored state with a copy of another instance. */
  FunctionAnalysis& operator=(const FunctionAnalysis&) = default;
};

/**
 * Analysis data for ClassDefinitionAST
 */
struct ClassAnalysis : public ExprAnalysis {
  QualifiedName qualifiedName;
  std::map<DeclarationId, std::shared_ptr<ClassDefinitionAST>> specializations;

  /** Creates an instance with its default state. */
  ClassAnalysis() = default;
  /** Creates an instance with its default state. */
  ClassAnalysis(const ClassAnalysis&) = default;
  /** Replaces the stored state with a copy of another instance. */
  ClassAnalysis& operator=(const ClassAnalysis&) = default;
};

/**
 * Analysis data for InterfaceDefinitionAST
 */
struct InterfaceAnalysis : public ExprAnalysis {
  QualifiedName qualifiedName;

  /** Creates an instance with its default state. */
  InterfaceAnalysis() = default;
  /** Creates an instance with its default state. */
  InterfaceAnalysis(const InterfaceAnalysis&) = default;
  /** Replaces the stored state with a copy of another instance. */
  InterfaceAnalysis& operator=(const InterfaceAnalysis&) = default;
};

/**
 * Analysis data for ForInExprAST
 */
struct ForInAnalysis : public ExprAnalysis {
  TypePtr resolvedLoopVarType;
  DeclarationId iteratorFactory;
  DeclarationId iteratorNext;
  TypePtr iteratorResultType;

  /** Creates an instance with its default state. */
  ForInAnalysis() = default;
  /** Creates an instance with its default state. */
  ForInAnalysis(const ForInAnalysis&) = default;
  /** Replaces the stored state with a copy of another instance. */
  ForInAnalysis& operator=(const ForInAnalysis&) = default;
};

/**
 * Analysis data for MemberAccessAST
 */
struct MemberAccessAnalysis : public ExprAnalysis {
  std::vector<TypePtr> resolvedTypeArgs;
  // For a generic method call whose last param is an `args...` pack,
  // the resolved types of the actual variadic arguments. Used to key the
  // specialization key so different call arities/types get distinct
  // specializations.
  std::vector<TypePtr> resolvedVariadicArgTypes;
  // The symbol this access denotes, when it denotes one: a module's function
  // or variable, or the specialization the analyzer instantiated for a
  // generic call. Declaration IDs select emitted symbols. Empty for an
  // ordinary field or method access.
  QualifiedName qualifiedName;
  // True when this member access is a method used in value position (bound
  // method reference); its resolved type is then a LambdaType.
  bool isBoundMethodRef = false;
  // For a field write: what happens to the value the field held before it.
  // Decided by checkFieldInitialization; anywhere it has not looked, a write
  // replaces a live value, which is the safe reading.
  sun::ast::FieldWriteKind fieldWrite = sun::ast::FieldWriteKind::ReplacesValue;

  /** Creates an instance with its default state. */
  MemberAccessAnalysis() = default;
  /** Creates an instance with its default state. */
  MemberAccessAnalysis(const MemberAccessAnalysis&) = default;
  /** Replaces the stored state with a copy of another instance. */
  MemberAccessAnalysis& operator=(const MemberAccessAnalysis&) = default;
};

/**
 * Analysis data for CallExprAST
 */
struct CallAnalysis : public ExprAnalysis {
  // How each argument reaches its parameter, decided by the semantic analyzer
  // once the callee's signature is known; codegen carries these out.
  std::vector<sun::semantic_analysis::ArgConversion> argConversions;

  /** Creates an instance with its default state. */
  CallAnalysis() = default;
  /** Creates an instance with its default state. */
  CallAnalysis(const CallAnalysis&) = default;
  /** Replaces the stored state with a copy of another instance. */
  CallAnalysis& operator=(const CallAnalysis&) = default;
};

/**
 * Analysis data for GenericCallAST
 */
struct GenericCallAnalysis : public ExprAnalysis {
  std::vector<TypePtr> resolvedTypeArgs;
  const FunctionAST* genericFunctionAST = nullptr;
  // Concrete callable signature selected for this generic call.
  TypePtr resolvedCalleeType;
  // As CallAnalysis::argConversions, for `f<T>(args)` and `Box<T>(args)`
  std::vector<sun::semantic_analysis::ArgConversion> argConversions;

  /** Creates an instance with its default state. */
  GenericCallAnalysis() = default;
  /** Creates an instance with its default state. */
  GenericCallAnalysis(const GenericCallAnalysis&) = default;
  /** Replaces the stored state with a copy of another instance. */
  GenericCallAnalysis& operator=(const GenericCallAnalysis&) = default;
};

/**
 * Analysis data for DeclareTypeAST
 */
struct DeclareTypeAnalysis : public ExprAnalysis {
  TypePtr resolvedDeclaredType;

  /** Creates an instance with its default state. */
  DeclareTypeAnalysis() = default;
  /** Creates an instance with its default state. */
  DeclareTypeAnalysis(const DeclareTypeAnalysis&) = default;
  /** Replaces the stored state with a copy of another instance. */
  DeclareTypeAnalysis& operator=(const DeclareTypeAnalysis&) = default;
};

/**
 * Analysis data for variable nodes (VarRef, VarCreate, RefCreate)
 */
struct VariableAnalysis : public ExprAnalysis {
  QualifiedName qualifiedName;

  /** Creates an instance with its default state. */
  VariableAnalysis() = default;
  /** Creates an instance with its default state. */
  VariableAnalysis(const VariableAnalysis&) = default;
  /** Replaces the stored state with a copy of another instance. */
  VariableAnalysis& operator=(const VariableAnalysis&) = default;
};

}  // namespace sun::ast
