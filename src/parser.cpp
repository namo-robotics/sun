// src/parser.cpp
#include "parser.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "library_cache.h"
#include "llvm/Support/raw_ostream.h"

#define PARSER_TIMER_START(name) \
  auto parser_timer_##name = std::chrono::high_resolution_clock::now()
#define PARSER_TIMER_END(name)                            \
  do {                                                    \
    auto end = std::chrono::high_resolution_clock::now(); \
    (void)end;                                            \
    (void)parser_timer_##name;                            \
  } while (0)

// === Implement all parsing functions ===

std::unique_ptr<ExprAST> Parser::parseNumberExpr() {
  std::unique_ptr<ExprAST> result;
  if (curTok.kind == TokenKind::INTEGER) {
    result = std::make_unique<NumberExprAST>(curTok.getInteger().value());
  } else {
    result = std::make_unique<NumberExprAST>(curTok.getFloat().value());
  }
  getNextToken();  // consume the number
  return result;
}

std::unique_ptr<ExprAST> Parser::parseStringLiteral() {
  auto result = std::make_unique<StringLiteralAST>(curTok.getString().value());
  getNextToken();  // consume the string
  return result;
}

std::unique_ptr<ExprAST> Parser::parseArrayLiteral() {
  assert(curTok.kind == TokenKind::BRACKET_OPEN);
  getNextToken();  // eat '['

  std::vector<std::unique_ptr<ExprAST>> elements;

  // Handle empty array
  if (curTok.kind == TokenKind::BRACKET_CLOSE) {
    getNextToken();  // eat ']'
    return std::make_unique<ArrayLiteralAST>(std::move(elements));
  }

  // Parse comma-separated elements
  while (true) {
    auto elem = parseExpression();
    if (!elem) return nullptr;
    elements.push_back(std::move(elem));

    if (curTok.kind == TokenKind::BRACKET_CLOSE) break;

    if (curTok.kind != TokenKind::COMMA) {
      parsingError("expected ',' or ']' in array literal");
      return nullptr;
    }
    getNextToken();  // eat ','
  }

  getNextToken();  // eat ']'
  return std::make_unique<ArrayLiteralAST>(std::move(elements));
}

std::unique_ptr<ExprAST> Parser::parseParenExpr() {
  assert(curTok.kind == TokenKind::PAREN_OPEN);
  getNextToken();  // eat (
  auto v = parseExpression();
  if (!v) return nullptr;

  if (curTok.kind != TokenKind::PAREN_CLOSE) parsingError("expected ')'");
  getNextToken();  // eat )
  return v;
}

unique_ptr<IfExprAST> Parser::parseIfStatement() {
  getNextToken();  // eat the 'if'

  auto Cond = parseExpression();
  if (!Cond) return nullptr;

  // Require curly braces for then block
  if (curTok.kind != TokenKind::BRACE_OPEN)
    parsingError("expected '{' after if condition");

  auto ThenBlock = parseBlock();
  if (!ThenBlock) return nullptr;

  // Then block should return a single value (last expression)
  unique_ptr<ExprAST> Then;
  if (ThenBlock->getBody().empty()) {
    Then = std::make_unique<BoolLiteralAST>(false);  // default
  } else if (ThenBlock->getBody().size() == 1) {
    // Move the single expression out
    Then = std::move(const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
        ThenBlock->getBody())[0]);
  } else {
    Then = std::move(ThenBlock);
  }

  // Optional else
  unique_ptr<ExprAST> Else;
  if (curTok.kind == TokenKind::ELSE) {
    getNextToken();  // eat 'else'

    // Check for else-if
    if (curTok.kind == TokenKind::IF) {
      Else = parseIfStatement();
      if (!Else) return nullptr;
    } else {
      // Require curly braces for else block
      if (curTok.kind != TokenKind::BRACE_OPEN)
        parsingError("expected '{' after else");

      auto ElseBlock = parseBlock();
      if (!ElseBlock) return nullptr;

      if (ElseBlock->getBody().empty()) {
        Else = std::make_unique<BoolLiteralAST>(false);
      } else if (ElseBlock->getBody().size() == 1) {
        Else = std::move(const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
            ElseBlock->getBody())[0]);
      } else {
        Else = std::move(ElseBlock);
      }
    }
  }
  // No else - Else remains nullptr

  return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then),
                                     std::move(Else));
}

// Parse match expression: match value { pattern => expr, ... }
unique_ptr<MatchExprAST> Parser::parseMatchExpression() {
  getNextToken();  // eat 'match'

  // Parse the discriminant expression
  auto discriminant = parseExpression();
  if (!discriminant) {
    parsingError("expected expression after 'match'");
    return nullptr;
  }

  // Expect opening brace
  if (curTok.kind != TokenKind::BRACE_OPEN) {
    parsingError("expected '{' after match expression");
    return nullptr;
  }
  getNextToken();  // eat '{'

  // Parse match arms
  std::vector<MatchArm> arms;
  while (curTok.kind != TokenKind::BRACE_CLOSE) {
    // Check for wildcard pattern: _
    bool isWildcard = (curTok.kind == TokenKind::UNDERSCORE);
    std::unique_ptr<ExprAST> pattern = nullptr;

    if (isWildcard) {
      getNextToken();  // eat '_'
    } else {
      // Parse pattern expression (for now, just literals and identifiers)
      pattern = parsePrimary();
      if (!pattern) {
        parsingError("expected pattern in match arm");
        return nullptr;
      }
    }

    // Expect =>
    if (curTok.kind != TokenKind::FAT_ARROW) {
      parsingError("expected '=>' after pattern in match arm");
      return nullptr;
    }
    getNextToken();  // eat '=>'

    // Parse body expression
    // Check if body is a block
    std::unique_ptr<ExprAST> body;
    if (curTok.kind == TokenKind::BRACE_OPEN) {
      body = parseBlock();
    } else {
      body = parseExpression();
    }
    if (!body) {
      parsingError("expected expression after '=>' in match arm");
      return nullptr;
    }

    arms.emplace_back(std::move(pattern), isWildcard, std::move(body));

    // Check for comma (optional before closing brace)
    if (curTok.kind == TokenKind::COMMA) {
      getNextToken();  // eat ','
    } else if (curTok.kind != TokenKind::BRACE_CLOSE) {
      parsingError("expected ',' or '}' in match expression");
      return nullptr;
    }
  }

  getNextToken();  // eat '}'

  if (arms.empty()) {
    parsingError("match expression must have at least one arm");
    return nullptr;
  }

  return std::make_unique<MatchExprAST>(std::move(discriminant),
                                        std::move(arms));
}

// Parse function: function name(args) returnType { body }
// or: function name<T, U>(args) returnType { body }
unique_ptr<FunctionAST> Parser::parseFunction() {
  getNextToken();  // eat 'function'

  // Allow both regular identifiers and intrinsic identifiers (e.g., __index__)
  if (curTok.kind != TokenKind::IDENTIFIER &&
      curTok.kind != TokenKind::INTRINSIC_IDENTIFIER)
    parsingError("Expected function name after 'function'");

  std::string funcName = curTok.getIdentifier().value();
  getNextToken();  // eat function name

  // Parse optional type parameters: function name<T, U>(...)
  std::vector<std::string> typeParameters;
  if (curTok.kind == TokenKind::LESS) {
    getNextToken();  // eat '<'

    // Parse comma-separated list of type parameter names
    while (curTok.kind == TokenKind::IDENTIFIER) {
      typeParameters.push_back(curTok.getIdentifier().value());
      getNextToken();  // eat type parameter name

      if (curTok.kind == TokenKind::COMMA) {
        getNextToken();  // eat ','
      } else {
        break;
      }
    }

    if (typeParameters.empty()) {
      parsingError("expected type parameter name after '<'");
    }

    if (curTok.kind != TokenKind::GREATER) {
      parsingError("expected '>' after type parameters");
    }
    getNextToken();  // eat '>'
  }

  auto result =
      parseFunctionLiteral(funcName, std::move(typeParameters), false);
  return unique_ptr<FunctionAST>(static_cast<FunctionAST*>(result.release()));
}

// Parse lambda: lambda (args) returnType { body }
unique_ptr<LambdaAST> Parser::parseLambda() {
  getNextToken();                                    // eat 'lambda'
  auto result = parseFunctionLiteral("", {}, true);  // anonymous function
  return unique_ptr<LambdaAST>(static_cast<LambdaAST*>(result.release()));
}

// Parse a function literal: (args) returnType { body }
unique_ptr<ExprAST> Parser::parseFunctionLiteral(
    const std::string& name, std::vector<std::string> typeParameters,
    bool isLambda) {
  if (curTok.kind != TokenKind::PAREN_OPEN)
    parsingError("Expected '(' in function literal");

  std::vector<std::pair<std::string, TypeAnnotation>> args;
  std::optional<std::string> variadicParamName;
  std::optional<TypeAnnotation> variadicConstraint;

  getNextToken();  // eat '('
  if (curTok.kind != TokenKind::PAREN_CLOSE) {
    while (curTok.kind == TokenKind::IDENTIFIER) {
      std::string argName = curTok.getIdentifier().value();
      getNextToken();  // eat identifier

      // Check for variadic parameter: args... or args...: _init_args<T>
      if (curTok.kind == TokenKind::ELLIPSIS) {
        variadicParamName = argName;
        getNextToken();  // eat '...'

        // Check for optional constraint: args...: _init_args<T>
        if (curTok.kind == TokenKind::COLON) {
          getNextToken();  // eat ':'
          variadicConstraint = parseTypeAnnotation();
        }

        // Variadic param must be last - break out of loop
        break;
      }

      // Type annotation is required: arg: type
      if (curTok.kind != TokenKind::COLON) {
        parsingError("Expected ':' and type annotation for argument '" +
                     argName + "'");
      }
      getNextToken();  // eat ':'
      auto argType = parseTypeAnnotation();

      args.emplace_back(std::move(argName), std::move(argType));

      if (curTok.kind == TokenKind::COMMA)
        getNextToken();
      else
        break;
    }
  }

  if (curTok.kind != TokenKind::PAREN_CLOSE)
    parsingError("Expected ')' in function literal");

  getNextToken();  // eat ')'

  // Check for return type (no arrow, type comes directly after parentheses)
  // Syntax: function foo(args) ReturnType, IError { ... }
  std::optional<TypeAnnotation> retType;
  if (curTok.kind != TokenKind::BRACE_OPEN) {
    retType = parseTypeAnnotation();

    // Check for error union: ", IError"
    if (curTok.kind == TokenKind::COMMA) {
      getNextToken();  // eat ','
      // Only accept 'IError' identifier
      bool isErrorType = false;
      if (curTok.kind == TokenKind::IDENTIFIER) {
        auto id = curTok.getIdentifier();
        isErrorType = id.has_value() && id.value() == "IError";
      }
      if (!isErrorType) {
        parsingError("expected 'IError' after ',' in return type");
      }
      getNextToken();  // eat 'IError'
      if (retType.has_value()) {
        retType->canError = true;
      }
    }
  }

  // Parse function body
  if (curTok.kind != TokenKind::BRACE_OPEN)
    parsingError("Expected '{' to start function body");

  auto body = parseBlock();
  if (!body) return nullptr;

  auto proto = std::make_unique<PrototypeAST>(
      name, std::move(args), std::move(retType), std::move(typeParameters),
      std::move(variadicParamName), std::move(variadicConstraint));
  if (isLambda) {
    return std::make_unique<LambdaAST>(std::move(proto), std::move(body));
  } else {
    return std::make_unique<FunctionAST>(std::move(proto), std::move(body));
  }
}

// Internal helper that parses var declaration without consuming trailing
// semicolon
unique_ptr<VariableCreationAST> Parser::parseVarDeclaration() {
  getNextToken();  // eat the 'var'

  // At least one variable name is required
  if (curTok.kind != TokenKind::IDENTIFIER) {
    parsingError("expected identifier after 'var'");
  }

  std::string name = std::get<std::string>(curTok.value);
  getNextToken();  // eat identifier

  // Optional type annotation: var x: i32 = ...
  std::optional<TypeAnnotation> typeAnnot;
  if (curTok.kind == TokenKind::COLON) {
    getNextToken();  // eat ':'
    typeAnnot = parseTypeAnnotation();
  }

  if (curTok.kind != TokenKind::EQUAL) {
    parsingError("expected '=' after variable declaration");
  }

  getNextToken();  // eat '='

  // parseExpression handles function/lambda keywords automatically
  auto value = parseExpression();
  if (!value) {
    parsingError("variable initialization expression expected");
  }

  return std::make_unique<VariableCreationAST>(name, std::move(value),
                                               std::move(typeAnnot));
}

unique_ptr<VariableCreationAST> Parser::parseVarStatement() {
  auto decl = parseVarDeclaration();

  if (curTok.kind != TokenKind::SEMI_COLON) {
    parsingError("expected ';' after variable declaration");
  }
  getNextToken();  // eat ';'

  return decl;
}

unique_ptr<ReferenceCreationAST> Parser::parseRefStatement() {
  Position refLoc = curTok.start;  // Capture location of 'ref' keyword
  getNextToken();                  // eat the 'ref'

  // Check for 'const' modifier: ref const x = y
  bool isMutable = true;
  if (curTok.kind == TokenKind::IDENTIFIER &&
      std::get<std::string>(curTok.value) == "const") {
    isMutable = false;
    getNextToken();  // eat 'const'
  }

  // Require identifier
  if (curTok.kind != TokenKind::IDENTIFIER) {
    parsingError("expected identifier after 'ref'");
  }

  std::string name = std::get<std::string>(curTok.value);
  getNextToken();  // eat identifier

  if (curTok.kind != TokenKind::EQUAL) {
    parsingError("expected '=' after reference name");
  }

  getNextToken();  // eat '='

  // Parse the target expression (must be an lvalue - variable reference)
  auto target = parseExpression();
  if (!target) {
    parsingError("reference target expression expected");
  }

  if (curTok.kind != TokenKind::SEMI_COLON) {
    parsingError("expected ';' after reference declaration");
  }
  getNextToken();  // eat ';'

  return std::make_unique<ReferenceCreationAST>(name, std::move(target),
                                                isMutable, refLoc);
}

