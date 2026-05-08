// codegen_visitor.cpp - Main expression dispatch and basic expression codegen

#include "codegen_visitor.h"

#include <cmath>
#include <cstdint>

#include "ast.h"
#include "codegen.h"

static ExitOnError ExitOnErr;

using namespace llvm;

// -------------------------------------------------------------------
// Expression dispatch
// -------------------------------------------------------------------
Value* CodegenVisitor::codegen(const ExprAST& expr) {
  // Skip nodes marked by semantic analyzer (e.g. diamond dependency duplicates)
  if (expr.shouldSkipCodegen()) {
    return ConstantFP::get(ctx.getContext(), APFloat(0.0));
  }

  switch (expr.getType()) {
    case ASTNodeType::NUMBER:
      return codegen(static_cast<const NumberExprAST&>(expr));
    case ASTNodeType::STRING_LITERAL:
      return codegen(static_cast<const StringLiteralAST&>(expr));
    case ASTNodeType::ARRAY_LITERAL:
      return codegen(static_cast<const ArrayLiteralAST&>(expr));
    case ASTNodeType::INDEX:
      return codegen(static_cast<const IndexAST&>(expr));
    case ASTNodeType::VARIABLE_CREATION:
      return codegen(static_cast<const VariableCreationAST&>(expr));
    case ASTNodeType::VARIABLE_REFERENCE:
      return codegen(static_cast<const VariableReferenceAST&>(expr));
    case ASTNodeType::VARIABLE_ASSIGNMENT:
      return codegen(static_cast<const VariableAssignmentAST&>(expr));
    case ASTNodeType::REFERENCE_CREATION:
      return codegen(static_cast<const ReferenceCreationAST&>(expr));
    case ASTNodeType::UNARY:
      return codegen(static_cast<const UnaryExprAST&>(expr));
    case ASTNodeType::BINARY:
      return codegen(static_cast<const BinaryExprAST&>(expr));
    case ASTNodeType::CALL:
      return codegen(static_cast<const CallExprAST&>(expr));
    case ASTNodeType::IF:
      return codegen(static_cast<const IfExprAST&>(expr));
    case ASTNodeType::MATCH:
      return codegen(static_cast<const MatchExprAST&>(expr));
    case ASTNodeType::FOR_LOOP:
      return codegen(static_cast<const ForExprAST&>(expr));
    case ASTNodeType::FOR_IN_LOOP:
      return codegen(static_cast<const ForInExprAST&>(expr));
    case ASTNodeType::WHILE_LOOP:
      return codegen(static_cast<const WhileExprAST&>(expr));
    case ASTNodeType::BLOCK:
      return codegen(static_cast<const BlockExprAST&>(expr));
    case ASTNodeType::INDEXED_ASSIGNMENT:
      return codegen(static_cast<const IndexedAssignmentAST&>(expr));
    case ASTNodeType::FUNCTION:
      return codegenFunc(
          const_cast<FunctionAST&>(static_cast<const FunctionAST&>(expr)));
    case ASTNodeType::LAMBDA:
      return codegenLambda(
          const_cast<LambdaAST&>(static_cast<const LambdaAST&>(expr)));
    case ASTNodeType::RETURN:
      return codegen(static_cast<const ReturnExprAST&>(expr));
    case ASTNodeType::BREAK_STMT:
      return codegen(static_cast<const BreakAST&>(expr));
    case ASTNodeType::CONTINUE_STMT:
      return codegen(static_cast<const ContinueAST&>(expr));
    case ASTNodeType::IMPORT:
      // Import statements are processed before codegen (by the parser).
      // Nothing to generate - return a void/zero value.
      return ConstantFP::get(ctx.getContext(), APFloat(0.0));
    case ASTNodeType::IMPORT_SCOPE: {
      // Expanded import scope — generate code for all declarations inside.
      // Diamond dependency duplicates are already marked skipCodegen by SA.
      const auto& importScope = static_cast<const ImportScopeAST&>(expr);
      return codegen(importScope.getBody());
    }
    case ASTNodeType::DECLARE_TYPE: {
      // Declare statements trigger generic class instantiation.
      // Semantic analysis resolved the type; specialized class should already
      // be generated at definition site.
      const auto& declareExpr = static_cast<const DeclareTypeAST&>(expr);
      if (!declareExpr.hasResolvedDeclaredType()) {
        logAndThrowError(
            "Internal error: declare type not resolved by semantic analysis");
        return nullptr;
      }
      // Type was resolved - nothing more to do in codegen
      return ConstantFP::get(ctx.getContext(), APFloat(0.0));
    }
    case ASTNodeType::CLASS_DEFINITION:
      return codegen(static_cast<const ClassDefinitionAST&>(expr));
    case ASTNodeType::INTERFACE_DEFINITION:
      return codegen(static_cast<const InterfaceDefinitionAST&>(expr));
    case ASTNodeType::ENUM_DEFINITION:
      return codegen(static_cast<const EnumDefinitionAST&>(expr));
    case ASTNodeType::THIS:
      return codegen(static_cast<const ThisExprAST&>(expr));
    case ASTNodeType::MEMBER_ACCESS:
      return codegen(static_cast<const MemberAccessAST&>(expr));
    case ASTNodeType::MEMBER_ASSIGNMENT:
      return codegen(static_cast<const MemberAssignmentAST&>(expr));
    case ASTNodeType::TRY_CATCH:
      return codegen(static_cast<const TryCatchExprAST&>(expr));
    case ASTNodeType::THROW:
      return codegen(static_cast<const ThrowExprAST&>(expr));
    case ASTNodeType::GENERIC_CALL:
      return codegen(static_cast<const GenericCallAST&>(expr));
    case ASTNodeType::PACK_EXPANSION: {
      // Pack expansion (args...) cannot be used as a standalone expression
      // It must be used in a call argument position to expand variadic args
      logAndThrowError(
          "Pack expansion '...' can only be used in function call arguments");
      return nullptr;
    }
    case ASTNodeType::NAMESPACE: {
      // Module declarations: generate code for all declarations inside
      // Name mangling is handled by semantic analysis (qualified names on AST)
      const auto& ns = static_cast<const NamespaceAST&>(expr);
      return codegen(ns.getBody());
    }
    case ASTNodeType::USING: {
      // Using imports are resolved by semantic analysis and stored on AST
      // nodes. Codegen doesn't need to track them separately.
      return ConstantFP::get(ctx.getContext(), APFloat(0.0));
    }
    case ASTNodeType::QUALIFIED_NAME: {
      // Qualified name lookup (e.g., sun.Vec)
      const auto& qn = static_cast<const QualifiedNameAST&>(expr);
      std::string fullName = qn.getFullName();
      std::string mangledName = qn.getMangledName();

      // Try to find as a function
      Function* func = module->getFunction(mangledName);
      if (func) {
        return func;
      }

      // Try to find as a global variable
      GlobalVariable* gv = module->getGlobalVariable(mangledName);
      if (gv) {
        return ctx.builder->CreateLoad(gv->getValueType(), gv,
                                       mangledName + ".val");
      }

      logAndThrowError("Unknown qualified name: " + fullName);
      return nullptr;
    }
    case ASTNodeType::NULL_LITERAL:
      return ConstantPointerNull::get(PointerType::getUnqual(ctx.getContext()));
    case ASTNodeType::BOOL_LITERAL: {
      const auto& boolLit = static_cast<const BoolLiteralAST&>(expr);
      return ConstantInt::get(llvm::Type::getInt1Ty(ctx.getContext()),
                              boolLit.getValue() ? 1 : 0);
    }
    default:
      break;
  }
  logAndThrowError("Unknown expression node type");
}

