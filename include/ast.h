// ast.h — umbrella header that includes all AST node headers
//
// This file provides backward compatibility for existing code that includes
// "ast.h". All AST node classes are now defined in individual headers under
// include/ast/.

#pragma once

// Foundation headers
#include "ast/analysis.h"
#include "ast/ast_common.h"
#include "ast/ast_fwd.h"
#include "ast/ast_utils.h"
#include "ast/expr_ast.h"
#include "ast/type_annotation.h"

// Literal nodes
#include "ast/array_literal_ast.h"
#include "ast/bool_literal_ast.h"
#include "ast/null_literal_ast.h"
#include "ast/number_expr_ast.h"
#include "ast/string_literal_ast.h"

// Index/Slice nodes
#include "ast/array_index_ast.h"
#include "ast/index_ast.h"
#include "ast/slice_expr_ast.h"

// Variable nodes
#include "ast/reference_creation_ast.h"
#include "ast/variable_assignment_ast.h"
#include "ast/variable_creation_ast.h"
#include "ast/variable_reference_ast.h"

// Expression nodes
#include "ast/binary_expr_ast.h"
#include "ast/pack_expansion_ast.h"
#include "ast/unary_expr_ast.h"

// Control flow nodes
#include "ast/block_expr_ast.h"
#include "ast/break_ast.h"
#include "ast/continue_ast.h"
#include "ast/for_expr_ast.h"
#include "ast/for_in_expr_ast.h"
#include "ast/if_expr_ast.h"
#include "ast/indexed_assignment_ast.h"
#include "ast/match_expr_ast.h"
#include "ast/return_expr_ast.h"
#include "ast/unsafe_block_ast.h"
#include "ast/while_expr_ast.h"

// Function-related nodes
#include "ast/call_expr_ast.h"
#include "ast/function_ast.h"
#include "ast/lambda_ast.h"
#include "ast/prototype_ast.h"
#include "ast/spawn_expr_ast.h"

// Error handling nodes
#include "ast/throw_expr_ast.h"
#include "ast/try_catch_expr_ast.h"

// Module/import nodes
#include "ast/import_ast.h"
#include "ast/import_scope_ast.h"
#include "ast/module_ast.h"
#include "ast/qualified_name_ast.h"
#include "ast/using_ast.h"

// Class/interface/enum nodes
#include "ast/class_definition_ast.h"
#include "ast/enum_definition_ast.h"
#include "ast/interface_definition_ast.h"

// Member access nodes
#include "ast/generic_call_ast.h"
#include "ast/member_access_ast.h"
#include "ast/member_assignment_ast.h"
#include "ast/this_expr_ast.h"

// Type declaration
#include "ast/declare_type_ast.h"

// Legacy content removed - all classes now in individual headers
