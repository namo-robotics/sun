// codegen_visitor.cpp - Main expression dispatch and basic expression codegen

#include "codegen/codegen_visitor.h"

#include <cstdint>

#include "codegen/codegen.h"

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

  debugInfo.attachExpressionLocation(*ctx.builder, expr.getLocation());

  // Reading an expression reads *through* any reference it produced. The
  // contexts that want the address instead — binding a ref variable, passing
  // to a ref parameter, returning a ref, assigning through one — go via
  // tryCodegenAddress rather than here.
  return loadIfRef(codegenExpression(expr), expr.getResolvedType());
}

Value* CodegenVisitor::codegenExpression(const ExprAST& expr) {
  switch (expr.getType()) {
    case ASTNodeType::NUMBER:
      return codegen(static_cast<const NumberExprAST&>(expr));
    case ASTNodeType::CHAR_LITERAL:
      return codegen(static_cast<const CharLiteralAST&>(expr));
    case ASTNodeType::STRING_LITERAL:
      return codegen(static_cast<const StringLiteralAST&>(expr));
    case ASTNodeType::STRUCT_LITERAL:
      return codegen(static_cast<const StructLiteralAST&>(expr));
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
    case ASTNodeType::TERNARY:
      return codegen(static_cast<const TernaryExprAST&>(expr));
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
    case ASTNodeType::COMPOUND_ASSIGNMENT:
      return codegen(static_cast<const CompoundAssignmentAST&>(expr));
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
      // Import statements should never reach codegen (parser errors on them)
      return ConstantFP::get(ctx.getContext(), APFloat(0.0));
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
    case ASTNodeType::UNSAFE_BLOCK:
      return codegen(static_cast<const UnsafeBlockAST&>(expr));
    case ASTNodeType::GENERIC_CALL:
      return codegen(static_cast<const GenericCallAST&>(expr));
    case ASTNodeType::PACK_EXPANSION: {
      // Pack expansion (args...) cannot be used as a standalone expression
      // It must be used in a call argument position to expand variadic args
      logAndThrowError(
          "Pack expansion '...' can only be used in function call arguments");
      return nullptr;
    }
    case ASTNodeType::MODULE: {
      // Module declarations: generate code for all declarations inside
      // Name mangling is handled by semantic analysis (qualified names on AST)
      const auto& ns = static_cast<const ModuleAST&>(expr);
      return codegen(ns.getBody());
    }
    case ASTNodeType::MOON_SCOPE: {
      // Moon scope wraps module stubs from a moon import
      // Generate code for all contained modules
      const auto& moonScope = static_cast<const MoonScopeAST&>(expr);
      return codegen(moonScope.getBody());
    }
    case ASTNodeType::USING: {
      // Using imports are resolved by semantic analysis and stored on AST
      // nodes. Codegen doesn't need to track them separately.
      return ConstantFP::get(ctx.getContext(), APFloat(0.0));
    }
    case ASTNodeType::MANIFEST: {
      // Manifest blocks are processed by the driver before compilation.
      // Nothing to generate for codegen.
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

Value* CodegenVisitor::codegen(const CharLiteralAST& expr) {
  // A byte literal is a u8; a char is a Unicode scalar value in an i32.
  return ConstantInt::get(expr.isByte() ? Type::getInt8Ty(ctx.getContext())
                                        : Type::getInt32Ty(ctx.getContext()),
                          expr.getValue());
}

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
        case sun::Type::Kind::Bool:
          return ConstantInt::get(Type::getInt1Ty(ctx.getContext()), val != 0);
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
  // Floating point literal -> f64 unless context typed it f32
  auto resolvedType = expr.getResolvedType();
  if (resolvedType && resolvedType->isFloat32()) {
    return ConstantFP::get(Type::getFloatTy(ctx.getContext()),
                           expr.getFloatVal());
  }
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

// True if the expression's resolved Sun type is an unsigned integer.
// Floats, bool, and enums answer false and take the signed/default path.
static bool isUnsignedExpr(const ExprAST& expr) {
  auto type = sun::unwrapRef(expr.getResolvedType());
  return type && type->isUnsigned();
}

Value* CodegenVisitor::extendInt(Value* value, llvm::Type* destTy,
                                 const sun::TypePtr& sourceType) {
  auto srcType = sun::unwrapRef(sourceType);
  return srcType && srcType->isUnsigned()
             ? ctx.builder->CreateZExt(value, destTy, "widen")
             : ctx.builder->CreateSExt(value, destTy, "widen");
}

Value* CodegenVisitor::createIntDivRem(Value* L, Value* R, bool isModulo,
                                       bool isUnsigned) {
  if (isModulo) {
    return isUnsigned ? ctx.builder->CreateURem(L, R, "modtmp")
                      : ctx.builder->CreateSRem(L, R, "modtmp");
  }
  return isUnsigned ? ctx.builder->CreateUDiv(L, R, "divtmp")
                    : ctx.builder->CreateSDiv(L, R, "divtmp");
}

// Bring two scalar operands to a common type (int and float widening);
// throws on incompatible operand types. Extension mode follows each
// operand's own Sun-type signedness.
void CodegenVisitor::unifyBinaryOperands(Value*& L, Value*& R,
                                         const sun::TypePtr& lhsSunType,
                                         const sun::TypePtr& rhsSunType,
                                         const Position& loc) {
  llvm::Type* LT = L->getType();
  llvm::Type* RT = R->getType();
  if (LT == RT) return;

  // Integer widening: always widen smaller to larger (safe)
  if (LT->isIntegerTy() && RT->isIntegerTy()) {
    unsigned lhsBits = LT->getIntegerBitWidth();
    unsigned rhsBits = RT->getIntegerBitWidth();

    if (lhsBits < rhsBits) {
      L = extendInt(L, RT, lhsSunType);
    } else if (rhsBits < lhsBits) {
      R = extendInt(R, LT, rhsSunType);
    }
  }
  // Float widening: always widen f32 to f64 (safe)
  else if (LT->isFloatingPointTy() && RT->isFloatingPointTy()) {
    if (LT->isFloatTy() && RT->isDoubleTy()) {
      L = ctx.builder->CreateFPExt(L, RT, "widen");
    } else if (RT->isFloatTy() && LT->isDoubleTy()) {
      R = ctx.builder->CreateFPExt(R, LT, "widen");
    }
  } else if (LT->isIntegerTy() && RT->isFloatingPointTy()) {
    logAndThrowError(
        "Type mismatch in binary operation: cannot mix integer and float", loc);
  } else if (LT->isFloatingPointTy() && RT->isIntegerTy()) {
    logAndThrowError(
        "Type mismatch in binary operation: cannot mix float and integer", loc);
  } else {
    logAndThrowError(
        "Type mismatch in binary operation: incompatible operand types", loc);
  }

  // Final type check after potential widening
  if (L->getType() != R->getType()) {
    logAndThrowError(
        "Type mismatch in binary operation: operands must have the same type",
        loc);
  }
}

// Emit an arithmetic/bitwise/shift operation on unified operands. Shared by
// binary expressions and compound assignment. Comparisons and logical ops
// are not handled here.
Value* CodegenVisitor::emitBinaryOp(TokenKind op, Value* L, Value* R,
                                    bool unsignedOp, const Position& loc) {
  bool isInteger = L->getType()->isIntegerTy();

  switch (op) {
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
        return codegenSafeDivision(L, R, /*isModulo=*/false, unsignedOp);
      }
      if (!isInteger) return ctx.builder->CreateFDiv(L, R, "divtmp");
      return createIntDivRem(L, R, /*isModulo=*/false, unsignedOp);
    }
    case TokenKind::PERCENT: {
      if (!isInteger) {
        logAndThrowError("Modulo operator (%) requires integer operands", loc);
      }
      if (currentFunctionCanError) {
        // Safe modulo: check for zero and return error if so
        return codegenSafeDivision(L, R, /*isModulo=*/true, unsignedOp);
      }
      return createIntDivRem(L, R, /*isModulo=*/true, unsignedOp);
    }
    case TokenKind::AMPERSAND: {
      if (!isInteger) {
        logAndThrowError("Bitwise AND operator (&) requires integer operands",
                         loc);
      }
      return ctx.builder->CreateAnd(L, R, "andtmp");
    }
    case TokenKind::PIPE: {
      if (!isInteger) {
        logAndThrowError("Bitwise OR operator (|) requires integer operands",
                         loc);
      }
      return ctx.builder->CreateOr(L, R, "ortmp");
    }
    case TokenKind::CARET: {
      if (!isInteger) {
        logAndThrowError("Bitwise XOR operator (^) requires integer operands",
                         loc);
      }
      return ctx.builder->CreateXor(L, R, "xortmp");
    }
    case TokenKind::LEFT_SHIFT: {
      if (!isInteger) {
        logAndThrowError("Left shift operator (<<) requires integer operands",
                         loc);
      }
      return ctx.builder->CreateShl(L, R, "shltmp");
    }
    case TokenKind::RIGHT_SHIFT: {
      if (!isInteger) {
        logAndThrowError("Right shift operator (>>) requires integer operands",
                         loc);
      }
      // Logical shift for unsigned, arithmetic (sign-extending) for signed
      return unsignedOp ? ctx.builder->CreateLShr(L, R, "shrtmp")
                        : ctx.builder->CreateAShr(L, R, "shrtmp");
    }
    default:
      logAndThrowError("Unknown binary operator", loc);
  }
}

Value* CodegenVisitor::codegen(const BinaryExprAST& expr) {
  // Handle short-circuit logical operators (and, or)
  if (expr.getOp().kind == TokenKind::AND ||
      expr.getOp().kind == TokenKind::OR) {
    return codegenLogicalOp(expr);
  }

  // Standard scalar binary operations
  Value* L = codegen(*expr.getLHS());
  Value* R = codegen(*expr.getRHS());
  if (!L || !R) return nullptr;

  // Signedness of div/rem/shift/compare follows the LHS operand's Sun type;
  // mixed signed/unsigned operands are permitted by isAssignableTo.
  bool unsignedOp = isUnsignedExpr(*expr.getLHS());

  unifyBinaryOperands(L, R, expr.getLHS()->getResolvedType(),
                      expr.getRHS()->getResolvedType(), expr.getLocation());

  bool isInteger = L->getType()->isIntegerTy();
  bool isPointer = L->getType()->isPointerTy();

  switch (expr.getOp().kind) {
    case TokenKind::LESS:
    case TokenKind::LESS_EQUAL:
    case TokenKind::GREATER:
    case TokenKind::GREATER_EQUAL: {
      // (signed int, unsigned int, float) predicate per operator
      llvm::CmpInst::Predicate sPred, uPred, fPred;
      switch (expr.getOp().kind) {
        case TokenKind::LESS:
          sPred = llvm::CmpInst::ICMP_SLT;
          uPred = llvm::CmpInst::ICMP_ULT;
          fPred = llvm::CmpInst::FCMP_ULT;
          break;
        case TokenKind::LESS_EQUAL:
          sPred = llvm::CmpInst::ICMP_SLE;
          uPred = llvm::CmpInst::ICMP_ULE;
          fPred = llvm::CmpInst::FCMP_ULE;
          break;
        case TokenKind::GREATER:
          sPred = llvm::CmpInst::ICMP_SGT;
          uPred = llvm::CmpInst::ICMP_UGT;
          fPred = llvm::CmpInst::FCMP_UGT;
          break;
        default:
          sPred = llvm::CmpInst::ICMP_SGE;
          uPred = llvm::CmpInst::ICMP_UGE;
          fPred = llvm::CmpInst::FCMP_UGE;
          break;
      }
      if (!isInteger) return ctx.builder->CreateFCmp(fPred, L, R, "cmptmp");
      return ctx.builder->CreateICmp(unsignedOp ? uPred : sPred, L, R,
                                     "cmptmp");
    }
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
      // Arithmetic / bitwise / shift operators
      return emitBinaryOp(expr.getOp().kind, L, R, unsignedOp,
                          expr.getLocation());
  }
}

Value* CodegenVisitor::codegen(const UnaryExprAST& expr) {
  Value* OperandV = codegen(*expr.getOperand());
  if (!OperandV) return nullptr;

  switch (expr.getOp().kind) {
    case TokenKind::MINUS:
      return OperandV->getType()->isFloatingPointTy()
                 ? ctx.builder->CreateFNeg(OperandV, "negtmp")
                 : ctx.builder->CreateNeg(OperandV, "negtmp");
    case TokenKind::NOT:
      // Semantic analysis guarantees a bool (i1) operand
      return ctx.builder->CreateNot(OperandV, "nottmp");
    case TokenKind::TILDE:
      // Semantic analysis guarantees an integer operand
      return ctx.builder->CreateNot(OperandV, "bnottmp");
    default:
      logAndThrowError("Unknown unary operator: " + expr.getOp().text,
                       expr.getLocation());
  }
}