unique_ptr<ExprAST> Parser::parseIdentifierExpr() {
  std::string idName = std::get<std::string>(curTok.value);

  getNextToken();  // eat identifier

  // Check for pack expansion: args...
  if (curTok.kind == TokenKind::ELLIPSIS) {
    getNextToken();  // eat '...'
    return std::make_unique<PackExpansionAST>(std::move(idName));
  }

  // Note: We don't parse dot-based qualified names (like sun.Vec) here.
  // In expression context, dots are member access (handled by postfix parsing).
  // For module-qualified types, use type annotations: var x: sun.Vec<T>
  // For module symbols, use: using sun; then refer to them unqualified

  // Check for generic function call: create<Type>(args...)
  if (curTok.kind == TokenKind::LESS) {
    // Could be a generic call or a comparison - use backtracking to decide
    // Save parser state for backtracking
    Token savedCurTok = curTok;
    auto savedLexerPos = lexer.getPosition();
    auto savedTokenStack = tokenStack;

    getNextToken();  // eat '<'

    // Try to parse type arguments (one or more separated by commas)
    bool isGenericCall = false;
    if (isTypeToken(curTok.kind) || curTok.kind == TokenKind::IDENTIFIER) {
      std::vector<std::unique_ptr<TypeAnnotation>> typeArgs;
      auto typeArg = parseTypeAnnotation();
      typeArgs.push_back(std::make_unique<TypeAnnotation>(std::move(typeArg)));

      // Parse additional type arguments separated by commas
      while (curTok.kind == TokenKind::COMMA) {
        getNextToken();  // eat ','
        if (!isTypeToken(curTok.kind) && curTok.kind != TokenKind::IDENTIFIER) {
          break;  // Not a type arg after comma, not a generic call
        }
        auto nextTypeArg = parseTypeAnnotation();
        typeArgs.push_back(
            std::make_unique<TypeAnnotation>(std::move(nextTypeArg)));
      }

      if (curTok.kind == TokenKind::GREATER) {
        getNextToken();  // eat '>'

        // Must be followed by '(' for a function call
        if (curTok.kind == TokenKind::PAREN_OPEN) {
          getNextToken();  // eat '('

          // Parse arguments
          std::vector<std::unique_ptr<ExprAST>> args;
          if (curTok.kind != TokenKind::PAREN_CLOSE) {
            while (true) {
              auto arg = parseExpression();
              if (!arg) return nullptr;
              args.push_back(std::move(arg));

              if (curTok.kind == TokenKind::PAREN_CLOSE) break;
              if (curTok.kind != TokenKind::COMMA) {
                parsingError(
                    "expected ')' or ',' in generic function call arguments");
                return nullptr;
              }
              getNextToken();  // eat ','
            }
          }
          getNextToken();  // eat ')'

          // Create a GenericCallAST node
          return std::make_unique<GenericCallAST>(
              std::move(idName), std::move(typeArgs), std::move(args));
        }
      }
    }

    // Not a generic call - backtrack to before '<'
    curTok = savedCurTok;
    lexer.setPosition(savedLexerPos);
    tokenStack = savedTokenStack;
  }

  // Just return a variable reference - call handling is done in postfix
  // parsing This allows for first-class function support where variables can
  // hold functions
  return std::make_unique<VariableReferenceAST>(idName);
}

unique_ptr<ExprAST> Parser::parsePrimary() {
  // Handle prefix operators: + and -
  if (curTok.kind == TokenKind::PLUS || curTok.kind == TokenKind::MINUS) {
    Token opTok = curTok;  // Save the operator token (for kind and position)
    getNextToken();        // eat + or -

    auto operand = parsePrimary();  // Recursively parse the operand
    if (!operand) return nullptr;

    if (opTok.kind == TokenKind::PLUS) {
      // Unary + is a no-op: just return the operand
      return operand;
    } else  // MINUS
    {
      // Unary minus: transform into 0 - operand
      // Create zero with matching type to avoid type mismatch errors
      std::unique_ptr<NumberExprAST> zero;
      if (operand->getType() == ASTNodeType::NUMBER) {
        auto* numExpr = static_cast<NumberExprAST*>(operand.get());
        if (numExpr->isInteger()) {
          zero = std::make_unique<NumberExprAST>(static_cast<int64_t>(0));
        } else {
          zero = std::make_unique<NumberExprAST>(0.0);
        }
      } else {
        // For non-literal operands, default to integer zero
        // (type checking will handle any coercion needed)
        zero = std::make_unique<NumberExprAST>(static_cast<int64_t>(0));
      }

      return std::make_unique<BinaryExprAST>(opTok, std::move(zero),
                                             std::move(operand));
    }
  }

  unique_ptr<ExprAST> base;

  // Regular primaries
  switch (curTok.kind) {
    default:
      parsingError("unknown token when expecting an expression");
    case TokenKind::IDENTIFIER:
    case TokenKind::INTRINSIC_IDENTIFIER:
      base = parseIdentifierExpr();
      break;
    case TokenKind::IF:
      base = parseIfStatement();
      break;
    case TokenKind::MATCH:
      base = parseMatchExpression();
      break;
    case TokenKind::INTEGER:
    case TokenKind::FLOAT:
      base = parseNumberExpr();
      break;
    case TokenKind::STRING:
      base = parseStringLiteral();
      break;
    case TokenKind::PAREN_OPEN:
      base = parseParenExpr();
      break;
    case TokenKind::FOR:
      base = parseForLoop();
      break;
    case TokenKind::WHILE:
      base = parseWhileLoop();
      break;
    case TokenKind::VAR:
      base = parseVarStatement();
      break;
    case TokenKind::THIS:
      base = std::make_unique<ThisExprAST>();
      getNextToken();  // eat 'this'
      break;
    case TokenKind::FUNCTION:
      parsingError(
          "'function' cannot be used as an expression; use 'lambda' instead");
      break;
    case TokenKind::LAMBDA:
      base = parseLambda();
      break;
    case TokenKind::TRY:
      // try { ... } catch (e: IError) { ... } syntax
      getNextToken();  // eat 'try'
      if (curTok.kind != TokenKind::BRACE_OPEN) {
        parsingError("expected '{' after 'try'");
        return nullptr;
      }
      base = parseTryCatch();
      break;
    case TokenKind::THROW:
      base = parseThrow();
      break;
    case TokenKind::NULL_LITERAL:
      base = std::make_unique<NullLiteralAST>();
      getNextToken();  // eat 'null'
      break;
    case TokenKind::TRUE_LITERAL:
      base = std::make_unique<BoolLiteralAST>(true);
      getNextToken();  // eat 'true'
      break;
    case TokenKind::FALSE_LITERAL:
      base = std::make_unique<BoolLiteralAST>(false);
      getNextToken();  // eat 'false'
      break;
    case TokenKind::BRACKET_OPEN:
      base = parseArrayLiteral();
      break;
  }

  if (!base) return nullptr;

  // Parse postfix expressions (array indexing)
  return parsePostfixExpr(std::move(base));
}

// Parse postfix expressions like obj.field or func(args) or arr[i, j]
unique_ptr<ExprAST> Parser::parsePostfixExpr(unique_ptr<ExprAST> base) {
  while (curTok.kind == TokenKind::PAREN_OPEN ||
         curTok.kind == TokenKind::DOT ||
         curTok.kind == TokenKind::BRACKET_OPEN) {
    if (curTok.kind == TokenKind::BRACKET_OPEN) {
      // Array indexing with optional slices: arr[i], arr[i, j], arr[1:10, :5]
      getNextToken();  // eat '['

      std::vector<std::unique_ptr<SliceExprAST>> indices;
      if (curTok.kind != TokenKind::BRACKET_CLOSE) {
        while (true) {
          // Parse slice component: either single index or range slice
          // Cases: expr, :, :expr, expr:, expr:expr
          std::unique_ptr<ExprAST> start = nullptr;
          std::unique_ptr<ExprAST> end = nullptr;
          bool isRange = false;

          if (curTok.kind == TokenKind::COLON) {
            // Starts with colon: [:] or [:expr]
            isRange = true;
            getNextToken();  // eat ':'

            // Check if there's an end expression
            if (curTok.kind != TokenKind::COMMA &&
                curTok.kind != TokenKind::BRACKET_CLOSE) {
              end = parseExpression();
              if (!end) return nullptr;
            }
          } else {
            // Starts with expression: [expr] or [expr:] or [expr:expr]
            start = parseExpression();
            if (!start) return nullptr;

            if (curTok.kind == TokenKind::COLON) {
              // It's a range slice
              isRange = true;
              getNextToken();  // eat ':'

              // Check if there's an end expression
              if (curTok.kind != TokenKind::COMMA &&
                  curTok.kind != TokenKind::BRACKET_CLOSE) {
                end = parseExpression();
                if (!end) return nullptr;
              }
            }
          }

          // Create the appropriate SliceExprAST
          if (isRange) {
            indices.push_back(std::make_unique<SliceExprAST>(
                std::move(start), std::move(end), true));
          } else {
            indices.push_back(std::make_unique<SliceExprAST>(std::move(start)));
          }

          if (curTok.kind == TokenKind::BRACKET_CLOSE) break;

          if (curTok.kind != TokenKind::COMMA) {
            parsingError("expected ']' or ',' in array index");
            return nullptr;
          }
          getNextToken();  // eat ','
        }
      }
      getNextToken();  // eat ']'

      if (indices.empty()) {
        parsingError("array index cannot be empty");
        return nullptr;
      }

      base = std::make_unique<IndexAST>(std::move(base), std::move(indices));
    } else if (curTok.kind == TokenKind::DOT) {
      getNextToken();  // eat '.'

      if (curTok.kind != TokenKind::IDENTIFIER) {
        parsingError("expected member name after '.'");
        return nullptr;
      }

      std::string memberName = curTok.getIdentifier().value();
      getNextToken();  // eat identifier

      // Parse optional generic type arguments: .method<Type>()
      // Use backtracking to distinguish from comparison (e.g., this.x < 5)
      std::vector<std::unique_ptr<TypeAnnotation>> typeArgs;
      if (curTok.kind == TokenKind::LESS) {
        // Save parser state for backtracking
        Token savedCurTok = curTok;
        auto savedLexerPos = lexer.getPosition();
        auto savedTokenStack = tokenStack;

        getNextToken();  // eat '<'

        // Try to parse type arguments
        bool isGenericMethod = false;
        if (isTypeToken(curTok.kind) || curTok.kind == TokenKind::IDENTIFIER) {
          std::vector<std::unique_ptr<TypeAnnotation>> tempTypeArgs;
          tempTypeArgs.push_back(
              std::make_unique<TypeAnnotation>(parseTypeAnnotation()));

          // Parse additional type arguments separated by commas
          while (curTok.kind == TokenKind::COMMA) {
            getNextToken();  // eat ','
            if (!isTypeToken(curTok.kind) &&
                curTok.kind != TokenKind::IDENTIFIER) {
              break;  // Not a type arg after comma
            }
            tempTypeArgs.push_back(
                std::make_unique<TypeAnnotation>(parseTypeAnnotation()));
          }

          if (curTok.kind == TokenKind::GREATER) {
            getNextToken();  // eat '>'

            // Must be followed by '(' for a method call
            if (curTok.kind == TokenKind::PAREN_OPEN) {
              isGenericMethod = true;
              typeArgs = std::move(tempTypeArgs);
            }
          }
        }

        if (!isGenericMethod) {
          // Not a generic method call - backtrack
          curTok = savedCurTok;
          lexer.setPosition(savedLexerPos);
          tokenStack = savedTokenStack;
        }
      }

      base = std::make_unique<MemberAccessAST>(
          std::move(base), std::move(memberName), std::move(typeArgs));
    } else {
      // Function call as postfix: base(args)
      // This handles indirect calls through function pointer variables
      getNextToken();  // eat '('

      std::vector<std::unique_ptr<ExprAST>> args;
      if (curTok.kind != TokenKind::PAREN_CLOSE) {
        while (true) {
          if (auto arg = parseExpression())
            args.push_back(std::move(arg));
          else
            return nullptr;

          if (curTok.kind == TokenKind::PAREN_CLOSE) break;

          if (curTok.kind != TokenKind::COMMA)
            parsingError("Expected ')' or ',' in argument list");

          getNextToken();  // eat ','
        }
      }
      getNextToken();  // eat ')'

      // Create a unified call expression (callee is an expression)
      base = std::make_unique<CallExprAST>(std::move(base), std::move(args));
    }
  }

  return base;
}

// Check if a token is a type keyword
bool Parser::isTypeToken(TokenKind kind) {
  switch (kind) {
    case TokenKind::TYPE_I8:
    case TokenKind::TYPE_I16:
    case TokenKind::TYPE_I32:
    case TokenKind::TYPE_I64:
    case TokenKind::TYPE_U8:
    case TokenKind::TYPE_U16:
    case TokenKind::TYPE_U32:
    case TokenKind::TYPE_U64:
    case TokenKind::TYPE_F32:
    case TokenKind::TYPE_F64:
    case TokenKind::TYPE_BOOL:
    case TokenKind::TYPE_VOID:
    case TokenKind::PTR:
    case TokenKind::RAW_PTR:
    case TokenKind::STATIC_PTR:
    case TokenKind::IDENTIFIER:            // User-defined class types
    case TokenKind::INTRINSIC_IDENTIFIER:  // Intrinsic types like _init_args
      return true;
    default:
      return false;
  }
}

