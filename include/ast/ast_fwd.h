// ast_fwd.h — Forward declarations for all AST classes

#pragma once

// Forward declarations for all AST node classes
struct TypeAnnotation;
class ExprAST;

// Literal nodes
class NumberExprAST;
class StringLiteralAST;
class NullLiteralAST;
class BoolLiteralAST;
class ArrayLiteralAST;

// Variable nodes
class VariableReferenceAST;
class VariableCreationAST;
class VariableAssignmentAST;
class ReferenceCreationAST;

// Expression nodes
class BinaryExprAST;
class UnaryExprAST;
class IndexAST;
class ArrayIndexAST;
class SliceExprAST;
class PackExpansionAST;

// Block and control flow nodes
class BlockExprAST;
class IfExprAST;
class MatchExprAST;
class ForExprAST;
class ForInExprAST;
class WhileExprAST;
class UnsafeBlockAST;
class BreakAST;
class ContinueAST;
class ReturnExprAST;

// Function-related nodes
class PrototypeAST;
class FunctionAST;
class LambdaAST;
class CallExprAST;
class GenericCallAST;
class SpawnExprAST;

// Class/interface/enum nodes
class ClassDefinitionAST;
class InterfaceDefinitionAST;
class EnumDefinitionAST;

// Member access nodes
class MemberAccessAST;
class MemberAssignmentAST;
class ThisExprAST;
class IndexedAssignmentAST;

// Error handling nodes
class ThrowExprAST;
class TryCatchExprAST;

// Module/import nodes
class ModuleAST;
class UsingAST;
class QualifiedNameAST;

// Type declaration
class DeclareTypeAST;
