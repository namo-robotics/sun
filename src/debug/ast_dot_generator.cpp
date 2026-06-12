// ast_dot_generator.cpp — Generate Graphviz DOT representation of AST

#include "debug/ast_dot_generator.h"

#include <sstream>

std::string AstDotGenerator::generate(const ExprAST* root) {
  out.str("");
  out.clear();
  nodeCounter = 0;

  out << "digraph AST {\n";
  out << "  rankdir=TB;\n";
  out << "  node [shape=box, fontname=\"Courier\", fontsize=10];\n";
  out << "  edge [fontname=\"Courier\", fontsize=9];\n";
  out << "\n";

  if (root) {
    visitNode(root);
  }

  out << "}\n";
  return out.str();
}

std::string AstDotGenerator::escapeLabel(const std::string& s) {
  std::string result;
  result.reserve(s.size());
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
      case '\t':
        result += "\\t";
        break;
      case '<':
        result += "&lt;";
        break;
      case '>':
        result += "&gt;";
        break;
      default:
        result += c;
    }
  }
  // Truncate very long labels
  if (result.size() > 50) {
    result = result.substr(0, 47) + "...";
  }
  return result;
}

std::string AstDotGenerator::getNodeLabel(const ExprAST* node) {
  if (!node) return "null";
  return escapeLabel(node->dotLabel());
}

int AstDotGenerator::visitNode(const ExprAST* node) {
  if (!node) return -1;

  int id = nodeCounter++;
  std::string label = getNodeLabel(node);

  out << "  n" << id << " [label=\"" << label << "\"];\n";

  visitChildren(node, id);

  return id;
}