// -------------------------------------------------------------------
// Number and string literals
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const NumberExprAST& expr) {
  if (expr.isInteger()) {
    int64_t val = expr.getIntVal();

    // Use the resolved type if available (set by semantic analyzer for
    // context-dependent typing)
    sun::TypePtr resolvedType = expr.getResolvedType();
    if (resolvedType && resolvedType->isPrimitive()) {
      const auto* primType =
          static_cast<const sun::PrimitiveType*>(resolvedType.get());
      switch (primType->getKind()) {
        case sun::Type::Kind::Int8:
          return ConstantInt::get(Type::getInt8Ty(ctx.getContext()),
                                  static_cast<int8_t>(val));
        case sun::Type::Kind::Int16:
          return ConstantInt::get(Type::getInt16Ty(ctx.getContext()),
                                  static_cast<int16_t>(val));
        case sun::Type::Kind::Int32:
          return ConstantInt::get(Type::getInt32Ty(ctx.getContext()),
                                  static_cast<int32_t>(val));
        case sun::Type::Kind::Int64:
          return ConstantInt::get(Type::getInt64Ty(ctx.getContext()), val);
        case sun::Type::Kind::UInt8:
          return ConstantInt::get(Type::getInt8Ty(ctx.getContext()),
                                  static_cast<uint8_t>(val));
        case sun::Type::Kind::UInt16:
          return ConstantInt::get(Type::getInt16Ty(ctx.getContext()),
                                  static_cast<uint16_t>(val));
        case sun::Type::Kind::UInt32:
          return ConstantInt::get(Type::getInt32Ty(ctx.getContext()),
                                  static_cast<uint32_t>(val));
        case sun::Type::Kind::UInt64:
          return ConstantInt::get(Type::getInt64Ty(ctx.getContext()),
                                  static_cast<uint64_t>(val));
        default:
          break;
      }
    }

    // Default behavior: i32 (or i64 if out of i32 range)
    if (val >= INT32_MIN && val <= INT32_MAX) {
      return ConstantInt::get(Type::getInt32Ty(ctx.getContext()),
                              static_cast<int32_t>(val));
    }
    return ConstantInt::get(Type::getInt64Ty(ctx.getContext()), val);
  }
  // Floating point literal -> f64
  return ConstantFP::get(ctx.getContext(), APFloat(expr.getFloatVal()));
}