// Parse type annotation: i32, f64, matrix(i32, 2, 3), _(param_types)
// return_type (function), (param_types) return_type (lambda)
TypeAnnotation Parser::parseTypeAnnotation() {
  TypeAnnotation type;

  // Check for function type: _(param_types) return_type (named function)
  if (curTok.kind == TokenKind::UNDERSCORE) {
    type.baseName = "fn";
    getNextToken();  // eat '_'

    if (curTok.kind != TokenKind::PAREN_OPEN) {
      parsingError("expected '(' after '_' in function type");
      return type;
    }
    getNextToken();  // eat '('

    // Parse parameter types
    if (curTok.kind != TokenKind::PAREN_CLOSE) {
      while (true) {
        auto paramType = parseTypeAnnotation();
        type.paramTypes.push_back(
            std::make_unique<TypeAnnotation>(std::move(paramType)));

        if (curTok.kind == TokenKind::COMMA) {
          getNextToken();  // eat ','
        } else {
          break;
        }
      }
    }

    if (curTok.kind != TokenKind::PAREN_CLOSE) {
      parsingError("expected ')' in function type");
      return type;
    }
    getNextToken();  // eat ')'

    type.returnType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    return type;
  }

  // Check for lambda type: (param_types) return_type (anonymous function)
  if (curTok.kind == TokenKind::PAREN_OPEN) {
    type.baseName = "lambda";
    getNextToken();  // eat '('

    // Parse parameter types
    if (curTok.kind != TokenKind::PAREN_CLOSE) {
      while (true) {
        auto paramType = parseTypeAnnotation();
        type.paramTypes.push_back(
            std::make_unique<TypeAnnotation>(std::move(paramType)));

        if (curTok.kind == TokenKind::COMMA) {
          getNextToken();  // eat ','
        } else {
          break;
        }
      }
    }

    if (curTok.kind != TokenKind::PAREN_CLOSE) {
      parsingError("expected ')' in lambda type");
      return type;
    }
    getNextToken();  // eat ')'

    type.returnType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    return type;
  }

  if (curTok.kind == TokenKind::PTR) {
    // ptr<elementType> - owning pointer with unique_ptr semantics
    type.baseName = "ptr";
    getNextToken();  // eat 'ptr'

    if (curTok.kind != TokenKind::LESS)
      parsingError("expected '<' after 'ptr'");
    getNextToken();  // eat '<'

    // Parse pointee type
    type.elementType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    if (curTok.kind != TokenKind::GREATER)
      parsingError("expected '>' after ptr type");
    getNextToken();  // eat '>'

    return type;
  }

  if (curTok.kind == TokenKind::RAW_PTR) {
    // raw_ptr<elementType> - non-owning pointer for C interop
    type.baseName = "raw_ptr";
    getNextToken();  // eat 'raw_ptr'

    if (curTok.kind != TokenKind::LESS)
      parsingError("expected '<' after 'raw_ptr'");
    getNextToken();  // eat '<'

    // Parse pointee type
    type.elementType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    if (curTok.kind != TokenKind::GREATER)
      parsingError("expected '>' after raw_ptr type");
    getNextToken();  // eat '>'

    return type;
  }

  if (curTok.kind == TokenKind::STATIC_PTR) {
    // static_ptr<elementType> - pointer to immortal static data
    type.baseName = "static_ptr";
    getNextToken();  // eat 'static_ptr'

    if (curTok.kind != TokenKind::LESS)
      parsingError("expected '<' after 'static_ptr'");
    getNextToken();  // eat '<'

    // Parse pointee type
    type.elementType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    if (curTok.kind != TokenKind::GREATER)
      parsingError("expected '>' after static_ptr type");
    getNextToken();  // eat '>'

    return type;
  }

  if (curTok.kind == TokenKind::REF) {
    // ref type - reference type with implicit dereferencing
    type.baseName = "ref";
    getNextToken();  // eat 'ref'

    // Parse referenced type directly (no parentheses)
    type.elementType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    return type;
  }

  if (curTok.kind == TokenKind::ARRAY) {
    // array<T, N> or array<T, M, N> - fixed-size array type
    type.baseName = "array";
    getNextToken();  // eat 'array'

    if (curTok.kind != TokenKind::LESS) {
      parsingError("expected '<' after 'array'");
      return type;
    }
    getNextToken();  // eat '<'

    // Parse element type
    type.elementType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    // Parse dimensions (comma-separated integers)
    while (curTok.kind == TokenKind::COMMA) {
      getNextToken();  // eat ','

      if (curTok.kind != TokenKind::INTEGER) {
        parsingError("expected integer dimension in array type");
        return type;
      }

      type.arrayDimensions.push_back(
          static_cast<size_t>(curTok.getInteger().value()));
      getNextToken();  // eat integer
    }

    if (curTok.kind != TokenKind::GREATER) {
      parsingError("expected '>' after array type");
      return type;
    }
    getNextToken();  // eat '>'

    // Note: empty arrayDimensions is allowed - means "unsized" array
    // that accepts any array<T, ...> of the same element type

    return type;
  }

  {
    // Primitive type
    switch (curTok.kind) {
      case TokenKind::TYPE_I8:
        type.baseName = "i8";
        break;
      case TokenKind::TYPE_I16:
        type.baseName = "i16";
        break;
      case TokenKind::TYPE_I32:
        type.baseName = "i32";
        break;
      case TokenKind::TYPE_I64:
        type.baseName = "i64";
        break;
      case TokenKind::TYPE_U8:
        type.baseName = "u8";
        break;
      case TokenKind::TYPE_U16:
        type.baseName = "u16";
        break;
      case TokenKind::TYPE_U32:
        type.baseName = "u32";
        break;
      case TokenKind::TYPE_U64:
        type.baseName = "u64";
        break;
      case TokenKind::TYPE_F32:
        type.baseName = "f32";
        break;
      case TokenKind::TYPE_F64:
        type.baseName = "f64";
        break;
      case TokenKind::TYPE_BOOL:
        type.baseName = "bool";
        break;
      case TokenKind::TYPE_VOID:
        type.baseName = "void";
        break;
      case TokenKind::IDENTIFIER:
      case TokenKind::INTRINSIC_IDENTIFIER:
        // User-defined type (class name) or intrinsic type constraint (like
        // _init_args<T>)
        type.baseName = curTok.getIdentifier().value();
        break;
      default:
        parsingError("expected type name");
        type.baseName = "f64";  // default fallback
        return type;
    }
    getNextToken();  // eat type name

    // Check for qualified type path: Module.Type
    // (only for identifier-based types)
    while (curTok.kind == TokenKind::DOT) {
      getNextToken();  // eat '.'

      if (curTok.kind != TokenKind::IDENTIFIER) {
        parsingError("expected identifier after '.'");
        return type;
      }

      // Append to baseName with dot separator
      type.baseName += ".";
      type.baseName += curTok.getIdentifier().value();
      getNextToken();  // eat identifier
    }

    // Check for generic type arguments: ClassName<T, U, ...>
    if (curTok.kind == TokenKind::LESS) {
      getNextToken();  // eat '<'

      // Parse comma-separated list of type arguments
      while (true) {
        auto typeArg = parseTypeAnnotation();
        type.typeArguments.push_back(
            std::make_unique<TypeAnnotation>(std::move(typeArg)));

        if (curTok.kind == TokenKind::COMMA) {
          getNextToken();  // eat ','
        } else {
          break;
        }
      }

      if (curTok.kind != TokenKind::GREATER) {
        parsingError("expected '>' after generic type arguments");
        return type;
      }
      getNextToken();  // eat '>'
    }
  }

  return type;
}

unique_ptr<ExprAST> Parser::parseAssignmentOrExpression() {
  auto idToken = curTok;
  std::string idName = curTok.getIdentifier().value();

  getNextToken();  // eat identifier

  // Check for simple variable assignment: x = ...
  if (curTok.kind == TokenKind::EQUAL) {
    getNextToken();  // eat '='
    auto expr = parseExpression();
    if (!expr) return nullptr;

    if (curTok.kind == TokenKind::SEMI_COLON)
      getNextToken();
    else
      parsingError("expected ';' after variable assignment");

    return std::make_unique<VariableAssignmentAST>(idName, std::move(expr));
  }

  // Not an assignment, parse as expression
  // Put back the identifier token for expression parsing
  pushToken(idToken);
  auto expr = parseExpression();

  // Check for indexed assignment: x[i] = value or x[i, j] = value
  if (curTok.kind == TokenKind::EQUAL &&
      expr->getType() == ASTNodeType::INDEX) {
    getNextToken();  // eat '='
    auto value = parseExpression();
    if (!value) return nullptr;

    if (curTok.kind == TokenKind::SEMI_COLON)
      getNextToken();
    else
      parsingError("expected ';' after indexed assignment");

    return std::make_unique<IndexedAssignmentAST>(std::move(expr),
                                                  std::move(value));
  }

  if (curTok.kind == TokenKind::SEMI_COLON)
    getNextToken();
  else
    parsingError("expected ';' after expression statement");
  return expr;
}

unique_ptr<ExprAST> Parser::parseExpression() {
  // Only lambda can be used as an expression, not function
  if (curTok.kind == TokenKind::FUNCTION) {
    parsingError(
        "'function' cannot be used as an expression; use 'lambda' instead");
  }
  if (curTok.kind == TokenKind::LAMBDA) {
    return parseLambda();
  }

  auto lhs = parsePrimary();
  if (!lhs) return nullptr;

  return parseBinOpRhs(0, std::move(lhs));
}

std::unique_ptr<ExprAST> Parser::parseBinOpRhs(int exprPrec,
                                               std::unique_ptr<ExprAST> lhs) {
  while (true) {
    if (curTok.precedence < exprPrec) return lhs;

    Token binOp = curTok;
    getNextToken();  // eat binop

    auto rhs = parsePrimary();
    if (!rhs) return nullptr;

    if (binOp.precedence < curTok.precedence) {
      rhs = parseBinOpRhs(binOp.precedence + 1, std::move(rhs));
      if (!rhs) return nullptr;
    }

    lhs =
        std::make_unique<BinaryExprAST>(binOp, std::move(lhs), std::move(rhs));
  }
}

std::unique_ptr<PrototypeAST> Parser::parsePrototype() {
  std::string fnName;

  // Allow both regular identifiers and intrinsic identifiers (e.g., __index__)
  if (curTok.kind != TokenKind::IDENTIFIER &&
      curTok.kind != TokenKind::INTRINSIC_IDENTIFIER) {
    parsingError("Expected function name in prototype");
  }

  fnName = curTok.getIdentifier().value();
  getNextToken();

  // Parse optional type parameters: function name<T, U>(...)
  std::vector<std::string> typeParameters;
  if (curTok.kind == TokenKind::LESS) {
    getNextToken();  // eat '<'

    // Parse comma-separated list of type parameter names
    while (curTok.kind == TokenKind::IDENTIFIER) {
      typeParameters.push_back(curTok.getIdentifier().value());
      getNextToken();  // eat type parameter name

      if (curTok.kind == TokenKind::COMMA) {
        getNextToken();  // eat ','
      } else {
        break;
      }
    }

    if (typeParameters.empty()) {
      parsingError("expected type parameter name after '<'");
    }

    if (curTok.kind != TokenKind::GREATER) {
      parsingError("expected '>' after type parameters");
    }
    getNextToken();  // eat '>'
  }

  if (curTok.kind != TokenKind::PAREN_OPEN)
    parsingError("Expected '(' in prototype");

  std::vector<std::pair<std::string, TypeAnnotation>> args;

  getNextToken();  // eat '('
  if (curTok.kind == TokenKind::PAREN_CLOSE) {
    getNextToken();  // eat ')'
    // Check for return type (only if followed by a type token)
    std::optional<TypeAnnotation> retType;
    if (curTok.kind != TokenKind::BRACE_OPEN &&
        curTok.kind != TokenKind::SEMI_COLON &&
        (isTypeToken(curTok.kind) || curTok.kind == TokenKind::PAREN_OPEN ||
         curTok.kind == TokenKind::UNDERSCORE)) {
      retType = parseTypeAnnotation();
    }
    return std::make_unique<PrototypeAST>(
        fnName, std::move(args), std::move(retType), std::move(typeParameters));
  }

  std::optional<std::string> variadicParamName;
  std::optional<TypeAnnotation> variadicConstraint;

  while (curTok.kind == TokenKind::IDENTIFIER) {
    std::string argName = curTok.getIdentifier().value();
    getNextToken();  // eat identifier

    // Check for variadic parameter: args... or args...: _init_args<T>
    if (curTok.kind == TokenKind::ELLIPSIS) {
      variadicParamName = argName;
      getNextToken();  // eat '...'

      // Check for optional constraint: args...: _init_args<T>
      if (curTok.kind == TokenKind::COLON) {
        getNextToken();  // eat ':'
        variadicConstraint = parseTypeAnnotation();
      }

      // Variadic param must be last - break out of loop
      break;
    }

    // Type annotation is required: arg: type
    if (curTok.kind != TokenKind::COLON) {
      parsingError("Expected ':' and type annotation for argument '" + argName +
                   "'");
    }
    getNextToken();  // eat ':'
    auto argType = parseTypeAnnotation();

    args.emplace_back(std::move(argName), std::move(argType));

    if (curTok.kind == TokenKind::COMMA)
      getNextToken();
    else
      break;
  }

  if (curTok.kind != TokenKind::PAREN_CLOSE)
    parsingError("Expected ')' in prototype");

  // success.
  getNextToken();  // eat ')'.

  // Check for return type (only if followed by a type token, not semicolon)
  // Type can start with: type keywords, '(' for lambda type, '_' for fn type
  std::optional<TypeAnnotation> retType;
  if (curTok.kind != TokenKind::BRACE_OPEN &&
      curTok.kind != TokenKind::SEMI_COLON &&
      (isTypeToken(curTok.kind) || curTok.kind == TokenKind::PAREN_OPEN ||
       curTok.kind == TokenKind::UNDERSCORE)) {
    retType = parseTypeAnnotation();
  }

  return std::make_unique<PrototypeAST>(
      fnName, std::move(args), std::move(retType), std::move(typeParameters),
      std::move(variadicParamName), std::move(variadicConstraint));
}

