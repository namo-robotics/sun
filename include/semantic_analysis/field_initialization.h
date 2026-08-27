// field_initialization.h — What is initialized where inside a constructor

#pragma once

#include <vector>

#include "ast/ast_fwd.h"
#include "ast/class_definition_ast.h"
#include "semantic_analysis/types.h"

namespace sun {

/**
 * Checks a constructor against Sun's two-phase rule, and decides what each
 * write to a field does to the value that was there.
 *
 * The first phase runs until every field has been assigned. The body may
 * assign fields, and it may call the object's own methods — the walk follows
 * the call into the method's body with the state at that point, so a method
 * may only read fields that already have a value, and what it assigns counts
 * towards the constructor's own obligation. A method's write always replaces
 * and drops, whoever calls it; before a field's first value the storage is
 * all zero, which an owning deinit treats as nothing to release. Reading an
 * unassigned field, or handing `this` to anything else, is rejected: the
 * object is not a whole value yet. The second phase begins once every field
 * has a value, and behaves like any other method body.
 *
 * A constructor that can reach its end, or a `return`, with a field still
 * unassigned is rejected. That field would silently be zero — the same reason
 * a struct literal has to name every field. A write to an owning field that
 * only some paths have assigned is rejected too: it could neither start the
 * field's life nor drop what it replaces.
 *
 * Along the way each write is tagged: a write that starts a field's life
 * drops nothing, and every other write drops what it replaces. Nothing is
 * decided at run time.
 */
void checkFieldInitialization(const FunctionAST& constructor,
                              const ClassType& classType,
                              const std::vector<ClassMethodDecl>& methods);

}  // namespace sun