Value* CodegenVisitor::codegen(const StringLiteralAST& expr) {
  // Create a global string constant
  llvm::GlobalVariable* strGlobal =
      ctx.builder->CreateGlobalString(expr.getValue(), "str");
  // Get pointer to the first character (i8*)
  Value* strPtr = ctx.builder->CreateConstGEP2_32(strGlobal->getValueType(),
                                                  strGlobal, 0, 0, "str.ptr");

  // Create the fat pointer struct: { ptr data, i64 length }
  // Length is the string length (excluding null terminator)
  size_t strLen = expr.getValue().size();
  Value* length =
      ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()), strLen);

  // Build the struct { ptr, i64 } - reuse existing type if available
  llvm::StructType* staticPtrType = typeResolver.getStaticPtrType();

  Value* fatPtr = UndefValue::get(staticPtrType);
  fatPtr = ctx.builder->CreateInsertValue(fatPtr, strPtr, 0, "fatptr.data");
  fatPtr = ctx.builder->CreateInsertValue(fatPtr, length, 1, "fatptr.len");

  return fatPtr;
}

// -------------------------------------------------------------------
// Binary and unary expressions
// -------------------------------------------------------------------

Value* CodegenVisitor::codegen(const BinaryExprAST& expr) {
  // Standard scalar binary operations
  Value* L = codegen(*expr.getLHS());
  Value* R = codegen(*expr.getRHS());
  if (!L || !R) return nullptr;

  llvm::Type* LT = L->getType();
  llvm::Type* RT = R->getType();

  // Handle implicit type widening when safe
  if (LT != RT) {
    // Integer widening: always widen smaller to larger (safe)
    if (LT->isIntegerTy() && RT->isIntegerTy()) {
      unsigned lhsBits = LT->getIntegerBitWidth();
      unsigned rhsBits = RT->getIntegerBitWidth();

      if (lhsBits < rhsBits) {
        // Widen LHS to match RHS type
        L = ctx.builder->CreateSExt(L, RT, "widen");
        LT = RT;
      } else if (rhsBits < lhsBits) {
        // Widen RHS to match LHS type
        R = ctx.builder->CreateSExt(R, LT, "widen");
        RT = LT;
      }
    }
    // Float widening: always widen f32 to f64 (safe)
    else if (LT->isFloatingPointTy() && RT->isFloatingPointTy()) {
      if (LT->isFloatTy() && RT->isDoubleTy()) {
        // Widen LHS f32 to f64
        L = ctx.builder->CreateFPExt(L, RT, "widen");
        LT = RT;
      } else if (RT->isFloatTy() && LT->isDoubleTy()) {
        // Widen RHS f32 to f64
        R = ctx.builder->CreateFPExt(R, LT, "widen");
        RT = LT;
      }
    } else if (LT->isIntegerTy() && RT->isFloatingPointTy()) {
      logAndThrowError(
          "Type mismatch in binary operation: cannot mix integer and float",
          expr.getLocation());
    } else if (LT->isFloatingPointTy() && RT->isIntegerTy()) {
      logAndThrowError(
          "Type mismatch in binary operation: cannot mix float and integer",
          expr.getLocation());
    } else {
      logAndThrowError(
          "Type mismatch in binary operation: incompatible operand types",
          expr.getLocation());
    }
  }

  // Final type check after potential widening
  if (LT != RT) {
    logAndThrowError(
        "Type mismatch in binary operation: operands must have the same type",
        expr.getLocation());
  }

  bool isInteger = LT->isIntegerTy();
  bool isPointer = LT->isPointerTy();

  switch (expr.getOp().kind) {
    case TokenKind::PLUS:
      return isInteger ? ctx.builder->CreateAdd(L, R, "addtmp")
                       : ctx.builder->CreateFAdd(L, R, "addtmp");
    case TokenKind::MINUS:
      return isInteger ? ctx.builder->CreateSub(L, R, "subtmp")
                       : ctx.builder->CreateFSub(L, R, "subtmp");
    case TokenKind::STAR:
      return isInteger ? ctx.builder->CreateMul(L, R, "multmp")
                       : ctx.builder->CreateFMul(L, R, "multmp");
    case TokenKind::SLASH: {
      if (isInteger && currentFunctionCanError) {
        // Safe division: check for zero and return error if so
        return codegenSafeDivision(L, R);
      }
      return isInteger ? ctx.builder->CreateSDiv(L, R, "divtmp")
                       : ctx.builder->CreateFDiv(L, R, "divtmp");
    }
    case TokenKind::LESS:
      if (isInteger) {
        L = ctx.builder->CreateICmpSLT(L, R, "cmptmp");
      } else {
        L = ctx.builder->CreateFCmpULT(L, R, "cmptmp");
      }
      return L;
    case TokenKind::LESS_EQUAL:
      if (isInteger) {
        L = ctx.builder->CreateICmpSLE(L, R, "cmptmp");
      } else {
        L = ctx.builder->CreateFCmpULE(L, R, "cmptmp");
      }
      return L;
    case TokenKind::GREATER:
      if (isInteger) {
        L = ctx.builder->CreateICmpSGT(L, R, "cmptmp");
      } else {
        L = ctx.builder->CreateFCmpUGT(L, R, "cmptmp");
      }
      return L;
    case TokenKind::GREATER_EQUAL:
      if (isInteger) {
        L = ctx.builder->CreateICmpSGE(L, R, "cmptmp");
      } else {
        L = ctx.builder->CreateFCmpUGE(L, R, "cmptmp");
      }
      return L;
    case TokenKind::EQUAL_EQUAL:
      if (isPointer) {
        L = ctx.builder->CreateICmpEQ(L, R, "cmptmp");
      } else if (isInteger) {
        L = ctx.builder->CreateICmpEQ(L, R, "cmptmp");
      } else {
        L = ctx.builder->CreateFCmpOEQ(L, R, "cmptmp");
      }
      return L;
    case TokenKind::NOT_EQUAL:
      if (isPointer) {
        L = ctx.builder->CreateICmpNE(L, R, "cmptmp");
      } else if (isInteger) {
        L = ctx.builder->CreateICmpNE(L, R, "cmptmp");
      } else {
        L = ctx.builder->CreateFCmpONE(L, R, "cmptmp");
      }
      return L;
    default:
      logAndThrowError("Unknown binary operator: " + expr.getOp().text,
                       expr.getLocation());
  }
}

Value* CodegenVisitor::codegen(const UnaryExprAST& expr) {
  Value* OperandV = codegen(*expr.getOperand());
  if (!OperandV) return nullptr;

  logAndThrowError("Unknown unary operator: " + expr.getOp().text,
                   expr.getLocation());
}