unique_ptr<BlockExprAST> Parser::parseBlock() {
  std::vector<unique_ptr<ExprAST>> body;

  if (curTok.kind != TokenKind::BRACE_OPEN) parsingError("expected '{'");
  getNextToken();  // eat {

  while (curTok.kind != TokenKind::BRACE_CLOSE &&
         curTok.kind != TokenKind::TOK_EOF) {
    if (auto stmt = parseStatement()) {
      body.push_back(std::move(stmt));
    }
    // No error recovery needed - parseStatement handles its own token
    // consumption
  }

  if (curTok.kind != TokenKind::BRACE_CLOSE)
    parsingError("expected '}' at end of block");

  getNextToken();  // eat }

  return std::make_unique<BlockExprAST>(std::move(body));
}

unique_ptr<BlockExprAST> Parser::parseProgram() {
  std::vector<unique_ptr<ExprAST>> body;

  while (curTok.kind != TokenKind::BRACE_CLOSE &&
         curTok.kind != TokenKind::TOK_EOF) {
    if (auto stmt = parseStatement()) {
      body.push_back(std::move(stmt));
    }
    // No error recovery needed - parseStatement handles its own token
    // consumption
  }

  return std::make_unique<BlockExprAST>(std::move(body));
}

unique_ptr<ExprAST> Parser::parseStatement() {
  switch (curTok.kind) {
    case TokenKind::IMPORT:
      return parseImportStatement();

    case TokenKind::DECLARE:
      return parseDeclareStatement();

    case TokenKind::MODULE:
    case TokenKind::NAMESPACE:
      return parseNamespaceDecl();

    case TokenKind::USING:
      return parseUsingStatement();

    case TokenKind::CLASS: {
      auto classDef = parseClassDefinition();
      while (curTok.kind == TokenKind::SEMI_COLON)
        getNextToken();  // optional semicolons
      return classDef;
    }

    case TokenKind::INTERFACE: {
      auto interfaceDef = parseInterfaceDefinition();
      while (curTok.kind == TokenKind::SEMI_COLON)
        getNextToken();  // optional semicolons
      return interfaceDef;
    }

    case TokenKind::ENUM: {
      auto enumDef = parseEnumDefinition();
      while (curTok.kind == TokenKind::SEMI_COLON)
        getNextToken();  // optional semicolons
      return enumDef;
    }

    case TokenKind::VAR:
      return parseVarStatement();  // returns VarDeclExprAST or similar

    case TokenKind::REF:
      return parseRefStatement();  // returns ReferenceCreationAST

    case TokenKind::IF:
      return parseIfStatement();  // returns IfStmtAST (different from
                                  // if-expression!)
    case TokenKind::FOR:
      return parseForLoop();  // returns ForStmtAST
    case TokenKind::WHILE:
      return parseWhileLoop();  // returns WhileExprAST
    case TokenKind::BREAK:
      return parseBreak();  // returns BreakAST
    case TokenKind::CONTINUE:
      return parseContinue();  // returns ContinueAST
    case TokenKind::EXTERN: {
      // External function declaration: extern function name(args) ret;
      auto proto = parseExtern();
      if (curTok.kind == TokenKind::SEMI_COLON)
        getNextToken();  // eat optional semicolon
      // Wrap prototype in a FunctionAST with no body (nullptr)
      return std::make_unique<FunctionAST>(std::move(proto), nullptr);
    }
    case TokenKind::FUNCTION: {
      // Function definitions don't need trailing semicolons
      auto func = parseFunction();
      while (curTok.kind == TokenKind::SEMI_COLON)
        getNextToken();  // optional semicolons
      return func;
    }
    case TokenKind::IDENTIFIER: {
      return parseAssignmentOrExpression();
    }
    case TokenKind::THIS: {
      // Handle this.field = value; or this.method(...);
      auto thisExpr = std::make_unique<ThisExprAST>();
      getNextToken();  // eat 'this'

      // Must be followed by '.'
      if (curTok.kind != TokenKind::DOT) {
        parsingError("expected '.' after 'this'");
        return nullptr;
      }

      // Parse postfix expressions (member accesses, method calls)
      auto lhs = parsePostfixExpr(std::move(thisExpr));
      if (!lhs) return nullptr;

      // Check for assignment: this.field = value
      if (curTok.kind == TokenKind::EQUAL &&
          lhs->getType() == ASTNodeType::MEMBER_ACCESS) {
        getNextToken();  // eat '='
        auto value = parseExpression();
        if (!value) return nullptr;

        // Extract object and member from MemberAccessAST
        auto* memberAccess = static_cast<MemberAccessAST*>(lhs.get());

        // We need to move the object out - create a new AST
        // This is a bit awkward due to unique_ptr ownership
        // Create MemberAssignmentAST with the member info
        std::string memberName = memberAccess->getMemberName();

        // The object in memberAccess is 'this' or could be a chain
        // We need to reconstruct it - for now, handle simple this.field case
        auto assignExpr = std::make_unique<MemberAssignmentAST>(
            std::make_unique<ThisExprAST>(), std::move(memberName),
            std::move(value));

        if (curTok.kind == TokenKind::SEMI_COLON)
          getNextToken();
        else
          parsingError("expected ';' after member assignment");
        return assignExpr;
      }

      // Not an assignment - expression statement (like method call)
      if (curTok.kind == TokenKind::SEMI_COLON)
        getNextToken();
      else
        parsingError("expected ';' after expression statement");
      return lhs;
    }
    case TokenKind::RETURN:
      // parse return <expr>; or return;
      getNextToken();  // eat 'return'
      {
        std::unique_ptr<ExprAST> expr;
        // Check if there's an expression after return (not just semicolon)
        if (curTok.kind != TokenKind::SEMI_COLON) {
          expr = parseExpression();
          if (!expr) return nullptr;
        }

        if (curTok.kind == TokenKind::SEMI_COLON)
          getNextToken();
        else
          parsingError("expected ';' after return statement");

        return std::make_unique<ReturnExprAST>(std::move(expr));
      }
    case TokenKind::TRY: {
      // try { ... } catch (e: IError) { ... } syntax
      getNextToken();  // eat 'try'
      std::unique_ptr<ExprAST> tryExpr;
      if (curTok.kind != TokenKind::BRACE_OPEN) {
        parsingError("expected '{' after 'try'");
        return nullptr;
      }
      tryExpr = parseTryCatch();
      while (curTok.kind == TokenKind::SEMI_COLON)
        getNextToken();  // optional semicolons
      return tryExpr;
    }
    case TokenKind::THROW: {
      // Throw statement: throw <expr>;
      auto throwExpr = parseThrow();
      if (curTok.kind == TokenKind::SEMI_COLON)
        getNextToken();
      else
        parsingError("expected ';' after throw statement");
      return throwExpr;
    }
    case TokenKind::SEMI_COLON:
      getNextToken();  // ignore empty statement
      return nullptr;
    default: {
      // expression statement
      auto expr = parseExpression();
      if (curTok.kind == TokenKind::SEMI_COLON) {
        getNextToken();
        return expr;
      }
      // Many languages warn here: "missing semicolon" or make it optional in
      // some contexts
      parsingError("expected ';' after expression statement");
    }
  }
}

unique_ptr<ExprAST> Parser::parseForLoop() {
  getNextToken();  // eat 'for'

  if (curTok.kind != TokenKind::PAREN_OPEN)
    parsingError("Expected '(' after 'for'");
  getNextToken();  // eat '('

  // Check for for-in loop: for (var x: T in iterable) { ... }
  // We need to distinguish from for (var i: i32 = 0; ...; ...) { ... }
  if (curTok.kind == TokenKind::VAR) {
    // Save state for potential backtracking
    Token savedCurTok = curTok;
    auto savedLexerPos = lexer.getPosition();
    auto savedTokenStack = tokenStack;

    getNextToken();  // eat 'var'

    if (curTok.kind == TokenKind::IDENTIFIER) {
      std::string varName = curTok.getIdentifier().value();
      getNextToken();  // eat identifier

      if (curTok.kind == TokenKind::COLON) {
        getNextToken();  // eat ':'
        TypeAnnotation typeAnnot = parseTypeAnnotation();

        // Now check if next token is 'in' (contextual keyword)
        if (curTok.kind == TokenKind::IDENTIFIER &&
            curTok.getIdentifier().value() == "in") {
          // It's a for-in loop!
          getNextToken();  // eat 'in'

          auto iterable = parseExpression();
          if (!iterable) return nullptr;

          if (curTok.kind != TokenKind::PAREN_CLOSE) {
            parsingError("Expected ')' after for-in iterable");
            return nullptr;
          }
          getNextToken();  // eat ')'

          // Require curly braces for for-in body
          if (curTok.kind != TokenKind::BRACE_OPEN) {
            parsingError("Expected '{' for for-in body");
            return nullptr;
          }

          auto bodyBlock = parseBlock();
          if (!bodyBlock) return nullptr;

          // Convert block to single expression if needed
          unique_ptr<ExprAST> body;
          if (bodyBlock->getBody().empty()) {
            body = std::make_unique<NumberExprAST>(0.0);
          } else if (bodyBlock->getBody().size() == 1) {
            body = std::move(const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
                bodyBlock->getBody())[0]);
          } else {
            body = std::move(bodyBlock);
          }

          return std::make_unique<ForInExprAST>(
              std::move(varName), std::move(typeAnnot), std::move(iterable),
              std::move(body));
        }
      }
    }

    // Not a for-in loop, backtrack and continue with traditional for loop
    curTok = savedCurTok;
    lexer.setPosition(savedLexerPos);
    tokenStack = savedTokenStack;
  }

  // Traditional for loop: for (init; condition; increment) { body }
  // Parse initialization (can be empty, var declaration, or assignment)
  unique_ptr<ExprAST> init;
  if (curTok.kind != TokenKind::SEMI_COLON) {
    // Check if it's a variable declaration
    if (curTok.kind == TokenKind::VAR) {
      init = parseVarDeclaration();  // Without semicolon consumption
    } else {
      init = parseExpression();
    }
    if (!init) return nullptr;
  }

  if (curTok.kind != TokenKind::SEMI_COLON)
    parsingError("Expected ';' after for loop initialization");
  getNextToken();  // eat ';'

  // Parse condition (can be empty for infinite loop)
  unique_ptr<ExprAST> condition;
  if (curTok.kind != TokenKind::SEMI_COLON) {
    condition = parseExpression();
    if (!condition) return nullptr;
  }

  if (curTok.kind != TokenKind::SEMI_COLON)
    parsingError("Expected ';' after for loop condition");
  getNextToken();  // eat ';'

  // Parse increment (can be empty) - can be assignment or expression
  unique_ptr<ExprAST> increment;
  if (curTok.kind != TokenKind::PAREN_CLOSE) {
    // Check for assignment (identifier = expr)
    if (curTok.kind == TokenKind::IDENTIFIER) {
      auto savedPos = lexer.getPosition();
      auto savedTok = curTok;
      std::string idName = std::get<std::string>(curTok.value);
      getNextToken();

      if (curTok.kind == TokenKind::EQUAL) {
        // It's an assignment: i = i + 1
        getNextToken();  // eat '='
        auto expr = parseExpression();
        if (!expr) return nullptr;
        increment =
            std::make_unique<VariableAssignmentAST>(idName, std::move(expr));
      } else {
        // Not an assignment, backtrack and parse as expression
        lexer.setPosition(savedPos);
        curTok = savedTok;
        increment = parseExpression();
      }
    } else {
      increment = parseExpression();
    }
    if (!increment) return nullptr;
  }

  if (curTok.kind != TokenKind::PAREN_CLOSE)
    parsingError("Expected ')' after for loop increment");
  getNextToken();  // eat ')'

  // Require curly braces for for-loop body
  if (curTok.kind != TokenKind::BRACE_OPEN)
    parsingError("Expected '{' for for-loop body");

  auto bodyBlock = parseBlock();
  if (!bodyBlock) return nullptr;

  // Convert block to single expression if needed
  unique_ptr<ExprAST> body;
  if (bodyBlock->getBody().empty()) {
    body = std::make_unique<NumberExprAST>(0.0);
  } else if (bodyBlock->getBody().size() == 1) {
    body = std::move(const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
        bodyBlock->getBody())[0]);
  } else {
    body = std::move(bodyBlock);
  }

  return std::make_unique<ForExprAST>(std::move(init), std::move(condition),
                                      std::move(increment), std::move(body));
}

unique_ptr<WhileExprAST> Parser::parseWhileLoop() {
  getNextToken();  // eat 'while'

  if (curTok.kind != TokenKind::PAREN_OPEN)
    parsingError("Expected '(' after 'while'");
  getNextToken();  // eat '('

  auto condition = parseExpression();
  if (!condition) return nullptr;

  if (curTok.kind != TokenKind::PAREN_CLOSE)
    parsingError("Expected ')' after while condition");
  getNextToken();  // eat ')'

  // Require curly braces for while-loop body
  if (curTok.kind != TokenKind::BRACE_OPEN)
    parsingError("Expected '{' for while-loop body");

  auto bodyBlock = parseBlock();
  if (!bodyBlock) return nullptr;

  // Convert block to single expression if needed
  unique_ptr<ExprAST> body;
  if (bodyBlock->getBody().empty()) {
    body = std::make_unique<NumberExprAST>(0.0);
  } else if (bodyBlock->getBody().size() == 1) {
    body = std::move(const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
        bodyBlock->getBody())[0]);
  } else {
    body = std::move(bodyBlock);
  }

  return std::make_unique<WhileExprAST>(std::move(condition), std::move(body));
}

