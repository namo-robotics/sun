// ast_children.cpp — Read-only enumeration of every direct child of a node

#include "ast/ast_children.h"

#include "ast.h"

namespace {

void visit(const ExprAST* child, const ChildFn& fn) {
  if (child) fn(*child);
}

}  // namespace

void forEachChild(const ExprAST& node, const ChildFn& fn) {
  switch (node.getType()) {
    case ASTNodeType::BLOCK: {
      for (const auto& expr : static_cast<const BlockExprAST&>(node).getBody())
        visit(expr.get(), fn);
      break;
    }
    case ASTNodeType::FUNCTION: {
      const auto& func = static_cast<const FunctionAST&>(node);
      if (func.hasBody()) fn(func.getBody());
      break;
    }
    case ASTNodeType::LAMBDA: {
      const auto& lambda = static_cast<const LambdaAST&>(node);
      if (lambda.hasBody()) fn(lambda.getBody());
      break;
    }
    case ASTNodeType::VARIABLE_CREATION:
      visit(static_cast<const VariableCreationAST&>(node).getValue(), fn);
      break;
    case ASTNodeType::VARIABLE_ASSIGNMENT:
      visit(static_cast<const VariableAssignmentAST&>(node).getValue(), fn);
      break;
    case ASTNodeType::REFERENCE_CREATION:
      visit(static_cast<const ReferenceCreationAST&>(node).getTarget(), fn);
      break;
    case ASTNodeType::PAREN_EXPR:
      visit(static_cast<const ParenExprAST&>(node).getInner(), fn);
      break;
    case ASTNodeType::INTERPOLATED_STRING: {
      const auto& interp = static_cast<const InterpolatedStringAST&>(node);
      for (const auto& segment : interp.getSegments())
        if (!segment.isLiteral) visit(segment.expression.get(), fn);
      break;
    }
    case ASTNodeType::BINARY: {
      const auto& bin = static_cast<const BinaryExprAST&>(node);
      visit(bin.getLHS(), fn);
      visit(bin.getRHS(), fn);
      break;
    }
    case ASTNodeType::UNARY:
      visit(static_cast<const UnaryExprAST&>(node).getOperand(), fn);
      break;
    case ASTNodeType::CALL: {
      const auto& call = static_cast<const CallExprAST&>(node);
      visit(call.getCallee(), fn);
      for (const auto& arg : call.getArgs()) visit(arg.get(), fn);
      break;
    }
    case ASTNodeType::GENERIC_CALL: {
      for (const auto& arg : static_cast<const GenericCallAST&>(node).getArgs())
        visit(arg.get(), fn);
      break;
    }
    case ASTNodeType::IF: {
      const auto& ifExpr = static_cast<const IfExprAST&>(node);
      visit(ifExpr.getCond(), fn);
      visit(ifExpr.getThen(), fn);
      visit(ifExpr.getElse(), fn);
      break;
    }
    case ASTNodeType::TERNARY: {
      const auto& ternary = static_cast<const TernaryExprAST&>(node);
      visit(ternary.getCond(), fn);
      visit(ternary.getThen(), fn);
      visit(ternary.getElse(), fn);
      break;
    }
    case ASTNodeType::MATCH: {
      const auto& match = static_cast<const MatchExprAST&>(node);
      visit(match.getDiscriminant(), fn);
      for (const auto& arm : match.getArms()) {
        visit(arm.pattern.get(), fn);
        visit(arm.body.get(), fn);
      }
      break;
    }
    case ASTNodeType::FOR_LOOP: {
      const auto& forExpr = static_cast<const ForExprAST&>(node);
      visit(forExpr.getInit(), fn);
      visit(forExpr.getCondition(), fn);
      visit(forExpr.getIncrement(), fn);
      visit(forExpr.getBody(), fn);
      break;
    }
    case ASTNodeType::FOR_IN_LOOP: {
      const auto& forIn = static_cast<const ForInExprAST&>(node);
      visit(forIn.getIterable(), fn);
      visit(forIn.getBody(), fn);
      break;
    }
    case ASTNodeType::WHILE_LOOP: {
      const auto& whileExpr = static_cast<const WhileExprAST&>(node);
      visit(whileExpr.getCondition(), fn);
      visit(whileExpr.getBody(), fn);
      break;
    }
    case ASTNodeType::RETURN:
      visit(static_cast<const ReturnExprAST&>(node).getValue(), fn);
      break;
    case ASTNodeType::ARRAY_LITERAL: {
      for (const auto& elem :
           static_cast<const ArrayLiteralAST&>(node).getElements())
        visit(elem.get(), fn);
      break;
    }
    case ASTNodeType::STRUCT_LITERAL: {
      for (const auto& field :
           static_cast<const StructLiteralAST&>(node).getFields())
        visit(field.value.get(), fn);
      break;
    }
    case ASTNodeType::ARRAY_INDEX: {
      const auto& idx = static_cast<const ArrayIndexAST&>(node);
      visit(idx.getArray(), fn);
      for (const auto& index : idx.getIndices()) visit(index.get(), fn);
      break;
    }
    case ASTNodeType::INDEX: {
      const auto& idx = static_cast<const IndexAST&>(node);
      visit(idx.getTarget(), fn);
      for (const auto& slice : idx.getIndices()) visit(slice.get(), fn);
      break;
    }
    case ASTNodeType::SLICE: {
      const auto& slice = static_cast<const SliceExprAST&>(node);
      visit(slice.getStart(), fn);
      visit(slice.getEnd(), fn);
      break;
    }
    case ASTNodeType::INDEXED_ASSIGNMENT: {
      const auto& assign = static_cast<const IndexedAssignmentAST&>(node);
      visit(assign.getTarget(), fn);
      visit(assign.getValue(), fn);
      break;
    }
    case ASTNodeType::MODULE:
      fn(static_cast<const ModuleAST&>(node).getBody());
      break;
    case ASTNodeType::MOON_SCOPE:
      fn(static_cast<const MoonScopeAST&>(node).getBody());
      break;
    case ASTNodeType::CLASS_DEFINITION: {
      for (const auto& method :
           static_cast<const ClassDefinitionAST&>(node).getMethods())
        visit(method.function.get(), fn);
      break;
    }
    case ASTNodeType::INTERFACE_DEFINITION: {
      for (const auto& method :
           static_cast<const InterfaceDefinitionAST&>(node).getMethods())
        visit(method.function.get(), fn);
      break;
    }
    case ASTNodeType::MEMBER_ACCESS:
      visit(static_cast<const MemberAccessAST&>(node).getObject(), fn);
      break;
    case ASTNodeType::MEMBER_ASSIGNMENT: {
      const auto& assign = static_cast<const MemberAssignmentAST&>(node);
      visit(assign.getObject(), fn);
      visit(assign.getValue(), fn);
      break;
    }
    case ASTNodeType::COMPOUND_ASSIGNMENT: {
      const auto& assign = static_cast<const CompoundAssignmentAST&>(node);
      visit(assign.getTarget(), fn);
      visit(assign.getValue(), fn);
      break;
    }
    case ASTNodeType::TRY_CATCH: {
      const auto& tryCatch = static_cast<const TryCatchExprAST&>(node);
      fn(tryCatch.getTryBlock());
      for (const auto& clause : tryCatch.getCatchClauses())
        visit(clause.body.get(), fn);
      break;
    }
    case ASTNodeType::THROW: {
      const auto& throwExpr = static_cast<const ThrowExprAST&>(node);
      if (throwExpr.hasErrorExpr()) fn(throwExpr.getErrorExpr());
      break;
    }
    case ASTNodeType::UNSAFE_BLOCK:
      fn(static_cast<const UnsafeBlockAST&>(node).getBody());
      break;
    // Leaves
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
    case ASTNodeType::CHAR_LITERAL:
    case ASTNodeType::NULL_LITERAL:
    case ASTNodeType::BOOL_LITERAL:
    case ASTNodeType::VARIABLE_REFERENCE:
    case ASTNodeType::PROTOTYPE:
    case ASTNodeType::IMPORT:
    case ASTNodeType::IMPORT_SCOPE:
    case ASTNodeType::MANIFEST:
    case ASTNodeType::USING:
    case ASTNodeType::QUALIFIED_NAME:
    case ASTNodeType::ENUM_DEFINITION:
    case ASTNodeType::THIS:
    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
    case ASTNodeType::PACK_EXPANSION:
    case ASTNodeType::DECLARE_TYPE:
      break;
  }
}