void AstDotGenerator::visitChildren(const ExprAST* node, int parentId) {
  if (!node) return;

  auto emitEdge = [&](int childId, const std::string& label = "") {
    if (childId >= 0) {
      out << "  n" << parentId << " -> n" << childId;
      if (!label.empty()) {
        out << " [label=\"" << label << "\"]";
      }
      out << ";\n";
    }
  };

  switch (node->getType()) {
    case ASTNodeType::BLOCK: {
      const auto* block = static_cast<const BlockExprAST*>(node);
      for (const auto& expr : block->getBody()) {
        int childId = visitNode(expr.get());
        emitEdge(childId);
      }
      break;
    }
    case ASTNodeType::FUNCTION: {
      const auto* func = static_cast<const FunctionAST*>(node);
      if (func->hasBody()) {
        int bodyId = visitNode(&func->getBody());
        emitEdge(bodyId, "body");
      }
      break;
    }
    case ASTNodeType::LAMBDA: {
      const auto* lambda = static_cast<const LambdaAST*>(node);
      int bodyId = visitNode(&lambda->getBody());
      emitEdge(bodyId, "body");
      break;
    }
    case ASTNodeType::VARIABLE_CREATION: {
      const auto* var = static_cast<const VariableCreationAST*>(node);
      if (var->getValue()) {
        int initId = visitNode(var->getValue());
        emitEdge(initId, "init");
      }
      break;
    }
    case ASTNodeType::VARIABLE_ASSIGNMENT: {
      const auto* var = static_cast<const VariableAssignmentAST*>(node);
      int valId = visitNode(var->getValue());
      emitEdge(valId, "value");
      break;
    }
    case ASTNodeType::REFERENCE_CREATION: {
      const auto* ref = static_cast<const ReferenceCreationAST*>(node);
      int targetId = visitNode(ref->getTarget());
      emitEdge(targetId, "target");
      break;
    }
    case ASTNodeType::BINARY: {
      const auto* bin = static_cast<const BinaryExprAST*>(node);
      int lhsId = visitNode(bin->getLHS());
      int rhsId = visitNode(bin->getRHS());
      emitEdge(lhsId, "lhs");
      emitEdge(rhsId, "rhs");
      break;
    }
    case ASTNodeType::UNARY: {
      const auto* un = static_cast<const UnaryExprAST*>(node);
      int opId = visitNode(un->getOperand());
      emitEdge(opId);
      break;
    }
    case ASTNodeType::CALL: {
      const auto* call = static_cast<const CallExprAST*>(node);
      int calleeId = visitNode(call->getCallee());
      emitEdge(calleeId, "callee");
      const auto& args = call->getArgs();
      for (size_t i = 0; i < args.size(); ++i) {
        int argId = visitNode(args[i].get());
        emitEdge(argId, "arg" + std::to_string(i));
      }
      break;
    }
    case ASTNodeType::GENERIC_CALL: {
      const auto* gcall = static_cast<const GenericCallAST*>(node);
      const auto& args = gcall->getArgs();
      for (size_t i = 0; i < args.size(); ++i) {
        int argId = visitNode(args[i].get());
        emitEdge(argId, "arg" + std::to_string(i));
      }
      break;
    }
    case ASTNodeType::IF: {
      const auto* ifExpr = static_cast<const IfExprAST*>(node);
      int condId = visitNode(ifExpr->getCond());
      int thenId = visitNode(ifExpr->getThen());
      emitEdge(condId, "cond");
      emitEdge(thenId, "then");
      if (ifExpr->getElse()) {
        int elseId = visitNode(ifExpr->getElse());
        emitEdge(elseId, "else");
      }
      break;
    }
    case ASTNodeType::MATCH: {
      const auto* match = static_cast<const MatchExprAST*>(node);
      int discId = visitNode(match->getDiscriminant());
      emitEdge(discId, "match");
      const auto& arms = match->getArms();
      for (size_t i = 0; i < arms.size(); ++i) {
        int armId = visitNode(arms[i].body.get());
        emitEdge(armId, "arm" + std::to_string(i));
      }
      break;
    }
    case ASTNodeType::FOR_LOOP: {
      const auto* forExpr = static_cast<const ForExprAST*>(node);
      if (forExpr->getInit()) {
        int initId = visitNode(forExpr->getInit());
        emitEdge(initId, "init");
      }
      if (forExpr->getCondition()) {
        int condId = visitNode(forExpr->getCondition());
        emitEdge(condId, "cond");
      }
      if (forExpr->getIncrement()) {
        int stepId = visitNode(forExpr->getIncrement());
        emitEdge(stepId, "step");
      }
      int bodyId = visitNode(forExpr->getBody());
      emitEdge(bodyId, "body");
      break;
    }
    case ASTNodeType::FOR_IN_LOOP: {
      const auto* forIn = static_cast<const ForInExprAST*>(node);
      int iterableId = visitNode(forIn->getIterable());
      emitEdge(iterableId, "iterable");
      int bodyId = visitNode(forIn->getBody());
      emitEdge(bodyId, "body");
      break;
    }
    case ASTNodeType::WHILE_LOOP: {
      const auto* whileExpr = static_cast<const WhileExprAST*>(node);
      int condId = visitNode(whileExpr->getCondition());
      emitEdge(condId, "cond");
      int bodyId = visitNode(whileExpr->getBody());
      emitEdge(bodyId, "body");
      break;
    }
    case ASTNodeType::RETURN: {
      const auto* ret = static_cast<const ReturnExprAST*>(node);
      if (ret->hasValue()) {
        int valId = visitNode(ret->getValue());
        emitEdge(valId);
      }
      break;
    }
    case ASTNodeType::ARRAY_LITERAL: {
      const auto* arr = static_cast<const ArrayLiteralAST*>(node);
      const auto& elems = arr->getElements();
      for (size_t i = 0; i < elems.size(); ++i) {
        int elemId = visitNode(elems[i].get());
        emitEdge(elemId, "[" + std::to_string(i) + "]");
      }
      break;
    }
    case ASTNodeType::INDEX: {
      const auto* idx = static_cast<const IndexAST*>(node);
      int baseId = visitNode(idx->getTarget());
      emitEdge(baseId, "target");
      // SliceExprAST indices - just count them, don't traverse
      break;
    }
    case ASTNodeType::INDEXED_ASSIGNMENT: {
      const auto* ia = static_cast<const IndexedAssignmentAST*>(node);
      int targetId = visitNode(ia->getTarget());
      emitEdge(targetId, "target");
      int valId = visitNode(ia->getValue());
      emitEdge(valId, "value");
      break;
    }
    case ASTNodeType::MODULE: {
      const auto* mod = static_cast<const ModuleAST*>(node);
      int bodyId = visitNode(&mod->getBody());
      emitEdge(bodyId, "body");
      break;
    }
    case ASTNodeType::MOON_SCOPE: {
      // Don't expand MoonScopeAST nodes - they represent imported modules
      // and expanding them clutters the debug output
      break;
    }
    case ASTNodeType::CLASS_DEFINITION: {
      const auto* cls = static_cast<const ClassDefinitionAST*>(node);
      // Visit methods
      for (const auto& method : cls->getMethods()) {
        int methodId = visitNode(method.function.get());
        emitEdge(methodId, "method");
      }
      break;
    }
    case ASTNodeType::INTERFACE_DEFINITION: {
      // Interface methods are prototypes, not full functions
      break;
    }
    case ASTNodeType::MEMBER_ACCESS: {
      const auto* ma = static_cast<const MemberAccessAST*>(node);
      int objId = visitNode(ma->getObject());
      emitEdge(objId, "object");
      break;
    }
    case ASTNodeType::MEMBER_ASSIGNMENT: {
      const auto* ma = static_cast<const MemberAssignmentAST*>(node);
      int objId = visitNode(ma->getObject());
      emitEdge(objId, "object");
      int valId = visitNode(ma->getValue());
      emitEdge(valId, "value");
      break;
    }
    case ASTNodeType::TRY_CATCH: {
      const auto* tc = static_cast<const TryCatchExprAST*>(node);
      int tryId = visitNode(&tc->getTryBlock());
      emitEdge(tryId, "try");
      int catchId = visitNode(tc->getCatchClause().body.get());
      emitEdge(catchId, "catch");
      break;
    }
    case ASTNodeType::THROW: {
      const auto* th = static_cast<const ThrowExprAST*>(node);
      int valId = visitNode(&th->getErrorExpr());
      emitEdge(valId);
      break;
    }
    case ASTNodeType::SPAWN: {
      const auto* sp = static_cast<const SpawnExprAST*>(node);
      int lambdaId = visitNode(&sp->getLambda());
      emitEdge(lambdaId, "lambda");
      break;
    }
    case ASTNodeType::UNSAFE_BLOCK: {
      const auto* ub = static_cast<const UnsafeBlockAST*>(node);
      int bodyId = visitNode(&ub->getBody());
      emitEdge(bodyId, "body");
      break;
    }
    // Leaf nodes with no children
    case ASTNodeType::NUMBER:
    case ASTNodeType::STRING_LITERAL:
    case ASTNodeType::BOOL_LITERAL:
    case ASTNodeType::NULL_LITERAL:
    case ASTNodeType::VARIABLE_REFERENCE:
    case ASTNodeType::THIS:
    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
    case ASTNodeType::IMPORT:
    case ASTNodeType::USING:
    case ASTNodeType::QUALIFIED_NAME:
    case ASTNodeType::PROTOTYPE:
    case ASTNodeType::ENUM_DEFINITION:
    case ASTNodeType::SLICE:
    case ASTNodeType::ARRAY_INDEX:
    case ASTNodeType::PACK_EXPANSION:
    case ASTNodeType::DECLARE_TYPE:
      // No children to visit
      break;
    default:
      break;
  }
}