unique_ptr<BreakAST> Parser::parseBreak() {
  getNextToken();  // eat 'break'

  // Expect semicolon after break
  if (curTok.kind == TokenKind::SEMI_COLON) {
    getNextToken();  // eat ';'
  }

  return std::make_unique<BreakAST>();
}

unique_ptr<ContinueAST> Parser::parseContinue() {
  getNextToken();  // eat 'continue'

  // Expect semicolon after continue
  if (curTok.kind == TokenKind::SEMI_COLON) {
    getNextToken();  // eat ';'
  }

  return std::make_unique<ContinueAST>();
}

std::unique_ptr<PrototypeAST> Parser::parseExtern() {
  getNextToken();  // eat 'extern'

  // Expect 'function' keyword
  if (curTok.kind != TokenKind::FUNCTION) {
    parsingError("expected 'function' after 'extern'");
    return nullptr;
  }
  getNextToken();  // eat 'function'

  return parsePrototype();
}

// Parse import statement: import "path/to/file.sun";
unique_ptr<ImportAST> Parser::parseImportStatement() {
  getNextToken();  // eat 'import'

  if (curTok.kind != TokenKind::STRING) {
    parsingError("expected string literal after 'import'");
    return nullptr;
  }

  std::string path = curTok.getString().value();
  getNextToken();  // eat string literal

  if (curTok.kind != TokenKind::SEMI_COLON) {
    parsingError("expected ';' after import statement");
    return nullptr;
  }
  getNextToken();  // eat ';'

  return std::make_unique<ImportAST>(std::move(path));
}

// Parse declare statement: declare [Alias =] Type<Args>;
// Used for explicit generic instantiation and optional type aliasing
unique_ptr<DeclareTypeAST> Parser::parseDeclareStatement() {
  getNextToken();  // eat 'declare'

  std::optional<std::string> alias;

  // Check for alias: IDENTIFIER followed by '='
  if (curTok.kind == TokenKind::IDENTIFIER) {
    // Peek ahead to see if this is "Alias = Type" or just "Type"
    std::string name = curTok.getIdentifier().value();
    getNextToken();  // eat identifier

    if (curTok.kind == TokenKind::EQUAL) {
      // This is an alias declaration: declare Alias = Type;
      alias = name;
      getNextToken();  // eat '='
    } else {
      // Not an alias, this identifier is part of the type
      // We need to put back the identifier by handling it as a type
      // Create type annotation from the identifier we already consumed
      TypeAnnotation typeAnnot(name);

      // Check for generic type arguments: Type<Args>
      if (curTok.kind == TokenKind::LESS) {
        getNextToken();  // eat '<'
        while (true) {
          auto argType = parseTypeAnnotation();
          typeAnnot.typeArguments.push_back(
              std::make_unique<TypeAnnotation>(std::move(argType)));
          if (curTok.kind == TokenKind::COMMA) {
            getNextToken();  // eat ','
          } else {
            break;
          }
        }
        if (curTok.kind != TokenKind::GREATER) {
          parsingError("expected '>' after type arguments");
          return nullptr;
        }
        getNextToken();  // eat '>'
      }

      if (curTok.kind != TokenKind::SEMI_COLON) {
        parsingError("expected ';' after declare statement");
        return nullptr;
      }
      getNextToken();  // eat ';'

      return std::make_unique<DeclareTypeAST>(std::move(typeAnnot), alias);
    }
  }

  // Parse the type annotation
  auto typeAnnot = parseTypeAnnotation();

  if (curTok.kind != TokenKind::SEMI_COLON) {
    parsingError("expected ';' after declare statement");
    return nullptr;
  }
  getNextToken();  // eat ';'

  return std::make_unique<DeclareTypeAST>(std::move(typeAnnot), alias);
}

// Parse namespace/module declaration: module Name { declarations... }
// Supports both 'module' (preferred) and 'namespace' (legacy) keywords
unique_ptr<NamespaceAST> Parser::parseNamespaceDecl() {
  getNextToken();  // eat 'module' or 'namespace'

  if (curTok.kind != TokenKind::IDENTIFIER) {
    parsingError("expected module name");
    return nullptr;
  }

  std::string name = curTok.getIdentifier().value();
  getNextToken();  // eat module name

  if (curTok.kind != TokenKind::BRACE_OPEN) {
    parsingError("expected '{' after module name");
    return nullptr;
  }

  auto body = parseBlock();
  if (!body) return nullptr;

  return std::make_unique<NamespaceAST>(std::move(name), std::move(body));
}

// Parse using statement with dot-based syntax:
//   using sun;           -> import all from sun
//   using sun.Vec;       -> import specific symbol Vec from sun
//   using sun.Mat*;      -> prefix wildcard: import all starting with "Mat"
// Also supports legacy :: syntax for backward compatibility
unique_ptr<UsingAST> Parser::parseUsingStatement() {
  getNextToken();  // eat 'using'

  std::vector<std::string> namespacePath;
  std::string target;

  if (curTok.kind != TokenKind::IDENTIFIER) {
    parsingError("expected identifier after 'using'");
    return nullptr;
  }

  // Parse first identifier (module name)
  std::string firstName = curTok.getIdentifier().value();
  getNextToken();  // eat identifier

  // Check what follows: ';', '.', or '::'
  if (curTok.kind == TokenKind::SEMI_COLON) {
    // Simple form: "using sun;" means import all from sun
    namespacePath.push_back(std::move(firstName));
    target = "*";
    getNextToken();  // eat ';'
    return std::make_unique<UsingAST>(std::move(namespacePath),
                                      std::move(target));
  }

  // Dot-based path: using sun.Vec; or using sun.nested.Vec; or using sun.Mat*;
  if (curTok.kind == TokenKind::DOT) {
    namespacePath.push_back(std::move(firstName));

    while (curTok.kind == TokenKind::DOT) {
      getNextToken();  // eat '.'

      // Check for prefix wildcard: using sun.Mat*;
      if (curTok.kind == TokenKind::IDENTIFIER) {
        std::string part = curTok.getIdentifier().value();
        getNextToken();  // eat identifier

        if (curTok.kind == TokenKind::STAR) {
          // Prefix wildcard: "using sun.Mat*;" imports all starting with "Mat"
          target = part + "*";
          getNextToken();  // eat '*'
          break;
        } else if (curTok.kind == TokenKind::DOT) {
          // More path components: sun.nested.deeper
          namespacePath.push_back(std::move(part));
        } else {
          // Final target: "using sun.Vec;"
          target = std::move(part);
          break;
        }
      } else if (curTok.kind == TokenKind::STAR) {
        // Explicit wildcard after dot: "using sun.*;" (rare, same as "using
        // sun;")
        target = "*";
        getNextToken();  // eat '*'
        break;
      } else {
        parsingError("expected identifier or '*' after '.' in using statement");
        return nullptr;
      }
    }
  } else {
    parsingError("expected '.' or ';' after module name in using statement");
    return nullptr;
  }

  if (curTok.kind != TokenKind::SEMI_COLON) {
    parsingError("expected ';' after using statement");
    return nullptr;
  }
  getNextToken();  // eat ';'

  return std::make_unique<UsingAST>(std::move(namespacePath),
                                    std::move(target));
}

// Parse a qualified name (Module.name) or simple identifier
unique_ptr<ExprAST> Parser::parseQualifiedOrSimpleName() {
  if (curTok.kind != TokenKind::IDENTIFIER) {
    return nullptr;
  }

  std::string firstName = curTok.getIdentifier().value();
  getNextToken();  // eat first identifier

  // Check if it's a qualified name (dot-based)
  if (curTok.kind == TokenKind::DOT) {
    std::vector<std::string> parts;
    parts.push_back(std::move(firstName));

    while (curTok.kind == TokenKind::DOT) {
      getNextToken();  // eat '.'

      if (curTok.kind != TokenKind::IDENTIFIER) {
        parsingError("expected identifier after '.'");
        return nullptr;
      }

      parts.push_back(curTok.getIdentifier().value());
      getNextToken();  // eat identifier
    }

    return std::make_unique<QualifiedNameAST>(std::move(parts));
  }

  // Simple identifier - return as variable reference
  return std::make_unique<VariableReferenceAST>(std::move(firstName));
}

