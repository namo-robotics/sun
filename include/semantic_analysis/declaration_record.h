#pragma once

#include <memory>
#include <string>

#include "semantic_analysis/declaration_id.h"

/** Defines syntax-tree nodes and the annotations used to analyze them. */
namespace sun::ast {
class ExprAST;
}

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
struct SpecializationKey;

/** The source role of a declaration, independent of its resolved type. */
enum class DeclarationKind {
  Module = 0,
  Function = 1,
  Lambda = 2,
  Class = 3,
  Interface = 4,
  Enum = 5,
  Variable = 6,
  Reference = 7,
  Parameter = 8,
  TypeParameter = 9,
  LifetimeParameter = 10,
  Field = 11,
  Variant = 12,
  Binding = 13,
  Alias = 14
};

/** Declaration metadata shared by imported artifacts and analysis sessions. */
struct DeclarationRecord {
  DeclarationKind kind;
  std::string name;
  DeclarationId owner;
  DeclarationId module;
  bool imported = false;
  std::shared_ptr<const SpecializationKey> specialization;
  DeclarationId origin;
  std::string generatedRole;
  uint64_t generatedSlot = 0;
  // The syntax node that declares it, or null when no single node does: a
  // builtin, a field or variant inside a node, a module opened in several
  // places. Valid while the analyzed tree is alive, which the owner of the
  // analysis results keeps it for.
  const sun::ast::ExprAST* astNode = nullptr;
  // Artifact ownership is metadata, independent of the portable identifier.
  std::string bundleHash;
};

}  // namespace sun::semantic_analysis
