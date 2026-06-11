// ast_common.h — Common types shared by all AST nodes

#pragma once

#include <string>

#include "ast/ast_fwd.h"
#include "types.h"

enum class ASTNodeType {
  NUMBER,
  STRING_LITERAL,
  NULL_LITERAL,
  BOOL_LITERAL,
  ARRAY_LITERAL,  // [1, 2, 3] or [[1, 2], [3, 4]]
  ARRAY_INDEX,    // x[i] or x[i, j] for n-dimensional (legacy)
  INDEX,          // x[i] or x[i, j] or x[1:10, 3:5] for indexing/slicing
  SLICE,          // Slice expression: start:end or single index
  VARIABLE_REFERENCE,
  VARIABLE_CREATION,
  VARIABLE_ASSIGNMENT,
  REFERENCE_CREATION,  // ref x = y - creates a reference to y
  BINARY,
  UNARY,
  CALL,  // Unified call - callee is an expression (can be var ref, function
         // literal, etc.)
  PROTOTYPE,
  FUNCTION,
  LAMBDA,  // Anonymous function (lambda expression)
  IF,
  MATCH,  // match value { pattern => expr, ... }
  FOR_LOOP,
  FOR_IN_LOOP,  // for (var x: T in iterable) { ... }
  WHILE_LOOP,
  BLOCK,
  INDEXED_ASSIGNMENT,
  RETURN,
  IMPORT,        // import "file.sun";
  IMPORT_SCOPE,  // Expanded import scope (contains imported file's AST)
  MANIFEST,      // manifest { ... } - defines package metadata and entry point
  MODULE,        // module Name { ... }
  USING,         // using Module::name; or using Module::*;
  QUALIFIED_NAME,        // Namespace::name
  CLASS_DEFINITION,      // class Name { ... }
  INTERFACE_DEFINITION,  // interface Name { ... }
  ENUM_DEFINITION,       // enum Name { Variant1, Variant2, ... }
  MEMBER_ACCESS,         // object.field or object.method(...)
  THIS,                  // this keyword
  MEMBER_ASSIGNMENT,     // object.field = value
  TRY_CATCH,             // try { ... } catch (e: IError) { ... }
  THROW,                 // throw <expr>
  BREAK_STMT,            // break statement
  CONTINUE_STMT,         // continue statement
  GENERIC_CALL,          // Generic function call: create<Type>(args...)
  PACK_EXPANSION,        // args... - expand a variadic parameter pack
  DECLARE_TYPE,          // declare [Alias =] Type<Args>;
  SPAWN,                 // spawn(lambda) - create OS thread
  UNSAFE_BLOCK           // unsafe { ... } - unsafe operations block
};

struct Capture {
  std::string name;
  sun::TypePtr type;
};