// Collect AST nodes from an imported file (unified AST approach)
// Recursively parses imports and collects all non-import statements
void Parser::collectImports(
    const std::string& importPath,
    std::vector<std::unique_ptr<ExprAST>>& collectedAST) {
  // Check if this is a .moon import (precompiled library)
  if (importPath.size() > 5 &&
      importPath.substr(importPath.size() - 5) == ".moon") {
    collectMoonImport(importPath, collectedAST);
    return;
  }

  // Check if this import is available as a precompiled library
  // If so, we'll still parse for type info but link precompiled bitcode later
  if (sun::LibraryCache::instance().hasModule(importPath)) {
    // Record this as a precompiled import for later linking
    bool alreadyRecorded = false;
    for (const auto& path : *precompiledImports) {
      if (path == importPath) {
        alreadyRecorded = true;
        break;
      }
    }
    if (!alreadyRecorded) {
      precompiledImports->push_back(importPath);
    }
    // Continue to parse source for type information
    // The codegen will be skipped and precompiled bitcode linked instead
  }

  // Resolve the import path
  std::filesystem::path resolved;
  if (std::filesystem::path(importPath).is_absolute()) {
    resolved = importPath;
  } else {
    // Check SUN_PATH environment variable first
    const char* sunPath = std::getenv("SUN_PATH");
    if (sunPath && std::strlen(sunPath) > 0) {
      auto sunPathResolved = std::filesystem::path(sunPath) / importPath;
      if (std::filesystem::exists(sunPathResolved)) {
        resolved = sunPathResolved;
      }
    }
    // Check system-wide installation paths
    if (resolved.empty()) {
      auto sysPath =
          std::filesystem::path("/usr/share/sun/stdlib") / importPath;
      if (std::filesystem::exists(sysPath)) {
        resolved = sysPath;
      }
    }
    // Fall back to resolving relative to current file's directory
    if (resolved.empty()) {
      resolved = std::filesystem::path(baseDir) / importPath;
    }
  }

  if (!std::filesystem::exists(resolved)) {
    logAndThrowError("Could not find imported file: " + importPath);
    return;
  }

  resolved = std::filesystem::canonical(resolved);
  std::string resolvedStr = resolved.string();

  // Check for circular imports
  if (importedFiles->count(resolvedStr)) {
    return;  // Already imported, skip
  }
  importedFiles->insert(resolvedStr);

  // Read the imported file
  std::ifstream file(resolvedStr);
  if (!file.is_open()) {
    logAndThrowError("Could not open imported file: " + resolvedStr);
    return;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  // Create a sub-parser for the imported file, sharing import tracking state
  std::istringstream ss(source);
  Parser importParser(ss);
  importParser.baseDir = resolved.parent_path().string();
  importParser.importedFiles = importedFiles;  // Share cycle detection
  importParser.setFilePath(resolvedStr);  // Set file path for error messages
  importParser.getNextToken();            // Prime the first token

  // Parse the imported file
  auto blockAst = importParser.parseProgram();
  if (!blockAst) {
    logAndThrowError("Failed to parse imported file: " + resolvedStr);
    return;
  }

  // Recursively collect any nested imports first
  for (const auto& stmt : blockAst->getBody()) {
    if (stmt->isImport()) {
      const auto& importStmt = static_cast<const ImportAST&>(*stmt);
      importParser.collectImports(importStmt.getPath(), collectedAST);
    }
  }

  // Collect all non-import statements from this file
  for (auto& stmt : const_cast<std::vector<std::unique_ptr<ExprAST>>&>(
           blockAst->getBody())) {
    if (stmt && !stmt->isImport()) {
      collectedAST.push_back(std::move(stmt));
    }
  }
}

// Handle import of a precompiled .moon file
void Parser::collectMoonImport(
    const std::string& moonPath,
    std::vector<std::unique_ptr<ExprAST>>& collectedAST) {
  // Resolve the moon path
  std::filesystem::path resolved;
  if (std::filesystem::path(moonPath).is_absolute()) {
    resolved = moonPath;
  } else {
    // Check SUN_PATH environment variable first
    const char* sunPath = std::getenv("SUN_PATH");
    if (sunPath && std::strlen(sunPath) > 0) {
      auto sunPathResolved = std::filesystem::path(sunPath) / moonPath;
      if (std::filesystem::exists(sunPathResolved)) {
        resolved = sunPathResolved;
      }
    }
    // Check system-wide installation paths
    if (resolved.empty()) {
      auto sysPath = std::filesystem::path("/usr/lib/sun") / moonPath;
      if (std::filesystem::exists(sysPath)) {
        resolved = sysPath;
      }
    }
    // Fall back to resolving relative to current file's directory
    if (resolved.empty()) {
      resolved = std::filesystem::path(baseDir) / moonPath;
    }
  }

  if (!std::filesystem::exists(resolved)) {
    logAndThrowError("Could not find moon file: " + moonPath);
    return;
  }
  resolved = std::filesystem::canonical(resolved);
  std::string resolvedStr = resolved.string();

  // Check if already imported
  if (importedFiles->count(resolvedStr)) {
    return;
  }
  importedFiles->insert(resolvedStr);

  // Open the moon bundle
  PARSER_TIMER_START(open_moon);
  auto reader = sun::SunLibReader::open(resolved);
  if (!reader) {
    logAndThrowError("Failed to open moon: " + resolvedStr);
    return;
  }
  PARSER_TIMER_END(open_moon);

  // Process each module in the bundle
  PARSER_TIMER_START(process_modules);
  int parsedMethods = 0;
  for (const auto& modulePath : reader->listModules()) {
    // Record for linking
    bool alreadyRecorded = false;
    for (const auto& path : *precompiledImports) {
      if (path == modulePath) {
        alreadyRecorded = true;
        break;
      }
    }
    if (!alreadyRecorded) {
      precompiledImports->push_back(modulePath);
    }

    // Get metadata for this module
    const auto* metadata = reader->getMetadata(modulePath);
    if (!metadata) {
      continue;
    }

    // Note: Dependencies are already linked into the bitcode when the moon file
    // was created. The deps list in metadata is for informational purposes only
    // (e.g., tracking which libraries a module depends on). We do NOT try to
    // load them here because:
    // 1. The bitcode is self-contained
    // 2. The dependency paths are from the original build machine

    // Get symbol prefix from content hash for moon library isolation
    std::string symbolPrefix = metadata->getSymbolPrefix();

    // Namespace prefix for stripping from type signatures in V3 backward compat
    // In V4+, baseName is available directly
    std::string nsPrefix =
        metadata->moduleName.empty() ? "" : metadata->moduleName + "_";

    // Collect AST stubs for this module - may be wrapped in a namespace
    std::vector<std::unique_ptr<ExprAST>> moduleAST;

    // Create AST stubs from metadata
    // IMPORTANT: Process interfaces FIRST (before classes that implement them)
    for (const auto& ifaceInfo : metadata->interfaces) {
      std::vector<InterfaceMethodDecl> methods;
      for (const auto& method : ifaceInfo.methods) {
        std::unique_ptr<FunctionAST> func;
        bool hasDefaultImpl = false;

        // Create stub with prototype - body will be parsed lazily when
        // instantiated
        std::vector<std::pair<std::string, TypeAnnotation>> params;
        for (size_t i = 0; i < method.paramNames.size(); ++i) {
          params.push_back(
              {method.paramNames[i],
               parseTypeFromString(method.paramTypeSigs[i], nsPrefix)});
        }

        auto returnType =
            method.returnTypeSig.empty() || method.returnTypeSig == "void"
                ? std::nullopt
                : std::optional<TypeAnnotation>(
                      parseTypeFromString(method.returnTypeSig, nsPrefix));

        // Handle variadic parameter
        std::optional<std::string> variadicParam;
        std::optional<TypeAnnotation> variadicConstraint;
        if (!method.variadicParamName.empty()) {
          variadicParam = method.variadicParamName;
          if (!method.variadicConstraint.empty()) {
            variadicConstraint =
                parseTypeFromString(method.variadicConstraint, nsPrefix);
          }
        }

        auto proto = std::make_unique<PrototypeAST>(
            method.name, std::move(params), returnType, method.typeParams,
            variadicParam, variadicConstraint);

        // Create empty body - will be parsed lazily from sourceText
        auto body = std::make_unique<BlockExprAST>(
            std::vector<std::unique_ptr<ExprAST>>());

        func = std::make_unique<FunctionAST>(std::move(proto), std::move(body));

        // Store source for lazy parsing if available
        if (!method.bodySource.empty()) {
          func->setSourceText(method.bodySource);
          hasDefaultImpl = true;
        }

        methods.push_back({std::move(func), hasDefaultImpl});
      }

      // Use baseName directly - no stripping needed
      auto ifaceDef = std::make_unique<InterfaceDefinitionAST>(
          ifaceInfo.baseName,
          std::vector<std::string>{},         // TODO: deserialize type params
          std::vector<InterfaceFieldDecl>{},  // No fields for now
          std::move(methods),
          true);  // precompiled = true

      moduleAST.push_back(std::move(ifaceDef));
    }

    // Classes (after interfaces so interface lookups work)
    for (const auto& classInfo : metadata->classes) {
      std::vector<ClassFieldDecl> fields;
      for (const auto& field : classInfo.fields) {
        fields.push_back(
            {field.name, parseTypeFromString(field.typeSig, nsPrefix)});
      }

      std::vector<ClassMethodDecl> methods;
      for (const auto& method : classInfo.methods) {
        std::unique_ptr<FunctionAST> func;

        // Create stub with prototype - body will be parsed lazily when
        // instantiated
        std::vector<std::pair<std::string, TypeAnnotation>> params;
        for (size_t i = 0; i < method.paramNames.size(); ++i) {
          params.push_back(
              {method.paramNames[i],
               parseTypeFromString(method.paramTypeSigs[i], nsPrefix)});
        }

        auto returnType =
            method.returnTypeSig.empty() || method.returnTypeSig == "void"
                ? std::nullopt
                : std::optional<TypeAnnotation>(
                      parseTypeFromString(method.returnTypeSig, nsPrefix));

        // Handle variadic parameter
        std::optional<std::string> variadicParam;
        std::optional<TypeAnnotation> variadicConstraint;
        if (!method.variadicParamName.empty()) {
          variadicParam = method.variadicParamName;
          if (!method.variadicConstraint.empty()) {
            variadicConstraint =
                parseTypeFromString(method.variadicConstraint, nsPrefix);
          }
        }

        auto proto = std::make_unique<PrototypeAST>(
            method.name, std::move(params), returnType, method.typeParams,
            variadicParam, variadicConstraint);

        // Create empty body - will be parsed lazily from sourceText
        auto body = std::make_unique<BlockExprAST>(
            std::vector<std::unique_ptr<ExprAST>>());

        func = std::make_unique<FunctionAST>(std::move(proto), std::move(body));

        // Store source for lazy parsing if available
        if (!method.bodySource.empty()) {
          func->setSourceText(method.bodySource);
        }

        methods.push_back({std::move(func)});
      }

      // Convert string interfaces to ImplementedInterfaceAST
      // Format: "InterfaceName" or "InterfaceName<T, U>" for generic interfaces
      std::vector<ImplementedInterfaceAST> interfaces;
      for (const auto& ifaceStr : classInfo.interfaces) {
        ImplementedInterfaceAST iface;

        // Check for generic interface: "IIterable<V>"
        auto angleBracket = ifaceStr.find('<');
        if (angleBracket != std::string::npos) {
          iface.name = ifaceStr.substr(0, angleBracket);
          // Parse type arguments from "<T, U>"
          auto closeAngle = ifaceStr.rfind('>');
          if (closeAngle != std::string::npos &&
              closeAngle > angleBracket + 1) {
            std::string argsStr = ifaceStr.substr(
                angleBracket + 1, closeAngle - angleBracket - 1);
            // Split by comma (handling nested generics)
            int depth = 0;
            size_t start = 0;
            for (size_t i = 0; i <= argsStr.size(); ++i) {
              if (i == argsStr.size() || (argsStr[i] == ',' && depth == 0)) {
                std::string argStr = argsStr.substr(start, i - start);
                // Trim whitespace
                size_t first = argStr.find_first_not_of(' ');
                size_t last = argStr.find_last_not_of(' ');
                if (first != std::string::npos) {
                  argStr = argStr.substr(first, last - first + 1);
                }
                if (!argStr.empty()) {
                  iface.typeArguments.push_back(
                      parseTypeFromString(argStr, nsPrefix));
                }
                start = i + 1;
              } else if (argsStr[i] == '<') {
                depth++;
              } else if (argsStr[i] == '>') {
                depth--;
              }
            }
          }
        } else {
          iface.name = ifaceStr;
        }

        // Strip namespace prefix from interface name if present
        // (interfaces are stored qualified in metadata)
        if (!nsPrefix.empty() && iface.name.rfind(nsPrefix, 0) == 0) {
          iface.name = iface.name.substr(nsPrefix.size());
        }

        interfaces.push_back(std::move(iface));
      }

      // Use baseName directly - no stripping needed
      auto classDef = std::make_unique<ClassDefinitionAST>(
          classInfo.baseName, classInfo.typeParams, std::move(interfaces),
          std::move(fields), std::move(methods),
          true);  // precompiled = true

      moduleAST.push_back(std::move(classDef));
    }

    // Functions (extern declarations)
    for (const auto& sym : metadata->exports) {
      if (sym.kind == sun::ExportedSymbol::Kind::Function) {
        // Use baseName directly - no stripping needed
        // Parse function signature from typeSignature: "(i32, i32) -> i32"
        // Pass nsPrefix to strip namespace from types in the signature
        auto funcAST =
            parseFunctionSignature(sym.baseName, sym.typeSignature, nsPrefix);
        if (funcAST) {
          moduleAST.push_back(std::move(funcAST));
        }
      }
    }

    // Wrap all stubs in a library scope namespace (using content hash)
    // This creates: $hash$ { module { ... } } structure for isolation
    if (!moduleAST.empty()) {
      std::vector<std::unique_ptr<ExprAST>> libraryContent;

      // If module has a namespace, wrap stubs in inner NamespaceAST
      if (!metadata->moduleName.empty()) {
        auto nsBody = std::make_unique<BlockExprAST>(std::move(moduleAST));
        auto nsAST = std::make_unique<NamespaceAST>(metadata->moduleName,
                                                    std::move(nsBody));
        nsAST->setPrecompiled(true);
        libraryContent.push_back(std::move(nsAST));
      } else {
        // No module namespace - add stubs directly to library scope
        for (auto& ast : moduleAST) {
          libraryContent.push_back(std::move(ast));
        }
      }

      // Wrap in outer library scope namespace using content hash
      auto libBody = std::make_unique<BlockExprAST>(std::move(libraryContent));
      auto libAST =
          std::make_unique<NamespaceAST>(symbolPrefix, std::move(libBody));
      libAST->setPrecompiled(true);
      collectedAST.push_back(std::move(libAST));
    }
  }
  PARSER_TIMER_END(process_modules);

  // Store the moon reader in the cache for later linking
  sun::LibraryCache::instance().addBundle(resolved);
}

// Helper to parse a type string back into TypeAnnotation.
// If nsPrefix is provided (e.g., "sun_"), it will be stripped from type names.
TypeAnnotation Parser::parseTypeFromString(const std::string& typeStr,
                                           const std::string& nsPrefix) {
  // Check for ", error" suffix indicating error union type
  bool canError = false;
  std::string cleanType = typeStr;

  // Check for ", error" or ", IError" suffix (case-insensitive for error part)
  const std::string errorSuffix1 = ", error";
  const std::string errorSuffix2 = ", IError";
  if (cleanType.size() > errorSuffix1.size() &&
      cleanType.compare(cleanType.size() - errorSuffix1.size(),
                        errorSuffix1.size(), errorSuffix1) == 0) {
    canError = true;
    cleanType = cleanType.substr(0, cleanType.size() - errorSuffix1.size());
  } else if (cleanType.size() > errorSuffix2.size() &&
             cleanType.compare(cleanType.size() - errorSuffix2.size(),
                               errorSuffix2.size(), errorSuffix2) == 0) {
    canError = true;
    cleanType = cleanType.substr(0, cleanType.size() - errorSuffix2.size());
  }

  // Handle common primitive types
  if (cleanType == "void") {
    TypeAnnotation result("void");
    result.canError = canError;
    return result;
  }
  if (cleanType == "bool") {
    TypeAnnotation result("bool");
    result.canError = canError;
    return result;
  }
  if (cleanType == "i8") {
    TypeAnnotation result("i8");
    result.canError = canError;
    return result;
  }
  if (cleanType == "i16") {
    TypeAnnotation result("i16");
    result.canError = canError;
    return result;
  }
  if (cleanType == "i32") {
    TypeAnnotation result("i32");
    result.canError = canError;
    return result;
  }
  if (cleanType == "i64") {
    TypeAnnotation result("i64");
    result.canError = canError;
    return result;
  }
  if (cleanType == "f32") {
    TypeAnnotation result("f32");
    result.canError = canError;
    return result;
  }
  if (cleanType == "f64") {
    TypeAnnotation result("f64");
    result.canError = canError;
    return result;
  }
  if (cleanType == "string") {
    TypeAnnotation result("string");
    result.canError = canError;
    return result;
  }

  // Handle pointer types: ptr<T> or ptr(T)
  if (cleanType.size() > 4 && cleanType.substr(0, 4) == "ptr<") {
    size_t end = cleanType.rfind('>');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(4, end - 4);
      TypeAnnotation result("ptr");
      result.elementType = std::make_unique<TypeAnnotation>(
          parseTypeFromString(inner, nsPrefix));
      result.canError = canError;
      return result;
    }
  }
  if (cleanType.size() > 4 && cleanType.substr(0, 4) == "ptr(") {
    size_t end = cleanType.rfind(')');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(4, end - 4);
      TypeAnnotation result("ptr");
      result.elementType = std::make_unique<TypeAnnotation>(
          parseTypeFromString(inner, nsPrefix));
      result.canError = canError;
      return result;
    }
  }

  // Handle raw pointer types: raw_ptr<T> or raw_ptr(T)
  if (cleanType.size() > 8 && cleanType.substr(0, 8) == "raw_ptr<") {
    size_t end = cleanType.rfind('>');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(8, end - 8);
      TypeAnnotation result("raw_ptr");
      result.elementType = std::make_unique<TypeAnnotation>(
          parseTypeFromString(inner, nsPrefix));
      result.canError = canError;
      return result;
    }
  }
  if (cleanType.size() > 8 && cleanType.substr(0, 8) == "raw_ptr(") {
    size_t end = cleanType.rfind(')');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(8, end - 8);
      TypeAnnotation result("raw_ptr");
      result.elementType = std::make_unique<TypeAnnotation>(
          parseTypeFromString(inner, nsPrefix));
      result.canError = canError;
      return result;
    }
  }

  // Handle ref types: ref T or ref(T)
  if (cleanType.size() > 4 && cleanType.substr(0, 4) == "ref ") {
    std::string inner = cleanType.substr(4);
    TypeAnnotation result("ref");
    result.elementType =
        std::make_unique<TypeAnnotation>(parseTypeFromString(inner, nsPrefix));
    result.canError = canError;
    return result;
  }
  if (cleanType.size() > 4 && cleanType.substr(0, 4) == "ref(") {
    size_t end = cleanType.rfind(')');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(4, end - 4);
      TypeAnnotation result("ref");
      result.elementType = std::make_unique<TypeAnnotation>(
          parseTypeFromString(inner, nsPrefix));
      result.canError = canError;
      return result;
    }
  }

  // Handle array types: array<T> or array<T, N> or array<T, M, N>
  if (cleanType.size() > 6 && cleanType.substr(0, 6) == "array<") {
    size_t end = cleanType.rfind('>');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(6, end - 6);
      // Parse: "elementType" or "elementType, dim1, dim2, ..."
      // Find the first comma that separates element type from dimensions
      // Need to handle nested types like array<array<i32, 2>, 3>
      int depth = 0;
      size_t firstComma = std::string::npos;
      for (size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '<')
          depth++;
        else if (inner[i] == '>')
          depth--;
        else if (inner[i] == ',' && depth == 0) {
          firstComma = i;
          break;
        }
      }

      TypeAnnotation result("array");
      if (firstComma == std::string::npos) {
        // Unsized array: array<T>
        result.elementType = std::make_unique<TypeAnnotation>(
            parseTypeFromString(inner, nsPrefix));
      } else {
        // Sized array: array<T, dim1, dim2, ...>
        std::string elemType = inner.substr(0, firstComma);
        result.elementType = std::make_unique<TypeAnnotation>(
            parseTypeFromString(elemType, nsPrefix));

        // Parse dimensions
        std::string dims = inner.substr(firstComma + 1);
        std::istringstream dimStream(dims);
        std::string dimStr;
        while (std::getline(dimStream, dimStr, ',')) {
          // Trim whitespace
          size_t start = dimStr.find_first_not_of(" \t");
          size_t stop = dimStr.find_last_not_of(" \t");
          if (start != std::string::npos && stop != std::string::npos) {
            dimStr = dimStr.substr(start, stop - start + 1);
          }
          result.arrayDimensions.push_back(std::stoull(dimStr));
        }
      }
      result.canError = canError;
      return result;
    }
  }

  // Handle static pointer types: static_ptr<T> or static_ptr(T)
  if (cleanType.size() > 11 && cleanType.substr(0, 11) == "static_ptr<") {
    size_t end = cleanType.rfind('>');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(11, end - 11);
      TypeAnnotation result("static_ptr");
      result.elementType = std::make_unique<TypeAnnotation>(
          parseTypeFromString(inner, nsPrefix));
      result.canError = canError;
      return result;
    }
  }
  if (cleanType.size() > 11 && cleanType.substr(0, 11) == "static_ptr(") {
    size_t end = cleanType.rfind(')');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(11, end - 11);
      TypeAnnotation result("static_ptr");
      result.elementType = std::make_unique<TypeAnnotation>(
          parseTypeFromString(inner, nsPrefix));
      result.canError = canError;
      return result;
    }
  }

  // Handle generic class types: ClassName<T, U, ...>
  // Look for '<' that indicates generic type arguments
  auto angleBracketPos = cleanType.find('<');
  if (angleBracketPos != std::string::npos && angleBracketPos > 0) {
    // Make sure it's not a built-in type we already handled (ptr<, raw_ptr<,
    // etc.)
    std::string baseName = cleanType.substr(0, angleBracketPos);
    // Strip namespace prefix from base name if present
    if (!nsPrefix.empty() && baseName.rfind(nsPrefix, 0) == 0) {
      baseName = baseName.substr(nsPrefix.size());
    }
    if (baseName != "ptr" && baseName != "raw_ptr" &&
        baseName != "static_ptr" && baseName != "array" && baseName != "ref") {
      // This looks like a generic class type
      size_t end = cleanType.rfind('>');
      if (end != std::string::npos && end > angleBracketPos) {
        std::string argsStr =
            cleanType.substr(angleBracketPos + 1, end - angleBracketPos - 1);

        TypeAnnotation result(baseName);

        // Parse comma-separated type arguments, handling nested generics
        int depth = 0;
        size_t argStart = 0;
        for (size_t i = 0; i <= argsStr.size(); ++i) {
          char c =
              (i < argsStr.size()) ? argsStr[i] : ',';  // Treat end as comma
          if (c == '<') {
            depth++;
          } else if (c == '>') {
            depth--;
          } else if (c == ',' && depth == 0) {
            std::string argStr = argsStr.substr(argStart, i - argStart);
            // Trim whitespace
            size_t start = argStr.find_first_not_of(" \t");
            size_t stop = argStr.find_last_not_of(" \t");
            if (start != std::string::npos && stop != std::string::npos) {
              argStr = argStr.substr(start, stop - start + 1);
            }
            if (!argStr.empty()) {
              result.typeArguments.push_back(std::make_unique<TypeAnnotation>(
                  parseTypeFromString(argStr, nsPrefix)));
            }
            argStart = i + 1;
          }
        }

        result.canError = canError;
        return result;
      }
    }
  }

  // Default: treat as a class/interface name or type parameter
  // Strip namespace prefix if present
  std::string typeName = cleanType;
  if (!nsPrefix.empty() && typeName.rfind(nsPrefix, 0) == 0) {
    typeName = typeName.substr(nsPrefix.size());
  }
  TypeAnnotation result(typeName);
  result.canError = canError;
  return result;
}

// Parse a function signature string like "(i32, i32) -> i32" into an extern
// FunctionAST. If nsPrefix is provided, strip it from type names in the
// signature.
std::unique_ptr<FunctionAST> Parser::parseFunctionSignature(
    const std::string& name, const std::string& signature,
    const std::string& nsPrefix) {
  // Parse signature: "(param1, param2, ...) -> returnType"
  // Find the arrow
  size_t arrowPos = signature.find(") -> ");
  if (arrowPos == std::string::npos) {
    // No return type or malformed
    if (signature.empty() || signature[0] != '(') {
      return nullptr;
    }
    arrowPos = signature.find(")");
    if (arrowPos == std::string::npos) {
      return nullptr;
    }
  }

  // Extract parameters between parentheses
  std::vector<std::pair<std::string, TypeAnnotation>> params;
  if (signature.size() > 2 && signature[0] == '(') {
    std::string paramStr = signature.substr(1, arrowPos - 1);

    // Parse comma-separated parameters
    size_t pos = 0;
    int paramIndex = 0;
    while (pos < paramStr.size()) {
      // Skip whitespace
      while (pos < paramStr.size() &&
             (paramStr[pos] == ' ' || paramStr[pos] == ',')) {
        pos++;
      }
      if (pos >= paramStr.size()) break;

      // Find end of this type (next comma or end)
      size_t end = pos;
      int angleBrackets = 0;
      while (end < paramStr.size()) {
        char c = paramStr[end];
        if (c == '<')
          angleBrackets++;
        else if (c == '>')
          angleBrackets--;
        else if (c == ',' && angleBrackets == 0)
          break;
        end++;
      }

      std::string typeStr = paramStr.substr(pos, end - pos);
      // Trim whitespace
      while (!typeStr.empty() && typeStr.back() == ' ') typeStr.pop_back();
      while (!typeStr.empty() && typeStr.front() == ' ')
        typeStr = typeStr.substr(1);

      if (!typeStr.empty()) {
        std::string paramName = "arg" + std::to_string(paramIndex++);
        params.push_back({paramName, parseTypeFromString(typeStr, nsPrefix)});
      }

      pos = end;
    }
  }

  // Extract return type
  std::optional<TypeAnnotation> returnType;
  size_t returnStart = signature.find(") -> ");
  if (returnStart != std::string::npos) {
    std::string retStr = signature.substr(returnStart + 5);
    // Trim whitespace
    while (!retStr.empty() && retStr.back() == ' ') retStr.pop_back();
    while (!retStr.empty() && retStr.front() == ' ') retStr = retStr.substr(1);

    // For extern functions, void must be explicit
    if (!retStr.empty()) {
      returnType = parseTypeFromString(retStr, nsPrefix);
    }
  }

  // Create extern prototype (no body)
  auto proto =
      std::make_unique<PrototypeAST>(name, std::move(params), returnType);

  // Extern function - null body
  return std::make_unique<FunctionAST>(std::move(proto), nullptr);
}

// In parser.cpp (implementation)
std::unique_ptr<BlockExprAST> Parser::parseString(const std::string& source) {
  std::istringstream ss(source);
  lexer = Lexer(ss);
  getNextToken();  // Prime the first token
  return parseProgram();
}

// Thread-local stream storage for the reusable lexer
static thread_local std::unique_ptr<std::istringstream> tlMethodSourceStream;

// Parse a function from its source text (for loading generic methods from
// moon)
std::unique_ptr<FunctionAST> Parser::parseFunctionFromSource(
    const std::string& source) {
  PARSER_TIMER_START(func_parse);

  // Store source in thread-local to keep it alive during parsing
  tlMethodSourceStream = std::make_unique<std::istringstream>(source);

  // Reuse the existing lexer's NFA by resetting its input
  PARSER_TIMER_START(reset_input);
  lexer.resetInput(*tlMethodSourceStream);
  PARSER_TIMER_END(reset_input);

  getNextToken();  // Prime the first token

  if (curTok.kind != TokenKind::FUNCTION) {
    parsingError("Expected 'function' keyword in method source");
    return nullptr;
  }

  PARSER_TIMER_START(parse_func);
  auto result = parseFunction();
  PARSER_TIMER_END(parse_func);
  PARSER_TIMER_END(func_parse);

  return result;
}

// Static lazy parsing helper - uses thread-local Parser to avoid rebuilding
// NFA on every call. This centralizes lazy parsing logic for use by codegen.
std::unique_ptr<FunctionAST> Parser::lazyParseFunctionSource(
    const std::string& source) {
  // Thread-local parser and stream for efficient NFA reuse
  static thread_local std::unique_ptr<std::istringstream> lazyStream;
  static thread_local std::unique_ptr<Parser> lazyParser;

  // Keep the source string alive during parsing
  lazyStream = std::make_unique<std::istringstream>(source);

  if (!lazyParser) {
    // First call - create parser (builds NFA once)
    lazyParser = std::make_unique<Parser>(*lazyStream);
  }

  // Reuse existing parser's NFA via parseFunctionFromSource
  return lazyParser->parseFunctionFromSource(source);
}

// Parse class definition: class ClassName implements Interface1, Interface2 {
// fields and methods }
unique_ptr<ClassDefinitionAST> Parser::parseClassDefinition() {
  getNextToken();  // eat 'class'

  if (curTok.kind != TokenKind::IDENTIFIER) {
    parsingError("expected class name after 'class'");
    return nullptr;
  }

  std::string className = curTok.getIdentifier().value();
  getNextToken();  // eat class name

  // Parse optional type parameters: class Name<T, U, ...>
  std::vector<std::string> typeParameters;
  if (curTok.kind == TokenKind::LESS) {
    getNextToken();  // eat '<'

    // Parse comma-separated list of type parameter names
    while (curTok.kind == TokenKind::IDENTIFIER) {
      typeParameters.push_back(curTok.getIdentifier().value());
      getNextToken();  // eat type parameter name

      if (curTok.kind == TokenKind::COMMA) {
        getNextToken();  // eat ','
      } else {
        break;
      }
    }

    if (typeParameters.empty()) {
      parsingError("expected type parameter name after '<'");
      return nullptr;
    }

    if (curTok.kind != TokenKind::GREATER) {
      parsingError("expected '>' after type parameters");
      return nullptr;
    }
    getNextToken();  // eat '>'
  }

  // Parse optional implements clause
  std::vector<ImplementedInterfaceAST> implementedInterfaces;
  if (curTok.kind == TokenKind::IMPLEMENTS) {
    getNextToken();  // eat 'implements'

    // Parse comma-separated list of interface names with optional type args
    while (curTok.kind == TokenKind::IDENTIFIER) {
      ImplementedInterfaceAST iface;
      iface.name = curTok.getIdentifier().value();
      getNextToken();  // eat interface name

      // Parse optional type arguments: IIterator<T>
      if (curTok.kind == TokenKind::LESS) {
        getNextToken();  // eat '<'

        // Parse first type argument
        iface.typeArguments.push_back(parseTypeAnnotation());

        // Parse remaining type arguments
        while (curTok.kind == TokenKind::COMMA) {
          getNextToken();  // eat ','
          iface.typeArguments.push_back(parseTypeAnnotation());
        }

        if (curTok.kind != TokenKind::GREATER) {
          parsingError("expected '>' after interface type arguments");
          return nullptr;
        }
        getNextToken();  // eat '>'
      }

      implementedInterfaces.push_back(std::move(iface));

      if (curTok.kind == TokenKind::COMMA) {
        getNextToken();  // eat ','
      } else {
        break;
      }
    }

    if (implementedInterfaces.empty()) {
      parsingError("expected interface name after 'implements'");
      return nullptr;
    }
  }

  if (curTok.kind != TokenKind::BRACE_OPEN) {
    parsingError("expected '{' after class name");
    return nullptr;
  }
  getNextToken();  // eat '{'

  std::vector<ClassFieldDecl> fields;
  std::vector<ClassMethodDecl> methods;

  // Parse class body (fields and methods)
  while (curTok.kind != TokenKind::BRACE_CLOSE &&
         curTok.kind != TokenKind::TOK_EOF) {
    if (curTok.kind == TokenKind::VAR) {
      // Parse field declaration: var name: type;
      getNextToken();  // eat 'var'

      if (curTok.kind != TokenKind::IDENTIFIER) {
        parsingError("expected field name in class definition");
        return nullptr;
      }

      Position fieldLoc = curTok.start;  // Capture location before eating token
      std::string fieldName = curTok.getIdentifier().value();
      getNextToken();  // eat field name

      if (curTok.kind != TokenKind::COLON) {
        parsingError(
            "expected ':' after field name (type annotation required)");
        return nullptr;
      }
      getNextToken();  // eat ':'

      TypeAnnotation fieldType = parseTypeAnnotation();

      if (curTok.kind != TokenKind::SEMI_COLON) {
        parsingError("expected ';' after field declaration");
        return nullptr;
      }
      getNextToken();  // eat ';'

      fields.push_back({std::move(fieldName), std::move(fieldType), fieldLoc});
    } else if (curTok.kind == TokenKind::FUNCTION) {
      // Parse method declaration
      // Capture start position for source text extraction
      int startOffset = curTok.start.offset;

      auto func = parseFunction();
      if (!func) return nullptr;

      // For generic classes OR generic methods, capture the source text
      // Generic class methods need source text even if they're not themselves
      // generic, because they reference the class type parameter T
      if (!typeParameters.empty() || func->getProto().isGeneric()) {
        int endOffset = curTok.start.offset;
        std::string sourceText = getSourceText(startOffset, endOffset);
        func->setSourceText(sourceText);
      }

      bool isConstructor = (func->getProto().getName() == "init");

      ClassMethodDecl method;
      method.function = std::move(func);
      method.isConstructor = isConstructor;
      methods.push_back(std::move(method));

      // Skip optional semicolons after methods
      while (curTok.kind == TokenKind::SEMI_COLON) getNextToken();
    } else {
      parsingError(
          "expected 'var' (field) or 'function' (method) in class body");
      return nullptr;
    }
  }

  if (curTok.kind != TokenKind::BRACE_CLOSE) {
    parsingError("expected '}' at end of class definition");
    return nullptr;
  }
  getNextToken();  // eat '}'

  return std::make_unique<ClassDefinitionAST>(
      std::move(className), std::move(typeParameters),
      std::move(implementedInterfaces), std::move(fields), std::move(methods));
}

// Parse interface definition: interface InterfaceName<T, U> { fields and
// methods } Methods can have optional default implementations
unique_ptr<InterfaceDefinitionAST> Parser::parseInterfaceDefinition() {
  getNextToken();  // eat 'interface'

  if (curTok.kind != TokenKind::IDENTIFIER) {
    parsingError("expected interface name after 'interface'");
    return nullptr;
  }

  std::string interfaceName = curTok.getIdentifier().value();
  getNextToken();  // eat interface name

  // Parse optional type parameters: interface Name<T, U, ...>
  std::vector<std::string> typeParameters;
  if (curTok.kind == TokenKind::LESS) {
    getNextToken();  // eat '<'

    // Parse comma-separated list of type parameter names
    while (curTok.kind == TokenKind::IDENTIFIER) {
      typeParameters.push_back(curTok.getIdentifier().value());
      getNextToken();  // eat type parameter name

      if (curTok.kind == TokenKind::COMMA) {
        getNextToken();  // eat ','
      } else {
        break;
      }
    }

    if (typeParameters.empty()) {
      parsingError("expected type parameter name after '<'");
      return nullptr;
    }

    if (curTok.kind != TokenKind::GREATER) {
      parsingError("expected '>' after type parameters");
      return nullptr;
    }
    getNextToken();  // eat '>'
  }

  if (curTok.kind != TokenKind::BRACE_OPEN) {
    parsingError("expected '{' after interface name");
    return nullptr;
  }
  getNextToken();  // eat '{'

  std::vector<InterfaceFieldDecl> fields;
  std::vector<InterfaceMethodDecl> methods;

  // Parse interface body (fields and methods)
  while (curTok.kind != TokenKind::BRACE_CLOSE &&
         curTok.kind != TokenKind::TOK_EOF) {
    if (curTok.kind == TokenKind::VAR) {
      // Parse field declaration: var name: type;
      getNextToken();  // eat 'var'

      if (curTok.kind != TokenKind::IDENTIFIER) {
        parsingError("expected field name in interface definition");
        return nullptr;
      }

      Position fieldLoc = curTok.start;  // Capture location before eating token
      std::string fieldName = curTok.getIdentifier().value();
      getNextToken();  // eat field name

      if (curTok.kind != TokenKind::COLON) {
        parsingError(
            "expected ':' after field name (type annotation required)");
        return nullptr;
      }
      getNextToken();  // eat ':'

      TypeAnnotation fieldType = parseTypeAnnotation();

      if (curTok.kind != TokenKind::SEMI_COLON) {
        parsingError("expected ';' after field declaration");
        return nullptr;
      }
      getNextToken();  // eat ';'

      fields.push_back({std::move(fieldName), std::move(fieldType), fieldLoc});
    } else if (curTok.kind == TokenKind::FUNCTION) {
      // Parse method declaration (may have default implementation)
      // Interface methods can be:
      // 1. Just a signature: function name(args) returnType;
      // 2. Full method with default impl: function name(args) returnType { body
      // }

      // Capture start position for source text extraction (for generic methods)
      int methodStartOffset = curTok.start.offset;

      getNextToken();  // eat 'function'

      // Allow both regular identifiers and intrinsic identifiers
      if (curTok.kind != TokenKind::IDENTIFIER &&
          curTok.kind != TokenKind::INTRINSIC_IDENTIFIER) {
        parsingError("Expected method name in interface");
        return nullptr;
      }

      std::string funcName = curTok.getIdentifier().value();
      getNextToken();  // eat function name

      // Parse optional type parameters: function name<T, U>(...)
      std::vector<std::string> typeParameters;
      if (curTok.kind == TokenKind::LESS) {
        getNextToken();  // eat '<'

        // Parse comma-separated list of type parameter names
        while (curTok.kind == TokenKind::IDENTIFIER) {
          typeParameters.push_back(curTok.getIdentifier().value());
          getNextToken();  // eat type parameter name

          if (curTok.kind == TokenKind::COMMA) {
            getNextToken();  // eat ','
          } else {
            break;
          }
        }

        if (typeParameters.empty()) {
          parsingError("expected type parameter name after '<'");
          return nullptr;
        }

        if (curTok.kind != TokenKind::GREATER) {
          parsingError("expected '>' after type parameters");
          return nullptr;
        }
        getNextToken();  // eat '>'
      }

      if (curTok.kind != TokenKind::PAREN_OPEN) {
        parsingError("Expected '(' in method declaration");
        return nullptr;
      }

      std::vector<std::pair<std::string, TypeAnnotation>> args;
      std::optional<std::string> variadicParamName;
      std::optional<TypeAnnotation> variadicConstraint;
      getNextToken();  // eat '('

      if (curTok.kind != TokenKind::PAREN_CLOSE) {
        while (curTok.kind == TokenKind::IDENTIFIER) {
          std::string argName = curTok.getIdentifier().value();
          getNextToken();  // eat identifier

          // Check for variadic parameter: args... or args...: _init_args<T>
          if (curTok.kind == TokenKind::ELLIPSIS) {
            variadicParamName = argName;
            getNextToken();  // eat '...'

            // Check for optional constraint: args...: _init_args<T>
            if (curTok.kind == TokenKind::COLON) {
              getNextToken();  // eat ':'
              variadicConstraint = parseTypeAnnotation();
            }

            // Variadic param must be last - break out of loop
            break;
          }

          // Type annotation is required: arg: type
          if (curTok.kind != TokenKind::COLON) {
            parsingError("Expected ':' and type annotation for argument '" +
                         argName + "'");
          }
          getNextToken();  // eat ':'
          auto argType = parseTypeAnnotation();

          args.emplace_back(std::move(argName), std::move(argType));

          if (curTok.kind == TokenKind::COMMA)
            getNextToken();
          else
            break;
        }
      }

      if (curTok.kind != TokenKind::PAREN_CLOSE) {
        parsingError("Expected ')' in method declaration");
        return nullptr;
      }
      getNextToken();  // eat ')'

      // Check for return type
      std::optional<TypeAnnotation> retType;
      if (curTok.kind != TokenKind::BRACE_OPEN &&
          curTok.kind != TokenKind::SEMI_COLON) {
        retType = parseTypeAnnotation();
      }

      bool hasDefaultImpl = false;
      std::unique_ptr<BlockExprAST> body;

      if (curTok.kind == TokenKind::BRACE_OPEN) {
        // Has default implementation
        body = parseBlock();
        if (!body) return nullptr;
        hasDefaultImpl = true;
      } else if (curTok.kind == TokenKind::SEMI_COLON) {
        // Just a signature, no implementation
        getNextToken();  // eat ';'
        // Create an empty body
        body = std::make_unique<BlockExprAST>(
            std::vector<std::unique_ptr<ExprAST>>());
        hasDefaultImpl = false;
      } else {
        parsingError("Expected '{' or ';' after method signature in interface");
        return nullptr;
      }

      auto proto = std::make_unique<PrototypeAST>(
          funcName, std::move(args), std::move(retType),
          std::move(typeParameters), std::move(variadicParamName),
          std::move(variadicConstraint));
      auto func =
          std::make_unique<FunctionAST>(std::move(proto), std::move(body));

      // For generic methods with default impl, capture the source text
      if (func->getProto().isGeneric() && hasDefaultImpl) {
        int methodEndOffset = curTok.start.offset;
        std::string sourceText =
            getSourceText(methodStartOffset, methodEndOffset);
        func->setSourceText(sourceText);
      }

      InterfaceMethodDecl method;
      method.function = std::move(func);
      method.hasDefaultImpl = hasDefaultImpl;
      methods.push_back(std::move(method));

      // Skip optional semicolons after methods
      while (curTok.kind == TokenKind::SEMI_COLON) getNextToken();
    } else {
      parsingError(
          "expected 'var' (field) or 'function' (method) in interface body");
      return nullptr;
    }
  }

  if (curTok.kind != TokenKind::BRACE_CLOSE) {
    parsingError("expected '}' at end of interface definition");
    return nullptr;
  }
  getNextToken();  // eat '}'

  return std::make_unique<InterfaceDefinitionAST>(
      std::move(interfaceName), std::move(typeParameters), std::move(fields),
      std::move(methods));
}

// Parse enum definition: enum Name { Variant1, Variant2, ... }
// Syntax: enum ColorName { Red, Green, Blue }
unique_ptr<EnumDefinitionAST> Parser::parseEnumDefinition() {
  getNextToken();  // eat 'enum'

  if (curTok.kind != TokenKind::IDENTIFIER) {
    parsingError("expected enum name after 'enum'");
    return nullptr;
  }

  std::string enumName = curTok.getIdentifier().value();
  getNextToken();  // eat enum name

  if (curTok.kind != TokenKind::BRACE_OPEN) {
    parsingError("expected '{' after enum name");
    return nullptr;
  }
  getNextToken();  // eat '{'

  std::vector<EnumVariantDecl> variants;
  int64_t nextValue = 0;  // Auto-incrementing value for variants

  // Parse enum variants: Variant1, Variant2, ...
  while (curTok.kind != TokenKind::BRACE_CLOSE &&
         curTok.kind != TokenKind::TOK_EOF) {
    if (curTok.kind != TokenKind::IDENTIFIER) {
      parsingError("expected variant name in enum definition");
      return nullptr;
    }

    Position variantLoc = curTok.start;
    std::string variantName = curTok.getIdentifier().value();
    getNextToken();  // eat variant name

    int64_t variantValue = nextValue++;

    // TODO: Support explicit value assignment: Red = 1
    // For now, just auto-increment

    variants.push_back({std::move(variantName), variantValue, variantLoc});

    // Handle optional comma between variants
    if (curTok.kind == TokenKind::COMMA) {
      getNextToken();  // eat ','
    } else if (curTok.kind != TokenKind::BRACE_CLOSE) {
      // Allow trailing comma or no comma before '}'
      parsingError("expected ',' or '}' after enum variant");
      return nullptr;
    }
  }

  if (curTok.kind != TokenKind::BRACE_CLOSE) {
    parsingError("expected '}' at end of enum definition");
    return nullptr;
  }
  getNextToken();  // eat '}'

  if (variants.empty()) {
    parsingError("enum must have at least one variant");
    return nullptr;
  }

  return std::make_unique<EnumDefinitionAST>(std::move(enumName),
                                             std::move(variants));
}

// Parse throw expression: throw <expr>
// Syntax: throw errorExpr;
unique_ptr<ExprAST> Parser::parseThrow() {
  getNextToken();  // eat 'throw'

  // Parse the error expression being thrown
  auto errorExpr = parseExpression();
  if (!errorExpr) {
    parsingError("expected expression after 'throw'");
    return nullptr;
  }

  return std::make_unique<ThrowExprAST>(std::move(errorExpr));
}

// Parse try-catch expression: try { ... } catch (e: IError) { ... }
// Note: 'try' has already been consumed; we're at '{'
unique_ptr<ExprAST> Parser::parseTryCatch() {
  // Parse try block - we're already at '{'
  auto tryBlock = parseBlock();
  if (!tryBlock) {
    parsingError("expected block after 'try'");
    return nullptr;
  }

  // Expect 'catch'
  if (curTok.kind != TokenKind::CATCH) {
    parsingError("expected 'catch' after try block");
    return nullptr;
  }
  getNextToken();  // eat 'catch'

  // Parse catch clause: (name: Type)
  if (curTok.kind != TokenKind::PAREN_OPEN) {
    parsingError("expected '(' after 'catch'");
    return nullptr;
  }
  getNextToken();  // eat '('

  CatchClause catchClause;

  if (curTok.kind != TokenKind::IDENTIFIER) {
    parsingError("expected binding name in catch clause");
    return nullptr;
  }

  catchClause.bindingName = curTok.getIdentifier().value();
  getNextToken();  // eat identifier

  if (curTok.kind != TokenKind::COLON) {
    parsingError("expected ':' after binding name in catch clause");
    return nullptr;
  }
  getNextToken();  // eat ':'

  catchClause.bindingType = parseTypeAnnotation();

  if (curTok.kind != TokenKind::PAREN_CLOSE) {
    parsingError("expected ')' after catch binding");
    return nullptr;
  }
  getNextToken();  // eat ')'

  // Parse catch body
  if (curTok.kind != TokenKind::BRACE_OPEN) {
    parsingError("expected '{' to start catch body");
    return nullptr;
  }

  catchClause.body = parseBlock();
  if (!catchClause.body) return nullptr;

  return std::make_unique<TryCatchExprAST>(std::move(tryBlock),
                                           std::move(catchClause));
}