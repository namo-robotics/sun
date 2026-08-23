// src/parsing/parser.cpp
#include "parsing/parser.h"


#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "serialization/ast_deserializer.h"
#include "parsing/interpolated_string_parser.h"
#include "moon_bundling/library_cache.h"
#include "llvm/Support/raw_ostream.h"
#include "moon_bundling/metadata_extractor.h"
#include "moon.pb.h"
#include "support/sun_path.h"

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
  Position start = captureStart();
  std::unique_ptr<ExprAST> result;
  if (curTok.kind == TokenKind::INTEGER) {
    result = std::make_unique<NumberExprAST>(curTok.getInteger().value());
  } else {
    result = std::make_unique<NumberExprAST>(curTok.getFloat().value());
  }
  getNextToken();  // consume the number
  return finishNode(std::move(result), start);
}

std::unique_ptr<ExprAST> Parser::parseStringLiteral() {
  Position start = captureStart();
  auto result = std::make_unique<StringLiteralAST>(curTok.getString().value());
  getNextToken();  // consume the string
  return finishNode(std::move(result), start);
}

std::unique_ptr<ExprAST> Parser::parseArrayLiteral() {
  assert(curTok.kind == TokenKind::BRACKET_OPEN);
  Position start = captureStart();
  getNextToken();  // eat '['

  std::vector<std::unique_ptr<ExprAST>> elements;

  // Handle empty array
  if (curTok.kind == TokenKind::BRACKET_CLOSE) {
    getNextToken();  // eat ']'
    return finishNode(std::make_unique<ArrayLiteralAST>(std::move(elements)),
                      start);
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
  return finishNode(std::make_unique<ArrayLiteralAST>(std::move(elements)),
                    start);
}

std::unique_ptr<ExprAST> Parser::parseParenExpr() {
  assert(curTok.kind == TokenKind::PAREN_OPEN);
  Position start = captureStart();
  getNextToken();  // eat (
  auto v = parseExpression();
  if (!v) return nullptr;

  expectCurrentTokenKind(TokenKind::PAREN_CLOSE, "expected ')'");
  getNextToken();  // eat )
  return finishNode(std::make_unique<ParenExprAST>(std::move(v)), start);
}

unique_ptr<IfExprAST> Parser::parseIfStatement() {
  Position start = captureStart();
  getNextToken();  // eat the 'if'

  auto Cond = parseExpression();
  if (!Cond) return nullptr;

  // Require curly braces for then block
  expectCurrentTokenKind(TokenKind::BRACE_OPEN,
                         "expected '{' after if condition");

  auto ThenBlock = parseBlock();
  if (!ThenBlock) return nullptr;

  // Body kept as a block for losslessness; LoweringPass normalizes it
  unique_ptr<ExprAST> Then = std::move(ThenBlock);

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
      expectCurrentTokenKind(TokenKind::BRACE_OPEN, "expected '{' after else");

      auto ElseBlock = parseBlock();
      if (!ElseBlock) return nullptr;
      Else = std::move(ElseBlock);
    }
  }
  // No else - Else remains nullptr

  return finishNode(std::make_unique<IfExprAST>(std::move(Cond),
                                                std::move(Then),
                                                std::move(Else)),
                    start);
}

// Parse a match arm pattern: '_', an enum-variant path with an optional
// binding list (Shape.Circle(r, _)), or a plain unary expression compared by
// equality. A dotted path is parsed manually so a trailing '(' means
// destructuring bindings rather than a call expression.
Parser::ParsedPattern Parser::parsePattern() {
  ParsedPattern result;

  if (curTok.kind == TokenKind::UNDERSCORE) {
    getNextToken();  // eat '_'
    result.isWildcard = true;
    result.ok = true;
    return result;
  }

  if (curTok.kind == TokenKind::IDENTIFIER) {
    // Save state: if this is not a dotted path, backtrack to parseUnary
    Token savedCurTok = curTok;
    Token savedPrevTok = prevTok_;
    auto savedLexerPos = lexer.getPosition();
    auto savedTokenStack = tokenStack;

    Position start = captureStart();
    Position idLoc = start;
    idLoc.setEnd(curTok.end.line, curTok.end.column, curTok.end.offset);
    std::string firstName = curTok.getIdentifier().value();
    getNextToken();  // eat identifier

    if (curTok.kind == TokenKind::DOT) {
      auto base = std::make_unique<VariableReferenceAST>(firstName);
      base->setLocation(idLoc);
      std::unique_ptr<ExprAST> path = std::move(base);

      while (curTok.kind == TokenKind::DOT) {
        getNextToken();  // eat '.'
        if (curTok.kind != TokenKind::IDENTIFIER) {
          throwIdentifierError("expected identifier after '.' in match pattern");
          return result;
        }
        std::string member = curTok.getIdentifier().value();
        getNextToken();  // eat member name
        path = finishNode(
            std::make_unique<MemberAccessAST>(std::move(path), member), start);
      }

      // Optional payload binding list: (a, _, b)
      if (curTok.kind == TokenKind::PAREN_OPEN) {
        getNextToken();  // eat '('
        result.hasPayloadParens = true;
        if (curTok.kind == TokenKind::PAREN_CLOSE) {
          parsingError("payload pattern requires at least one binding");
          return result;
        }
        while (true) {
          PatternBinding binding;
          binding.location = captureStart();
          binding.location.setEnd(curTok.end.line, curTok.end.column,
                                  curTok.end.offset);
          if (curTok.kind == TokenKind::UNDERSCORE) {
            binding.isWildcard = true;
            getNextToken();  // eat '_'
          } else if (curTok.kind == TokenKind::IDENTIFIER) {
            binding.name = curTok.getIdentifier().value();
            getNextToken();  // eat binding name
          } else {
            throwIdentifierError(
                "match patterns bind payloads to fresh names; nested "
                "patterns and expressions are not supported here");
            return result;
          }
          result.bindings.push_back(std::move(binding));
          if (curTok.kind != TokenKind::COMMA) break;
          getNextToken();  // eat ','
        }
        expectCurrentTokenKind(TokenKind::PAREN_CLOSE,
                               "expected ')' after pattern bindings");
        getNextToken();  // eat ')'
      }

      result.pattern = std::move(path);
      result.ok = true;
      return result;
    }

    // Not a dotted path — backtrack and parse as a plain expression
    curTok = savedCurTok;
    prevTok_ = savedPrevTok;
    lexer.setPosition(savedLexerPos);
    tokenStack = savedTokenStack;
  }

  result.pattern = parseUnary();
  result.ok = result.pattern != nullptr;
  return result;
}

// Parse match expression: match value { pattern => expr, ... }
unique_ptr<MatchExprAST> Parser::parseMatchExpression() {
  Position start = captureStart();
  getNextToken();  // eat 'match'

  // Parse the discriminant expression
  auto discriminant = parseExpression();
  if (!discriminant) {
    parsingError("expected expression after 'match'");
    return nullptr;
  }

  // Expect opening brace
  expectCurrentTokenKind(TokenKind::BRACE_OPEN,
                         "expected '{' after match expression");
  getNextToken();  // eat '{'

  // Parse match arms
  std::vector<MatchArm> arms;
  while (curTok.kind != TokenKind::BRACE_CLOSE) {
    ParsedPattern parsed = parsePattern();
    if (!parsed.ok) {
      parsingError("expected pattern in match arm");
      return nullptr;
    }

    // Expect =>
    expectCurrentTokenKind(TokenKind::FAT_ARROW,
                           "expected '=>' after pattern in match arm");
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

    arms.emplace_back(std::move(parsed.pattern), parsed.isWildcard,
                      std::move(body));
    arms.back().hasPayloadParens = parsed.hasPayloadParens;
    arms.back().bindings = std::move(parsed.bindings);

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

  return finishNode(
      std::make_unique<MatchExprAST>(std::move(discriminant), std::move(arms)),
      start);
}

// Parse function: function name(args) returnType { body }
// or: function name<T, U>(args) returnType { body }
unique_ptr<FunctionAST> Parser::parseFunction() {
  Position start = captureStart();
  getNextToken();  // eat 'function'

  // Allow both regular identifiers and intrinsic identifiers (e.g., __index__)
  if (curTok.kind != TokenKind::IDENTIFIER &&
      curTok.kind != TokenKind::INTRINSIC_IDENTIFIER)
    throwIdentifierError("Expected function name after 'function'");

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
      throwIdentifierError("expected type parameter name after '<'");
    }

    expectCurrentTokenKind(TokenKind::GREATER,
                           "expected '>' after type parameters");
    getNextToken();  // eat '>'
  }

  auto result =
      parseFunctionLiteral(funcName, std::move(typeParameters), false);
  return finishNode(
      unique_ptr<FunctionAST>(static_cast<FunctionAST*>(result.release())),
      start);
}

// Parse lambda: lambda [ref x, ref y] (args) returnType { body }
// The optional bracketed list declares by-reference captures; all other
// captures are by value.
unique_ptr<LambdaAST> Parser::parseLambda() {
  Position lambdaLoc = captureStart();
  getNextToken();  // eat 'lambda'

  // Optional capture list: [ ref IDENT (, ref IDENT)* ]
  std::vector<std::string> refCaptureNames;
  if (curTok.kind == TokenKind::BRACKET_OPEN) {
    getNextToken();  // eat '['
    while (curTok.kind != TokenKind::BRACKET_CLOSE) {
      if (curTok.kind != TokenKind::REF) {
        parsingError(
            "expected 'ref' in lambda capture list (only by-reference "
            "captures are declared, e.g. [ref x])");
      }
      getNextToken();  // eat 'ref'

      if (curTok.kind != TokenKind::IDENTIFIER) {
        throwIdentifierError("expected variable name after 'ref' in capture list");
      }
      refCaptureNames.push_back(curTok.getIdentifier().value());
      getNextToken();  // eat identifier

      if (curTok.kind == TokenKind::COMMA) {
        getNextToken();  // eat ','
      } else if (curTok.kind != TokenKind::BRACKET_CLOSE) {
        parsingError("expected ',' or ']' in lambda capture list");
      }
    }
    getNextToken();  // eat ']'
  }

  auto result = parseFunctionLiteral("", {}, true);  // anonymous function
  auto lambda = unique_ptr<LambdaAST>(static_cast<LambdaAST*>(result.release()));
  lambda = finishNode(std::move(lambda), std::move(lambdaLoc));
  if (lambda) {
    if (!refCaptureNames.empty()) {
      const_cast<PrototypeAST&>(lambda->getProto())
          .setRefCaptureNames(std::move(refCaptureNames));
    }
  }
  return lambda;
}

// Parse a function literal: (args) returnType { body }
unique_ptr<ExprAST> Parser::parseFunctionLiteral(
    const std::string& name, std::vector<std::string> typeParameters,
    bool isLambda) {
  Position start = captureStart();
  expectCurrentTokenKind(TokenKind::PAREN_OPEN,
                         "Expected '(' in function literal");

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
      expectCurrentTokenKind(
          TokenKind::COLON,
          "Expected ':' and type annotation for argument '" + argName + "'");
      getNextToken();  // eat ':'
      auto argType = parseTypeAnnotation();

      args.emplace_back(std::move(argName), std::move(argType));

      if (curTok.kind == TokenKind::COMMA)
        getNextToken();
      else
        break;
    }
  }

  // A bare `...` in a definition's parameter list is C varargs. Sun has no
  // va_arg, so only extern declarations (parsed via parsePrototype) can use it.
  if (curTok.kind == TokenKind::ELLIPSIS) {
    parsingError(
        "C varargs ('...') are only allowed on 'extern function' declarations");
  }

  if (curTok.kind != TokenKind::PAREN_CLOSE) {
    throwIdentifierError("Expected ')' in function literal");
  }

  getNextToken();  // eat ')'

  // Check for return type (no arrow, type comes directly after parentheses)
  // Syntax: function foo(args) ReturnType, IError { ... }
  // Return type is required for all functions except 'init' methods
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
        // Extend the span over the ", IError" consumed here
        retType->span.setEnd(prevTok_.end.line, prevTok_.end.column,
                             prevTok_.end.offset);
      }
    }
  } else {
    // No return type specified - only allowed for 'init' methods
    if (name == "init") {
      // init methods implicitly return void
      retType = TypeAnnotation("void");
    } else if (isLambda) {
      parsingError("Return type is required for lambda expression");
    } else {
      parsingError("Return type is required for function '" + name + "'");
    }
  }

  // Signature span ends at the last token before the body
  Position protoLoc = start;
  protoLoc.setEnd(prevTok_.end.line, prevTok_.end.column, prevTok_.end.offset);

  // Parse function body
  expectCurrentTokenKind(TokenKind::BRACE_OPEN,
                         "Expected '{' to start function body");

  auto body = parseBlock();
  if (!body) return nullptr;

  auto proto = std::make_unique<PrototypeAST>(
      name, std::move(args), std::move(retType), std::move(typeParameters),
      std::move(variadicParamName), std::move(variadicConstraint));
  proto->setLocation(std::move(protoLoc));
  if (isLambda) {
    return finishNode(
        std::make_unique<LambdaAST>(std::move(proto), std::move(body)), start);
  } else {
    return finishNode(
        std::make_unique<FunctionAST>(std::move(proto), std::move(body)),
        start);
  }
}

// Parse a struct literal: { color: "red", speed: 120 }
//
// Only reachable where an initializer is expected, so there is no ambiguity
// with a block: a bare block is not a value.
unique_ptr<StructLiteralAST> Parser::parseStructLiteral() {
  Position start = captureStart();
  getNextToken();  // eat '{'

  std::vector<StructLiteralAST::FieldInit> fields;

  while (curTok.kind != TokenKind::BRACE_CLOSE &&
         curTok.kind != TokenKind::TOK_EOF) {
    if (curTok.kind != TokenKind::IDENTIFIER) {
      throwIdentifierError("expected a field name in struct literal");
      return nullptr;
    }
    Position fieldLoc = captureStart();
    std::string fieldName = curTok.getIdentifier().value();
    getNextToken();  // eat field name

    expectCurrentTokenKind(
        TokenKind::COLON, "expected ':' after field name '" + fieldName + "'");
    getNextToken();  // eat ':'

    // A nested literal initializes a class-typed field:
    // { inner: { a: 1, b: 2 }, tag: 3 }
    unique_ptr<ExprAST> value;
    if (curTok.kind == TokenKind::BRACE_OPEN) {
      value = parseStructLiteral();
    } else {
      value = parseExpression();
    }
    if (!value) {
      parsingError("expected a value for field '" + fieldName + "'");
      return nullptr;
    }
    fieldLoc.setEnd(prevTok_.end.line, prevTok_.end.column,
                    prevTok_.end.offset);
    fields.push_back({std::move(fieldName), std::move(value),
                      std::move(fieldLoc)});

    if (curTok.kind == TokenKind::COMMA) {
      getNextToken();  // eat ','
    } else {
      break;
    }
  }

  expectCurrentTokenKind(TokenKind::BRACE_CLOSE,
                         "expected '}' at end of struct literal");
  getNextToken();  // eat '}'

  return finishNode(std::make_unique<StructLiteralAST>(std::move(fields)),
                    start);
}

// Internal helper that parses var declaration without consuming trailing
// semicolon
unique_ptr<VariableCreationAST> Parser::parseVarDeclaration() {
  Position start = captureStart();
  bool isConst = curTok.kind == TokenKind::CONST;
  getNextToken();  // eat 'var' / 'const'

  // At least one variable name is required
  expectCurrentTokenKind(
      TokenKind::IDENTIFIER,
      std::string("expected identifier after '") + (isConst ? "const" : "var") +
          "'");

  std::string name = std::get<std::string>(curTok.value);
  getNextToken();  // eat identifier

  // Optional type annotation: var x: i32 = ...
  std::optional<TypeAnnotation> typeAnnot;
  if (curTok.kind == TokenKind::COLON) {
    getNextToken();  // eat ':'
    typeAnnot = parseTypeAnnotation();
  }

  expectCurrentTokenKind(TokenKind::EQUAL,
                         "expected '=' after variable declaration");

  getNextToken();  // eat '='

  // A '{' here starts a struct literal; the target type comes from the
  // annotation, which semantic analysis checks is present.
  unique_ptr<ExprAST> value;
  if (curTok.kind == TokenKind::BRACE_OPEN) {
    value = parseStructLiteral();
  } else {
    // parseExpression handles function/lambda keywords automatically
    value = parseExpression();
  }
  if (!value) {
    parsingError("variable initialization expression expected");
  }

  return finishNode(std::make_unique<VariableCreationAST>(
                        name, std::move(value), std::move(typeAnnot), isConst),
                    start);
}

unique_ptr<VariableCreationAST> Parser::parseVarStatement() {
  auto decl = parseVarDeclaration();

  expectCurrentTokenKind(TokenKind::SEMI_COLON,
                         "expected ';' after variable declaration");
  getNextToken();  // eat ';'

  return decl;
}

unique_ptr<ReferenceCreationAST> Parser::parseRefStatement(Position refLoc,
                                                           bool isMutable) {
  getNextToken();  // eat the 'ref'

  // Require identifier
  expectCurrentTokenKind(TokenKind::IDENTIFIER,
                         "expected identifier after 'ref'");

  std::string name = std::get<std::string>(curTok.value);
  getNextToken();  // eat identifier

  expectCurrentTokenKind(TokenKind::EQUAL, "expected '=' after reference name");

  getNextToken();  // eat '='

  // Parse the target expression (must be an lvalue - variable reference)
  auto target = parseExpression();
  if (!target) {
    parsingError("reference target expression expected");
  }

  expectCurrentTokenKind(TokenKind::SEMI_COLON,
                         "expected ';' after reference declaration");
  getNextToken();  // eat ';'

  return finishNode(std::make_unique<ReferenceCreationAST>(
                        name, std::move(target), isMutable, refLoc),
                    refLoc);
}

unique_ptr<ExprAST> Parser::parseConstStatement() {
  Position start = captureStart();
  Token constTok = curTok;
  getNextToken();  // eat 'const'

  switch (curTok.kind) {
    case TokenKind::REF:
      return parseRefStatement(start, /*isMutable=*/false);
    case TokenKind::IDENTIFIER:
      // parseVarDeclaration eats the declaring keyword itself
      pushToken(constTok);
      return parseVarStatement();
    case TokenKind::FUNCTION:
      parsingError(
          "'const function' is only allowed on class and interface methods");
    default:
      throwIdentifierError("expected a variable name or 'ref' after 'const'");
  }
}

unique_ptr<ExprAST> Parser::parseIdentifierExpr() {
  std::string idName = std::get<std::string>(curTok.value);
  // Create position with file path and end position
  Position idLoc = captureStart();
  idLoc.setEnd(curTok.end.line, curTok.end.column, curTok.end.offset);

  getNextToken();  // eat identifier

  // Check for pack expansion: args...
  if (curTok.kind == TokenKind::ELLIPSIS) {
    getNextToken();  // eat '...'
    return finishNode(std::make_unique<PackExpansionAST>(std::move(idName)),
                      idLoc);
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
    Token savedPrevTok = prevTok_;
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

      // Handle '>' or '>>' (for nested generics)
      if (isGreater()) {
        // Consume '>' (splits '>>', '>=', '>>=' if needed)
        consumeGreater("expected '>' after generic type arguments");

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
          return finishNode(
              std::make_unique<GenericCallAST>(
                  std::move(idName), std::move(typeArgs), std::move(args)),
              idLoc);
        }
      }
    }

    // Not a generic call - backtrack to before '<'
    curTok = savedCurTok;
    prevTok_ = savedPrevTok;
    lexer.setPosition(savedLexerPos);
    tokenStack = savedTokenStack;
  }

  // Just return a variable reference - call handling is done in postfix
  // parsing This allows for first-class function support where variables can
  // hold functions
  auto varRef = std::make_unique<VariableReferenceAST>(idName);
  varRef->setLocation(idLoc);  // end already set from the identifier token
  return varRef;
}

// Prefix operators: - (negation), not (logical), ~ (bitwise). Unary + is a
// no-op. Right-recursive, so `-(-x)` and `not not b` parse naturally; binds
// tighter than any binary operator.
unique_ptr<ExprAST> Parser::parseUnary() {
  if (curTok.kind == TokenKind::PLUS) {
    getNextToken();  // eat '+'
    return parseUnary();
  }

  if (curTok.kind == TokenKind::MINUS || curTok.kind == TokenKind::NOT ||
      curTok.kind == TokenKind::TILDE) {
    Position start = captureStart();
    Token opTok = curTok;
    getNextToken();  // eat the operator

    auto operand = parseUnary();
    if (!operand) return nullptr;

    return finishNode(
        std::make_unique<UnaryExprAST>(opTok, std::move(operand)), start);
  }

  return parsePrimary();
}

unique_ptr<ExprAST> Parser::parsePrimary() {
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
    case TokenKind::TEMPLATE_STRING: {
      // Parse interpolated template string: `Hello ${name}!`
      std::string content = curTok.getTemplateString().value();
      Position tokStart = curTok.start;
      Position tokEnd = curTok.end;
      getNextToken();  // consume the template string token
      usesStringInterpolation_ = true;
      base = InterpolatedStringParser::parseToAst(content, tokStart, tokEnd,
                                                  currentFilePath);
      break;
    }
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
    case TokenKind::CONST:
      base = parseConstStatement();
      break;
    case TokenKind::THIS: {
      Position start = captureStart();
      base = std::make_unique<ThisExprAST>();
      getNextToken();  // eat 'this'
      base = finishNode(std::move(base), start);
      break;
    }
    case TokenKind::FUNCTION:
      parsingError(
          "'function' cannot be used as an expression; use 'lambda' instead");
      break;
    case TokenKind::LAMBDA:
      base = parseLambda();
      break;
    case TokenKind::TRY: {
      // try { ... } catch (e: IError) { ... } syntax
      Position start = captureStart();
      getNextToken();  // eat 'try'
      if (curTok.kind != TokenKind::BRACE_OPEN) {
        parsingError("expected '{' after 'try'");
        return nullptr;
      }
      base = finishNode(parseTryCatch(), start);
      break;
    }
    case TokenKind::THROW:
      base = parseThrow();
      break;
    case TokenKind::SPAWN:
      base = parseSpawn();
      break;
    case TokenKind::UNSAFE:
      base = parseUnsafeBlock();
      break;
    case TokenKind::NULL_LITERAL: {
      Position start = captureStart();
      base = std::make_unique<NullLiteralAST>();
      getNextToken();  // eat 'null'
      base = finishNode(std::move(base), start);
      break;
    }
    case TokenKind::TRUE_LITERAL: {
      Position start = captureStart();
      base = std::make_unique<BoolLiteralAST>(true);
      getNextToken();  // eat 'true'
      base = finishNode(std::move(base), start);
      break;
    }
    case TokenKind::FALSE_LITERAL: {
      Position start = captureStart();
      base = std::make_unique<BoolLiteralAST>(false);
      getNextToken();  // eat 'false'
      base = finishNode(std::move(base), start);
      break;
    }
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
          Position compStart = captureStart();
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
            indices.push_back(finishNode(
                std::make_unique<SliceExprAST>(std::move(start),
                                               std::move(end), true),
                compStart));
          } else {
            indices.push_back(finishNode(
                std::make_unique<SliceExprAST>(std::move(start)), compStart));
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

      Position baseStart = base->getLocation();
      base = std::make_unique<IndexAST>(std::move(base), std::move(indices));
      extendSpan(*base, baseStart);
    } else if (curTok.kind == TokenKind::DOT) {
      getNextToken();  // eat '.'

      if (curTok.kind != TokenKind::IDENTIFIER) {
        throwIdentifierError("expected member name after '.'");
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
        Token savedPrevTok = prevTok_;
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

          // Handle '>' or '>>' (for nested generics)
          if (isGreater()) {
            // Consume '>' (splits '>>', '>=', '>>=' if needed)
            consumeGreater("expected '>' after generic type arguments");

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
          prevTok_ = savedPrevTok;
          lexer.setPosition(savedLexerPos);
          tokenStack = savedTokenStack;
        }
      }

      Position baseStart = base->getLocation();
      base = std::make_unique<MemberAccessAST>(
          std::move(base), std::move(memberName), std::move(typeArgs));
      extendSpan(*base, baseStart);
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
            expectCurrentTokenKind(TokenKind::PAREN_CLOSE,
                                   "Expected ')' or ',' in argument list");

          getNextToken();  // eat ','
        }
      }
      getNextToken();  // eat ')'

      // Create a unified call expression (callee is an expression)
      // The call spans from the callee's start to the closing ')'
      Position callLoc = base->getLocation();
      base = std::make_unique<CallExprAST>(std::move(base), std::move(args));
      extendSpan(*base, callLoc);
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
    case TokenKind::REF:
    case TokenKind::CONST:
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
  Position start = captureStart();
  TypeAnnotation type = parseTypeAnnotationImpl();
  start.setEnd(prevTok_.end.line, prevTok_.end.column, prevTok_.end.offset);
  type.span = std::move(start);
  return type;
}

TypeAnnotation Parser::parseTypeAnnotationImpl() {
  TypeAnnotation type;

  // Check for function type: _(param_types) return_type (named function)
  if (curTok.kind == TokenKind::UNDERSCORE) {
    type.baseName = "fn";
    getNextToken();  // eat '_'

    expectCurrentTokenKind(TokenKind::PAREN_OPEN,
                           "expected '(' after '_' in function type");
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

    expectCurrentTokenKind(TokenKind::PAREN_CLOSE,
                           "expected ')' in function type");
    getNextToken();  // eat ')'

    type.returnType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    return type;
  }

  // Check for lambda type: (param_types) return_type[, IError]
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

    expectCurrentTokenKind(TokenKind::PAREN_CLOSE,
                           "expected ')' in lambda type");
    getNextToken();  // eat ')'

    type.returnType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    // Throwing lambda type: (params) ret, IError. A ',' here is ambiguous
    // (may belong to an enclosing param list), so only consume it when the
    // next token is exactly 'IError'.
    if (curTok.kind == TokenKind::COMMA) {
      Token commaTok = curTok;
      Token savedPrev = prevTok_;
      getNextToken();  // tentatively eat ','
      auto id = curTok.getIdentifier();
      if (curTok.kind == TokenKind::IDENTIFIER && id.has_value() &&
          id.value() == "IError") {
        getNextToken();  // eat 'IError'
        type.canError = true;
      } else {
        pushToken(commaTok);  // not ours - restore the ','
        prevTok_ = savedPrev;  // keep spans ending at the real last token
      }
    }

    return type;
  }

  if (curTok.kind == TokenKind::PTR) {
    // ptr<elementType> - owning pointer with unique_ptr semantics
    type.baseName = "ptr";
    getNextToken();  // eat 'ptr'

    expectCurrentTokenKind(TokenKind::LESS, "expected '<' after 'ptr'");
    getNextToken();  // eat '<'

    // Parse pointee type
    type.elementType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    consumeGreater("expected '>' after ptr type");

    return type;
  }

  if (curTok.kind == TokenKind::RAW_PTR) {
    // raw_ptr<elementType> - non-owning pointer for C interop
    type.baseName = "raw_ptr";
    getNextToken();  // eat 'raw_ptr'

    expectCurrentTokenKind(TokenKind::LESS, "expected '<' after 'raw_ptr'");
    getNextToken();  // eat '<'

    // Parse pointee type
    type.elementType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    consumeGreater("expected '>' after raw_ptr type");

    return type;
  }

  if (curTok.kind == TokenKind::STATIC_PTR) {
    // static_ptr<elementType> - pointer to immortal static data
    type.baseName = "static_ptr";
    getNextToken();  // eat 'static_ptr'

    expectCurrentTokenKind(TokenKind::LESS, "expected '<' after 'static_ptr'");
    getNextToken();  // eat '<'

    // Parse pointee type
    type.elementType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    consumeGreater("expected '>' after static_ptr type");

    return type;
  }

  if (curTok.kind == TokenKind::CONST) {
    // const ref T - a reference whose referent cannot be changed
    getNextToken();  // eat 'const'
    expectCurrentTokenKind(TokenKind::REF,
                           "expected 'ref' after 'const' in a type (const ref T)");
    type.constRef = true;
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

    expectCurrentTokenKind(TokenKind::LESS, "expected '<' after 'array'");
    getNextToken();  // eat '<'

    // Parse element type
    type.elementType = std::make_unique<TypeAnnotation>(parseTypeAnnotation());

    // Parse dimensions (comma-separated integers)
    while (curTok.kind == TokenKind::COMMA) {
      getNextToken();  // eat ','

      expectCurrentTokenKind(TokenKind::INTEGER,
                             "expected integer dimension in array type");

      type.arrayDimensions.push_back(
          static_cast<size_t>(curTok.getInteger().value()));
      getNextToken();  // eat integer
    }

    consumeGreater("expected '>' after array type");

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

      expectCurrentTokenKind(TokenKind::IDENTIFIER,
                             "expected identifier after '.'");

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

      // Handle '>>' as two '>' for nested generics like Container<Container<T>>
      consumeGreater("expected '>' after generic type arguments");
    }
  }

  return type;
}

// True for '=' and every compound-assignment operator
static bool isAssignmentOp(TokenKind kind) {
  return kind == TokenKind::EQUAL || compoundToBinaryOp(kind).has_value();
}

// Shared tail for `name = rhs` / `name op= rhs` once the target identifier
// has been consumed and curTok is the assignment operator. Does not consume
// the trailing ';' (the for-loop increment has none).
unique_ptr<ExprAST> Parser::finishVariableAssignment(const std::string& name,
                                                     const Position& namePos) {
  Token opTok = curTok;
  getNextToken();  // eat '=' or 'op='
  auto value = parseExpression();
  if (!value) return nullptr;

  if (compoundToBinaryOp(opTok.kind)) {
    auto target = std::make_unique<VariableReferenceAST>(name);
    target->setLocation(namePos);
    return finishNode(std::make_unique<CompoundAssignmentAST>(
                          std::move(target), opTok, std::move(value)),
                      namePos);
  }
  return finishNode(
      std::make_unique<VariableAssignmentAST>(name, std::move(value)),
      namePos);
}

// Shared tail for `a.b = rhs` / `a.b op= rhs` / `this.f op= rhs` once the
// member-access target has been parsed and curTok is the assignment operator.
// Consumes the trailing ';'.
unique_ptr<ExprAST> Parser::finishMemberAssignment(unique_ptr<ExprAST> lhs) {
  Token opTok = curTok;
  getNextToken();  // eat '=' or 'op='
  auto value = parseExpression();
  if (!value) return nullptr;

  if (curTok.kind == TokenKind::SEMI_COLON)
    getNextToken();
  else
    parsingError("expected ';' after member assignment");

  Position start = lhs->getLocation();
  if (compoundToBinaryOp(opTok.kind)) {
    // Compound: keep the whole member access as the assignment target
    return finishNode(std::make_unique<CompoundAssignmentAST>(
                          std::move(lhs), opTok, std::move(value)),
                      start);
  }

  // Extract object and member from MemberAccessAST; releaseObject preserves
  // the full chain (e.g. this.a.b)
  auto* memberAccess = static_cast<MemberAccessAST*>(lhs.get());
  std::string memberName = memberAccess->getMemberName();
  auto object = memberAccess->releaseObject();

  return finishNode(
      std::make_unique<MemberAssignmentAST>(
          std::move(object), std::move(memberName), std::move(value)),
      start);
}

unique_ptr<ExprAST> Parser::parseAssignmentOrExpression() {
  auto idToken = curTok;
  std::string idName = curTok.getIdentifier().value();

  getNextToken();  // eat identifier

  // Check for simple variable assignment: x = ... or x op= ...
  if (isAssignmentOp(curTok.kind)) {
    auto assign = finishVariableAssignment(idName, idToken.start);
    if (!assign) return nullptr;

    if (curTok.kind == TokenKind::SEMI_COLON)
      getNextToken();
    else
      parsingError("expected ';' after variable assignment");
    return assign;
  }

  // Not an assignment, parse as expression
  // Put back the identifier token for expression parsing
  pushToken(idToken);
  auto expr = parseExpression();

  // Check for indexed assignment: x[i] = value or x[i] op= value
  if (isAssignmentOp(curTok.kind) && expr->getType() == ASTNodeType::INDEX) {
    Token opTok = curTok;
    getNextToken();  // eat '=' or 'op='
    auto value = parseExpression();
    if (!value) return nullptr;

    if (curTok.kind == TokenKind::SEMI_COLON)
      getNextToken();
    else
      parsingError("expected ';' after indexed assignment");

    Position start = expr->getLocation();
    if (compoundToBinaryOp(opTok.kind)) {
      return finishNode(std::make_unique<CompoundAssignmentAST>(
                            std::move(expr), opTok, std::move(value)),
                        start);
    }
    return finishNode(std::make_unique<IndexedAssignmentAST>(std::move(expr),
                                                             std::move(value)),
                      start);
  }

  // Check for member assignment: a.b = value, a.b.c.d = value, a.b op= value
  if (isAssignmentOp(curTok.kind) &&
      expr->getType() == ASTNodeType::MEMBER_ACCESS) {
    return finishMemberAssignment(std::move(expr));
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

  auto lhs = parseUnary();
  if (!lhs) return nullptr;

  auto expr = parseBinOpRhs(0, std::move(lhs));
  if (!expr) return nullptr;

  // Ternary conditional: cond ? then : else (right-associative)
  if (curTok.kind == TokenKind::QUESTION) {
    Position qLoc = curTok.start;
    getNextToken();  // eat '?'
    auto thenExpr = parseExpression();
    if (!thenExpr) return nullptr;
    if (curTok.kind != TokenKind::COLON)
      parsingError("expected ':' in ternary expression");
    getNextToken();  // eat ':'
    auto elseExpr = parseExpression();
    if (!elseExpr) return nullptr;
    Position start = expr->getLocation();
    auto ternary = std::make_unique<TernaryExprAST>(
        std::move(expr), std::move(thenExpr), std::move(elseExpr), qLoc);
    extendSpan(*ternary, start);
    return ternary;
  }
  return expr;
}

std::unique_ptr<ExprAST> Parser::parseBinOpRhs(int exprPrec,
                                               std::unique_ptr<ExprAST> lhs) {
  while (true) {
    if (curTok.precedence < exprPrec) return lhs;

    Token binOp = curTok;
    getNextToken();  // eat binop

    // `&&` / `||` lex as two bitwise operators; point newcomers at `and`/`or`.
    if ((binOp.kind == TokenKind::AMPERSAND || binOp.kind == TokenKind::PIPE) &&
        curTok.kind == binOp.kind && curTok.start.offset == binOp.end.offset) {
      curTok = Token::make(binOp.kind, binOp.start, curTok.end);
      parsingError(binOp.kind == TokenKind::AMPERSAND
                       ? "unexpected '&&' - Sun spells logical and as 'and'"
                       : "unexpected '||' - Sun spells logical or as 'or'");
    }

    auto rhs = parseUnary();
    if (!rhs) return nullptr;

    if (binOp.precedence < curTok.precedence) {
      rhs = parseBinOpRhs(binOp.precedence + 1, std::move(rhs));
      if (!rhs) return nullptr;
    }

    Position start = lhs->getLocation();
    lhs =
        std::make_unique<BinaryExprAST>(binOp, std::move(lhs), std::move(rhs));
    extendSpan(*lhs, start);
  }
}

std::unique_ptr<PrototypeAST> Parser::parsePrototype() {
  Position start = captureStart();
  std::string fnName;

  // Allow both regular identifiers and intrinsic identifiers (e.g., __index__)
  if (curTok.kind != TokenKind::IDENTIFIER &&
      curTok.kind != TokenKind::INTRINSIC_IDENTIFIER) {
    throwIdentifierError("Expected function name in prototype");
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
      throwIdentifierError("expected type parameter name after '<'");
    }

    if (curTok.kind != TokenKind::GREATER) {
      parsingError("expected '>' after type parameters");
    }
    getNextToken();  // eat '>'
  }

  expectCurrentTokenKind(TokenKind::PAREN_OPEN, "Expected '(' in prototype");

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
    auto proto = std::make_unique<PrototypeAST>(
        fnName, std::move(args), std::move(retType), std::move(typeParameters));
    start.setEnd(prevTok_.end.line, prevTok_.end.column, prevTok_.end.offset);
    proto->setLocation(std::move(start));
    return proto;
  }

  std::optional<std::string> variadicParamName;
  std::optional<TypeAnnotation> variadicConstraint;

  bool cVariadic = false;

  while (curTok.kind == TokenKind::IDENTIFIER ||
         curTok.kind == TokenKind::ELLIPSIS) {
    // C-style trailing varargs: `fn(fmt: raw_ptr<u8>, ...)`. Distinct from
    // Sun's `args...` pack below, which binds a name.
    if (curTok.kind == TokenKind::ELLIPSIS) {
      if (args.empty()) {
        parsingError(
            "'...' must follow at least one named parameter; C varargs "
            "cannot be the only parameter");
      }
      cVariadic = true;
      getNextToken();  // eat '...'
      break;           // must be last
    }

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

  if (curTok.kind != TokenKind::PAREN_CLOSE) {
    throwIdentifierError("Expected ')' in prototype");
  }

  // success.
  getNextToken();  // eat ')'."

  // Check for return type (only if followed by a type token, not semicolon)
  // Type can start with: type keywords, '(' for lambda type, '_' for fn type
  std::optional<TypeAnnotation> retType;
  if (curTok.kind != TokenKind::BRACE_OPEN &&
      curTok.kind != TokenKind::SEMI_COLON &&
      (isTypeToken(curTok.kind) || curTok.kind == TokenKind::PAREN_OPEN ||
       curTok.kind == TokenKind::UNDERSCORE)) {
    retType = parseTypeAnnotation();
  }

  auto proto = std::make_unique<PrototypeAST>(
      fnName, std::move(args), std::move(retType), std::move(typeParameters),
      std::move(variadicParamName), std::move(variadicConstraint));
  proto->setCVariadic(cVariadic);
  start.setEnd(prevTok_.end.line, prevTok_.end.column, prevTok_.end.offset);
  proto->setLocation(std::move(start));
  return proto;
}

unique_ptr<BlockExprAST> Parser::parseBlock(bool itemLevel) {
  Position start = captureStart();
  std::vector<unique_ptr<ExprAST>> body;

  expectCurrentTokenKind(TokenKind::BRACE_OPEN, "expected '{'");
  getNextToken();  // eat {

  bool savedItemLevel = atItemLevel_;
  atItemLevel_ = itemLevel;
  while (curTok.kind != TokenKind::BRACE_CLOSE &&
         curTok.kind != TokenKind::TOK_EOF) {
    if (auto stmt = parseStatement()) {
      body.push_back(std::move(stmt));
    }
    // No error recovery needed - parseStatement handles its own token
    // consumption
  }
  atItemLevel_ = savedItemLevel;

  expectCurrentTokenKind(TokenKind::BRACE_CLOSE,
                         "expected '}' at end of block");

  getNextToken();  // eat }

  return finishNode(std::make_unique<BlockExprAST>(std::move(body)), start);
}

unique_ptr<BlockExprAST> Parser::parseProgram() {
  Position start = captureStart();
  std::vector<unique_ptr<ExprAST>> body;

  atItemLevel_ = true;
  while (curTok.kind != TokenKind::BRACE_CLOSE &&
         curTok.kind != TokenKind::TOK_EOF) {
    if (auto stmt = parseStatement()) {
      body.push_back(std::move(stmt));
    }
    // No error recovery needed - parseStatement handles its own token
    // consumption
  }

  return finishNode(std::make_unique<BlockExprAST>(std::move(body)), start);
}

bool Parser::parsePublic() {
  if (curTok.kind != TokenKind::PUBLIC) return false;
  getNextToken();  // eat 'public'
  if (curTok.kind == TokenKind::PUBLIC)
    parsingError("duplicate 'public' modifier");
  return true;
}

bool Parser::parseConstModifier() {
  if (curTok.kind != TokenKind::CONST) return false;
  getNextToken();  // eat 'const'
  if (curTok.kind == TokenKind::CONST)
    parsingError("duplicate 'const' modifier");
  if (curTok.kind == TokenKind::PUBLIC)
    parsingError("'public' must come before 'const'");
  return true;
}

// Statement kinds that may carry a `public` modifier at item level.
static bool isPublicableStatementStart(TokenKind kind) {
  switch (kind) {
    case TokenKind::MODULE:
    case TokenKind::PARTIAL:
    case TokenKind::PACKED_CLASS:
    case TokenKind::CLASS:
    case TokenKind::INTERFACE:
    case TokenKind::ENUM:
    case TokenKind::VAR:
    case TokenKind::CONST:
    case TokenKind::EXTERN:
    case TokenKind::FUNCTION:
    case TokenKind::DECLARE:
      return true;
    default:
      return false;
  }
}

unique_ptr<ExprAST> Parser::parseStatement() {
  if (curTok.kind != TokenKind::PUBLIC) return parseStatementCore();

  Position start = captureStart();
  parsePublic();
  if (!atItemLevel_)
    parsingError("'public' is only allowed on module-level declarations");
  if (!isPublicableStatementStart(curTok.kind))
    parsingError(
        "'public' must precede a declaration (module, class, interface, "
        "enum, function, extern, declare, var or const)");

  auto node = parseStatementCore();
  if (node) {
    if (node->getType() == ASTNodeType::REFERENCE_CREATION)
      parsingError("'public' cannot be applied to a reference");
    node->setVisibility(sun::Visibility::Public);
    // `public module a.b.c` desugars to nested modules sharing one span;
    // every synthesized level is public.
    for (auto* mod = dynamic_cast<ModuleAST*>(node.get()); mod;) {
      const auto& stmts = mod->getBody().getBody();
      auto* inner = stmts.size() == 1
                        ? dynamic_cast<ModuleAST*>(stmts.front().get())
                        : nullptr;
      if (!inner || inner->getLocation().offset != mod->getLocation().offset)
        break;
      inner->setVisibility(sun::Visibility::Public);
      mod = inner;
    }
    extendSpanStart(*node, start);  // span covers the modifier
  }
  return node;
}

unique_ptr<ExprAST> Parser::parseStatementCore() {
  switch (curTok.kind) {
    case TokenKind::DECLARE:
      return parseDeclareStatement();

    case TokenKind::MANIFEST:
      return parseManifest();

    case TokenKind::MODULE:
      return parseModuleDecl();

    case TokenKind::USING:
      return parseUsingStatement();

    case TokenKind::PARTIAL: {
      // partial class X { ... } - partial class (methods only)
      getNextToken();  // eat 'partial'
      if (curTok.kind == TokenKind::PACKED_CLASS) {
        // A partial carries methods only, so there is no layout to pack
        parsingError("'partial' cannot be combined with 'packed_class'");
        return nullptr;
      }
      if (curTok.kind != TokenKind::CLASS) {
        parsingError("expected 'class' after 'partial'");
        return nullptr;
      }
      auto classDef = parseClassDefinition();
      if (classDef) {
        classDef->setIsPartial(true);
        // Validate: no fields allowed in partial class
        if (!classDef->getFields().empty()) {
          parsingError("partial class cannot define fields");
          return nullptr;
        }
        // Validate: no constructors allowed in partial class
        for (const auto& method : classDef->getMethods()) {
          if (method.isConstructor) {
            parsingError("partial class cannot define constructors");
            return nullptr;
          }
        }
      }
      while (curTok.kind == TokenKind::SEMI_COLON)
        getNextToken();  // optional semicolons
      return classDef;
    }

    case TokenKind::PACKED_CLASS:
    case TokenKind::CLASS: {
      // packed_class lays fields out with no padding; otherwise identical
      bool isPacked = curTok.kind == TokenKind::PACKED_CLASS;
      auto classDef = parseClassDefinition();
      if (classDef) classDef->setIsPacked(isPacked);
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

    case TokenKind::CONST:
      return parseConstStatement();

    case TokenKind::REF:
      return parseRefStatement(captureStart(), /*isMutable=*/true);

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
      Position start = captureStart();
      auto proto = parseExtern();
      if (curTok.kind == TokenKind::SEMI_COLON)
        getNextToken();  // eat optional semicolon
      // Wrap prototype in a FunctionAST with no body (nullptr)
      auto fn = std::make_unique<FunctionAST>(std::move(proto), nullptr);
      fn->setCExtern(true);  // C ABI — distinguishes from `declare function`
      return finishNode(std::move(fn), start);
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
      Position start = captureStart();
      auto thisExpr = std::make_unique<ThisExprAST>();
      getNextToken();  // eat 'this'
      thisExpr = finishNode(std::move(thisExpr), start);

      // Must be followed by '.'
      if (curTok.kind != TokenKind::DOT) {
        parsingError("expected '.' after 'this'");
        return nullptr;
      }

      // Parse postfix expressions (member accesses, method calls)
      auto lhs = parsePostfixExpr(std::move(thisExpr));
      if (!lhs) return nullptr;

      // Check for assignment: this.field = value or this.field op= value
      if (isAssignmentOp(curTok.kind) &&
          lhs->getType() == ASTNodeType::MEMBER_ACCESS) {
        return finishMemberAssignment(std::move(lhs));
      }

      // Not an assignment - expression statement (like method call)
      if (curTok.kind == TokenKind::SEMI_COLON)
        getNextToken();
      else
        parsingError("expected ';' after expression statement");
      return lhs;
    }
    case TokenKind::RETURN: {
      // parse return <expr>; or return;
      Position start = captureStart();
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

        return finishNode(std::make_unique<ReturnExprAST>(std::move(expr)),
                          start);
      }
    }
    case TokenKind::TRY: {
      // try { ... } catch (e: IError) { ... } syntax
      Position start = captureStart();
      getNextToken();  // eat 'try'
      std::unique_ptr<ExprAST> tryExpr;
      if (curTok.kind != TokenKind::BRACE_OPEN) {
        parsingError("expected '{' after 'try'");
        return nullptr;
      }
      tryExpr = finishNode(parseTryCatch(), start);
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
  Position forStart = captureStart();
  getNextToken();  // eat 'for'

  expectCurrentTokenKind(TokenKind::PAREN_OPEN, "Expected '(' after 'for'");
  getNextToken();  // eat '('

  // Check for for-in loop: for (var x: T in iterable) { ... }
  // We need to distinguish from for (var i: i32 = 0; ...; ...) { ... }
  if (curTok.kind == TokenKind::VAR || curTok.kind == TokenKind::CONST) {
    // Save state for potential backtracking
    Token savedCurTok = curTok;
    Token savedPrevTok = prevTok_;
    auto savedLexerPos = lexer.getPosition();
    auto savedTokenStack = tokenStack;
    bool isConst = curTok.kind == TokenKind::CONST;

    getNextToken();  // eat 'var' / 'const'

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

          expectCurrentTokenKind(TokenKind::PAREN_CLOSE,
                                 "Expected ')' after for-in iterable");
          getNextToken();  // eat ')'

          // Require curly braces for for-in body
          expectCurrentTokenKind(TokenKind::BRACE_OPEN,
                                 "Expected '{' for for-in body");

          auto bodyBlock = parseBlock();
          if (!bodyBlock) return nullptr;

          // Body kept as a block; LoweringPass normalizes it
          unique_ptr<ExprAST> body = std::move(bodyBlock);

          return finishNode(
              std::make_unique<ForInExprAST>(
                  std::move(varName), std::move(typeAnnot),
                  std::move(iterable), std::move(body), isConst),
              forStart);
        }
      }
    }

    // Not a for-in loop, backtrack and continue with traditional for loop
    curTok = savedCurTok;
    prevTok_ = savedPrevTok;
    lexer.setPosition(savedLexerPos);
    tokenStack = savedTokenStack;
  }

  // Traditional for loop: for (init; condition; increment) { body }
  // Parse initialization (can be empty, var declaration, or assignment)
  unique_ptr<ExprAST> init;
  if (curTok.kind != TokenKind::SEMI_COLON) {
    // Check if it's a variable declaration
    if (curTok.kind == TokenKind::VAR || curTok.kind == TokenKind::CONST) {
      init = parseVarDeclaration();  // Without semicolon consumption
    } else {
      init = parseExpression();
    }
    if (!init) return nullptr;
  }

  expectCurrentTokenKind(TokenKind::SEMI_COLON,
                         "Expected ';' after for loop initialization");
  getNextToken();  // eat ';'

  // Parse condition (can be empty for infinite loop)
  unique_ptr<ExprAST> condition;
  if (curTok.kind != TokenKind::SEMI_COLON) {
    condition = parseExpression();
    if (!condition) return nullptr;
  }

  expectCurrentTokenKind(TokenKind::SEMI_COLON,
                         "Expected ';' after for loop condition");
  getNextToken();  // eat ';'

  // Parse increment (can be empty) - can be assignment or expression
  unique_ptr<ExprAST> increment;
  if (curTok.kind != TokenKind::PAREN_CLOSE) {
    // Check for assignment (identifier = expr)
    if (curTok.kind == TokenKind::IDENTIFIER) {
      auto savedPos = lexer.getPosition();
      auto savedTok = curTok;
      Token savedPrevTok = prevTok_;
      std::string idName = std::get<std::string>(curTok.value);
      getNextToken();

      if (isAssignmentOp(curTok.kind)) {
        // It's an assignment: i = i + 1 or i op= expr (no trailing ';')
        increment = finishVariableAssignment(idName, savedTok.start);
        if (!increment) return nullptr;
      } else {
        // Not an assignment, backtrack and parse as expression
        lexer.setPosition(savedPos);
        curTok = savedTok;
        prevTok_ = savedPrevTok;
        increment = parseExpression();
      }
    } else {
      increment = parseExpression();
    }
    if (!increment) return nullptr;
  }

  expectCurrentTokenKind(TokenKind::PAREN_CLOSE,
                         "Expected ')' after for loop increment");
  getNextToken();  // eat ')'

  // Require curly braces for for-loop body
  expectCurrentTokenKind(TokenKind::BRACE_OPEN,
                         "Expected '{' for for-loop body");

  auto bodyBlock = parseBlock();
  if (!bodyBlock) return nullptr;

  // Body kept as a block; LoweringPass normalizes it
  unique_ptr<ExprAST> body = std::move(bodyBlock);

  return finishNode(
      std::make_unique<ForExprAST>(std::move(init), std::move(condition),
                                   std::move(increment), std::move(body)),
      forStart);
}

unique_ptr<WhileExprAST> Parser::parseWhileLoop() {
  Position start = captureStart();
  getNextToken();  // eat 'while'

  expectCurrentTokenKind(TokenKind::PAREN_OPEN, "Expected '(' after 'while'");
  getNextToken();  // eat '('

  auto condition = parseExpression();
  if (!condition) return nullptr;

  expectCurrentTokenKind(TokenKind::PAREN_CLOSE,
                         "Expected ')' after while condition");
  getNextToken();  // eat ')'

  // Require curly braces for while-loop body
  expectCurrentTokenKind(TokenKind::BRACE_OPEN,
                         "Expected '{' for while-loop body");

  auto bodyBlock = parseBlock();
  if (!bodyBlock) return nullptr;

  // Body kept as a block; LoweringPass normalizes it
  unique_ptr<ExprAST> body = std::move(bodyBlock);

  return finishNode(
      std::make_unique<WhileExprAST>(std::move(condition), std::move(body)),
      start);
}

unique_ptr<BreakAST> Parser::parseBreak() {
  Position start = captureStart();
  getNextToken();  // eat 'break'

  // Expect semicolon after break
  if (curTok.kind == TokenKind::SEMI_COLON) {
    getNextToken();  // eat ';'
  }

  return finishNode(std::make_unique<BreakAST>(), start);
}

unique_ptr<ContinueAST> Parser::parseContinue() {
  Position start = captureStart();
  getNextToken();  // eat 'continue'

  // Expect semicolon after continue
  if (curTok.kind == TokenKind::SEMI_COLON) {
    getNextToken();  // eat ';'
  }

  return finishNode(std::make_unique<ContinueAST>(), start);
}

std::unique_ptr<PrototypeAST> Parser::parseExtern() {
  getNextToken();  // eat 'extern'

  // Optional ABI string: extern "C" function ...
  // Only the C ABI exists today; naming it is allowed so the intent is
  // explicit and so other ABIs can be added without a syntax change.
  if (curTok.kind == TokenKind::STRING) {
    std::string abi = curTok.getString().value();
    if (abi != "C") {
      parsingError("unsupported extern ABI '" + abi + "'; expected \"C\"");
    }
    getNextToken();  // eat ABI string
  }

  // Expect 'function' keyword
  if (curTok.kind != TokenKind::FUNCTION) {
    parsingError("expected 'function' after 'extern'");
    return nullptr;
  }
  getNextToken();  // eat 'function'

  auto proto = parsePrototype();

  // Optional symbol rename: ... as "c_symbol".
  // `as` is matched contextually rather than reserved as a keyword, so
  // existing code may still use it as an identifier.
  if (proto && curTok.kind == TokenKind::IDENTIFIER &&
      curTok.getIdentifier().value() == "as") {
    getNextToken();  // eat 'as'
    if (curTok.kind != TokenKind::STRING) {
      parsingError("expected a string literal C symbol name after 'as'");
      return proto;
    }
    std::string symbol = curTok.getString().value();
    if (symbol.empty()) {
      parsingError("C symbol name after 'as' cannot be empty");
    }
    proto->setLinkName(std::move(symbol));
    getNextToken();  // eat symbol string
  }

  return proto;
}

// Parse manifest block:
// manifest {
//   suns = [ "file.sun", { path = "other.sun", hash = "abc" } ]
//   moons = ( "lib.moon", { path = "x.moon", hash = "def", rename = "y" } )
// }
unique_ptr<ManifestAST> Parser::parseManifest() {
  Position start = captureStart();
  getNextToken();  // eat 'manifest'

  expectCurrentTokenKind(TokenKind::BRACE_OPEN,
                         "expected '{' for manifest block");
  getNextToken();  // eat '{'

  std::vector<ManifestSunDependency> suns;
  std::vector<ManifestMoonDependency> moons;
  std::vector<ManifestProtoDependency> protos;

  while (curTok.kind == TokenKind::IDENTIFIER) {
    auto ident = curTok.getIdentifier().value();
    getNextToken();  // eat identifier

    expectCurrentTokenKind(TokenKind::COLON,
                           "expected ':' after '" + ident + "' in manifest");
    getNextToken();  // eat ':'

    if (ident == "suns") {
      suns = parseManifestSuns();
    } else if (ident == "moons") {
      moons = parseManifestMoons();
    } else if (ident == "protos") {
      protos = parseManifestProtos();
    } else {
      parsingError("unexpected identifier '" + ident +
                   "' in manifest; expected 'suns', 'moons' or 'protos'");
    }

    // Entries are newline-separated; a trailing ';' is tolerated
    if (curTok.kind == TokenKind::SEMI_COLON) {
      getNextToken();
    }
  }

  expectCurrentTokenKind(TokenKind::BRACE_CLOSE,
                         "expected '}' at end of manifest block");
  getNextToken();  // eat '}'

  return finishNode(std::make_unique<ManifestAST>(std::move(suns),
                                                  std::move(moons),
                                                  std::move(protos)),
                    start);
}

// Parse protos array: [ "schemas/telemetry.proto", ... ]
std::vector<ManifestProtoDependency> Parser::parseManifestProtos() {
  expectCurrentTokenKind(TokenKind::BRACKET_OPEN,
                         "expected '[' after 'protos:'");
  getNextToken();  // eat '['

  std::vector<ManifestProtoDependency> protos;

  while (curTok.kind != TokenKind::BRACKET_CLOSE) {
    expectCurrentTokenKind(TokenKind::STRING,
                           "expected string path in protos array");
    ManifestProtoDependency dep;
    dep.path = curTok.getString().value();
    getNextToken();  // eat string
    protos.push_back(std::move(dep));

    if (curTok.kind == TokenKind::COMMA) {
      getNextToken();  // eat ','
    } else if (curTok.kind != TokenKind::BRACKET_CLOSE) {
      parsingError("expected ',' or ']' in protos array");
    }
  }

  getNextToken();  // eat ']'
  return protos;
}

// Parse suns array: [ "file.sun", { path: "other.sun", hash: "abc" } ]
std::vector<ManifestSunDependency> Parser::parseManifestSuns() {
  expectCurrentTokenKind(TokenKind::BRACKET_OPEN, "expected '[' after 'suns:'");
  getNextToken();  // eat '['

  std::vector<ManifestSunDependency> suns;

  while (curTok.kind != TokenKind::BRACKET_CLOSE) {
    ManifestSunDependency dep;

    if (curTok.kind == TokenKind::STRING) {
      // Simple string form: "path/to/file.sun"
      dep.path = curTok.getString().value();
      getNextToken();  // eat string
    } else if (curTok.kind == TokenKind::BRACE_OPEN) {
      // Struct form: { path: "...", hash: "..." }
      getNextToken();  // eat '{'

      while (curTok.kind == TokenKind::IDENTIFIER) {
        auto fieldName = curTok.getIdentifier().value();
        getNextToken();  // eat field name

        expectCurrentTokenKind(
            TokenKind::COLON,
            "expected ':' after field name in sun dependency");
        getNextToken();  // eat ':'

        expectCurrentTokenKind(
            TokenKind::STRING,
            "expected string value for field '" + fieldName + "'");
        auto value = curTok.getString().value();
        getNextToken();  // eat string

        if (fieldName == "path") {
          dep.path = value;
        } else if (fieldName == "hash") {
          dep.hash = value;
        } else {
          parsingError("unknown field '" + fieldName + "' in sun dependency");
        }

        if (curTok.kind == TokenKind::COMMA) {
          getNextToken();  // eat ','
        }
      }

      expectCurrentTokenKind(TokenKind::BRACE_CLOSE,
                             "expected '}' at end of sun dependency");
      getNextToken();  // eat '}'

      if (dep.path.empty()) {
        parsingError("sun dependency missing required 'path' field");
      }
    } else {
      parsingError("expected string or '{' in suns array");
    }

    suns.push_back(std::move(dep));

    if (curTok.kind == TokenKind::COMMA) {
      getNextToken();  // eat ','
    } else if (curTok.kind != TokenKind::BRACKET_CLOSE) {
      parsingError("expected ',' or ']' in suns array");
    }
  }

  getNextToken();  // eat ']'
  return suns;
}

// Parse moons array: [ "lib.moon", { path: "x.moon", hash: "def", rename:
// "y" }, { url: "https://example.com/lib.moon" } ]
// A struct entry needs exactly one of 'path' or 'url'.
std::vector<ManifestMoonDependency> Parser::parseManifestMoons() {
  expectCurrentTokenKind(TokenKind::BRACKET_OPEN,
                         "expected '[' after 'moons:'");
  getNextToken();  // eat '['

  std::vector<ManifestMoonDependency> moons;

  while (curTok.kind != TokenKind::BRACKET_CLOSE) {
    ManifestMoonDependency dep;

    if (curTok.kind == TokenKind::STRING) {
      // Simple string form: "path/to/lib.moon"
      dep.path = curTok.getString().value();
      getNextToken();  // eat string
    } else if (curTok.kind == TokenKind::BRACE_OPEN) {
      // Struct form: { path: "...", hash: "...", rename: "..." }
      getNextToken();  // eat '{'

      while (curTok.kind == TokenKind::IDENTIFIER) {
        auto fieldName = curTok.getIdentifier().value();
        getNextToken();  // eat field name

        expectCurrentTokenKind(
            TokenKind::COLON,
            "expected ':' after field name in moon dependency");
        getNextToken();  // eat ':'

        expectCurrentTokenKind(
            TokenKind::STRING,
            "expected string value for field '" + fieldName + "'");
        auto value = curTok.getString().value();
        getNextToken();  // eat string

        if (fieldName == "path") {
          dep.path = value;
        } else if (fieldName == "url") {
          dep.url = value;
        } else if (fieldName == "hash") {
          dep.hash = value;
        } else if (fieldName == "rename") {
          dep.rename = value;
        } else {
          parsingError("unknown field '" + fieldName + "' in moon dependency");
        }

        if (curTok.kind == TokenKind::COMMA) {
          getNextToken();  // eat ','
        }
      }

      expectCurrentTokenKind(TokenKind::BRACE_CLOSE,
                             "expected '}' at end of moon dependency");
      getNextToken();  // eat '}'

      if (!dep.path.empty() && dep.url.has_value()) {
        parsingError("moon dependency cannot have both 'path' and 'url'");
      }
      if (dep.path.empty() && !dep.url.has_value()) {
        parsingError("moon dependency requires a 'path' or 'url' field");
      }
    } else {
      parsingError("expected string or '{' in moons array");
    }

    moons.push_back(std::move(dep));

    if (curTok.kind == TokenKind::COMMA) {
      getNextToken();  // eat ','
    } else if (curTok.kind != TokenKind::BRACKET_CLOSE) {
      parsingError("expected ',' or ']' in moons array");
    }
  }

  getNextToken();  // eat ']'
  return moons;
}

// Parse declare statement:
// - Forward function declaration: declare function name(args) RetType;
// - Type declaration: declare [Alias =] Type<Args>;
unique_ptr<ExprAST> Parser::parseDeclareStatement() {
  Position declStart = captureStart();
  getNextToken();  // eat 'declare'

  // Check for forward function declaration: declare function name(args)
  // RetType;
  if (curTok.kind == TokenKind::FUNCTION) {
    getNextToken();  // eat 'function'
    auto proto = parsePrototype();
    if (curTok.kind != TokenKind::SEMI_COLON) {
      parsingError("expected ';' after forward function declaration");
      return nullptr;
    }
    getNextToken();  // eat ';'
    // Return FunctionAST with no body (forward declaration)
    return finishNode(
        std::make_unique<FunctionAST>(std::move(proto), nullptr), declStart);
  }

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
        consumeGreater("expected '>' after type arguments");
      }

      expectCurrentTokenKind(TokenKind::SEMI_COLON,
                             "expected ';' after declare statement");
      getNextToken();  // eat ';'

      return finishNode(
          std::make_unique<DeclareTypeAST>(std::move(typeAnnot), alias),
          declStart);
    }
  } else {
    parsingError("expected 'function' or type name after 'declare'");
    return nullptr;
  }

  // Parse the type annotation
  auto typeAnnot = parseTypeAnnotation();

  expectCurrentTokenKind(TokenKind::SEMI_COLON,
                         "expected ';' after declare statement");
  getNextToken();  // eat ';'

  return finishNode(
      std::make_unique<DeclareTypeAST>(std::move(typeAnnot), alias),
      declStart);
}

// Parse module declaration: module Name { declarations... }
// Supports dotted names as shorthand for nested modules:
//   module sun.io { } expands to module sun { module io { } }
// Supports both 'module' (preferred) and 'namespace' (legacy) keywords
unique_ptr<ModuleAST> Parser::parseModuleDecl() {
  Position start = captureStart();
  getNextToken();  // eat 'module' or 'namespace'

  expectCurrentTokenKind(TokenKind::IDENTIFIER, "expected module name");

  // Collect dotted module path (e.g., "sun.io" becomes ["sun", "io"])
  std::vector<std::string> names;
  names.push_back(curTok.getIdentifier().value());
  getNextToken();  // eat first module name

  // Parse additional dotted segments: .identifier
  while (curTok.kind == TokenKind::DOT) {
    getNextToken();  // eat '.'
    expectCurrentTokenKind(TokenKind::IDENTIFIER,
                           "expected identifier after '.' in module name");
    names.push_back(curTok.getIdentifier().value());
    getNextToken();  // eat identifier
  }

  expectCurrentTokenKind(TokenKind::BRACE_OPEN,
                         "expected '{' after module name");

  auto body = parseBlock(/*itemLevel=*/true);
  if (!body) return nullptr;

  // Build nested modules from innermost to outermost
  // For "module a.b.c { body }", create:
  //   ModuleAST("a") { ModuleAST("b") { ModuleAST("c") { body } } }
  auto innermost =
      std::make_unique<ModuleAST>(std::move(names.back()), std::move(body));
  names.pop_back();

  std::unique_ptr<ModuleAST> result =
      finishNode(std::move(innermost), start);
  for (auto it = names.rbegin(); it != names.rend(); ++it) {
    // Wrap current result in a new block containing just this module
    std::vector<std::unique_ptr<ExprAST>> stmts;
    stmts.push_back(std::move(result));
    auto wrapperBody = std::make_unique<BlockExprAST>(std::move(stmts));
    result = finishNode(
        std::make_unique<ModuleAST>(std::move(*it), std::move(wrapperBody)),
        start);
  }

  return result;
}

// Parse using statement with dot-based syntax:
//   using sun;           -> import all from sun
//   using sun.Vec;       -> import specific symbol Vec from sun
unique_ptr<UsingAST> Parser::parseUsingStatement() {
  Position start = captureStart();
  getNextToken();  // eat 'using'

  std::vector<std::string> namespacePath;
  std::string target;

  expectCurrentTokenKind(TokenKind::IDENTIFIER,
                         "expected identifier after 'using'");

  // Parse first identifier (module name)
  std::string firstName = curTok.getIdentifier().value();
  getNextToken();  // eat identifier

  // Check what follows: ';' or '.'
  if (curTok.kind == TokenKind::SEMI_COLON) {
    // Simple form: "using sun;" means import all from sun
    namespacePath.push_back(std::move(firstName));
    target = "*";
    getNextToken();  // eat ';'
    return finishNode(std::make_unique<UsingAST>(std::move(namespacePath),
                                                 std::move(target)),
                      start);
  }

  // Dot-based path: using sun.Vec; or using sun.nested.Vec;
  if (curTok.kind == TokenKind::DOT) {
    namespacePath.push_back(std::move(firstName));

    while (curTok.kind == TokenKind::DOT) {
      getNextToken();  // eat '.'

      if (curTok.kind == TokenKind::IDENTIFIER) {
        std::string part = curTok.getIdentifier().value();
        getNextToken();  // eat identifier

        if (curTok.kind == TokenKind::DOT) {
          // More path components: sun.nested.deeper
          namespacePath.push_back(std::move(part));
        } else {
          // Final target: "using sun.Vec;"
          target = std::move(part);
          break;
        }
      } else {
        parsingError("expected identifier after '.' in using statement");
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

  return finishNode(std::make_unique<UsingAST>(std::move(namespacePath),
                                               std::move(target)),
                    start);
}

// Parse a qualified name (Module.name) or simple identifier
unique_ptr<ExprAST> Parser::parseQualifiedOrSimpleName() {
  if (curTok.kind != TokenKind::IDENTIFIER) {
    return nullptr;
  }

  Position start = captureStart();
  std::string firstName = curTok.getIdentifier().value();
  getNextToken();  // eat first identifier

  // Check if it's a qualified name (dot-based)
  if (curTok.kind == TokenKind::DOT) {
    std::vector<std::string> parts;
    parts.push_back(std::move(firstName));

    while (curTok.kind == TokenKind::DOT) {
      getNextToken();  // eat '.'

      expectCurrentTokenKind(TokenKind::IDENTIFIER,
                             "expected identifier after '.'");

      parts.push_back(curTok.getIdentifier().value());
      getNextToken();  // eat identifier
    }

    return finishNode(std::make_unique<QualifiedNameAST>(std::move(parts)),
                      start);
  }

  // Simple identifier - return as variable reference
  return finishNode(
      std::make_unique<VariableReferenceAST>(std::move(firstName)), start);
}

// Handle import of a precompiled .moon file
// Returns a MoonScopeAST wrapping all module stubs, or nullptr if already
// imported
std::unique_ptr<MoonScopeAST> Parser::collectMoonImport(
    const sun::MoonImport& moonImport) {
  const std::string& moonPath = moonImport.path;
  std::string contentHash;

  auto resolveOne = [&](const std::string& path) -> std::filesystem::path {
    if (std::filesystem::path(path).is_absolute()) {
      return std::filesystem::exists(path) ? std::filesystem::path(path)
                                           : std::filesystem::path();
    }
    // Check SUN_PATH directories
    std::filesystem::path resolved = sun::SunPath::resolve(path);
    // Check system-wide installation paths
    if (resolved.empty()) {
      auto sysPath = std::filesystem::path("/usr/lib/sun") / path;
      if (std::filesystem::exists(sysPath)) {
        resolved = sysPath;
      }
    }
    // Fall back to resolving relative to current file's directory
    if (resolved.empty()) {
      auto local = std::filesystem::path(baseDir) / path;
      if (std::filesystem::exists(local)) {
        resolved = local;
      }
    }
    return resolved;
  };

  // Bundles are resolved by the exact name given — the metadata's target
  // triple is validation, not a resolution input. Cross builds point at a
  // per-target bundle explicitly (e.g. build/aarch64-linux-gnu/stdlib.moon,
  // via the path itself, --lib-path or SUN_PATH ordering); a wrong-target
  // bundle is rejected at link time with an actionable error.
  std::filesystem::path resolved = resolveOne(moonPath);

  if (resolved.empty() || !std::filesystem::exists(resolved)) {
    logAndThrowError("Could not find moon file: " + moonPath);
    return nullptr;
  }
  resolved = std::filesystem::canonical(resolved);
  std::string resolvedStr = resolved.string();

  // Open the moon bundle
  PARSER_TIMER_START(open_moon);
  auto reader = sun::MoonReader::open(resolved);
  if (!reader) {
    logAndThrowError("Failed to open moon: " + resolvedStr);
    return nullptr;
  }
  PARSER_TIMER_END(open_moon);

  // Process each module in the bundle, grouping by module name
  // to consolidate stubs and detect name collisions
  PARSER_TIMER_START(process_modules);

  // Map module_name -> list of stubs (empty key = global scope).
  // Ordered, so a module is always emitted before the modules nested inside
  // it ("sun" sorts before "sun.io"). The declaration pre-pass registers a
  // module's types as it reaches it, so a nested module's class shape can
  // only name a generic from its parent if the parent came first.
  std::map<std::string, std::vector<std::unique_ptr<ExprAST>>> moduleStubs;
  // Track defined symbols per module for collision detection
  std::unordered_map<std::string, std::unordered_set<std::string>>
      moduleSymbols;
  // Visibility per (effective) dotted module name, from the bundle metadata
  std::unordered_map<std::string, sun::Visibility> moduleVisibility;

  // Track the primary module name (first non-empty module found)
  std::string primaryModuleName;

  for (const auto& moduleKey : reader->listModules()) {
    // Record for linking
    bool alreadyRecorded = false;
    for (const auto& key : *precompiledImports) {
      if (key == moduleKey) {
        alreadyRecorded = true;
        break;
      }
    }
    if (!alreadyRecorded) {
      precompiledImports->push_back(moduleKey);
    }

    // Get metadata for this module
    const auto* metadata = reader->getMetadata(moduleKey);
    if (!metadata) {
      continue;
    }

    // Capture content hash from first module (all share the same hash)
    if (contentHash.empty()) {
      contentHash = sun::getSymbolPrefix(*metadata);
    }

    // Track primary module name
    if (primaryModuleName.empty() && !metadata->module_name().empty()) {
      primaryModuleName = metadata->module_name();
    }

    // Create stubs into a temporary vector. The module's `using`
    // declarations come first so the stubs' field/parameter types resolve
    // the same names their source did (e.g. Vec<u8> from stdlib.moon).
    std::vector<std::unique_ptr<ExprAST>> stubs;
    for (const auto& u : metadata->usings()) {
      std::vector<std::string> path;
      std::stringstream ss(u);
      std::string seg;
      while (std::getline(ss, seg, '.')) path.push_back(seg);
      auto usingStub = std::make_unique<UsingAST>(std::move(path), "*");
      usingStub->setPrecompiled(true);
      stubs.push_back(std::move(usingStub));
    }
    createModuleStubs(*metadata, stubs);

    // Get the original module name and apply remapping if configured
    std::string modName = metadata->module_name();
    std::string effectiveName = moonImport.getAliasedModule(modName);
    if (!effectiveName.empty()) {
      auto& vis = moduleVisibility[effectiveName];
      if (metadata->visibility() == sun::ast::PUBLIC)
        vis = sun::Visibility::Public;
      // Ensure the entry exists even for a module with no stubs
      (void)moduleStubs[effectiveName];
    }

    // Check for name collisions within this moon
    auto& symbols = moduleSymbols[effectiveName];
    for (auto& stub : stubs) {
      std::string symbolName;
      if (auto* cls = dynamic_cast<ClassDefinitionAST*>(stub.get())) {
        symbolName = cls->getName();
      } else if (auto* iface =
                     dynamic_cast<InterfaceDefinitionAST*>(stub.get())) {
        symbolName = iface->getName();
      } else if (auto* func = dynamic_cast<FunctionAST*>(stub.get())) {
        // Use function name + param type annotations to allow overloads
        symbolName = func->getProto().getName();
        if (!func->getProto().getArgs().empty()) {
          symbolName += "(";
          for (size_t i = 0; i < func->getProto().getArgs().size(); ++i) {
            if (i > 0) symbolName += ",";
            symbolName += func->getProto().getArgs()[i].second.toString();
          }
          symbolName += ")";
        }
      }
      if (!symbolName.empty()) {
        if (!symbols.insert(symbolName).second) {
          logAndThrowError("Name collision in moon module '" + effectiveName +
                           "': duplicate symbol '" + symbolName + "'");
        }
      }
      moduleStubs[effectiveName].push_back(std::move(stub));
    }
  }

  // Visibility of a dotted prefix: its own entry if the bundle recorded one,
  // else public when any module below it is public (the prefix was only an
  // unrecorded shell around a public module)
  auto visibilityOf = [&](const std::string& dotted) {
    auto it = moduleVisibility.find(dotted);
    if (it != moduleVisibility.end()) return it->second;
    for (const auto& [name, vis] : moduleVisibility) {
      if (vis == sun::Visibility::Public && name.size() > dotted.size() &&
          name.compare(0, dotted.size(), dotted) == 0 &&
          name[dotted.size()] == '.')
        return sun::Visibility::Public;
    }
    return sun::Visibility::Private;
  };

  // Build consolidated ModuleAST nodes (one per unique module name)
  std::vector<std::unique_ptr<ExprAST>> allModuleASTs;
  for (auto& [modName, stubs] : moduleStubs) {
    if (stubs.empty()) continue;
    if (!modName.empty()) {
      // A dotted name ("a.b") becomes nested modules, innermost first
      std::vector<std::string> segs;
      {
        std::stringstream ss(modName);
        std::string seg;
        while (std::getline(ss, seg, '.')) segs.push_back(seg);
      }
      auto nsBody = std::make_unique<BlockExprAST>(std::move(stubs));
      std::unique_ptr<ExprAST> current;
      for (size_t i = segs.size(); i-- > 0;) {
        std::unique_ptr<BlockExprAST> body;
        if (current) {
          std::vector<std::unique_ptr<ExprAST>> one;
          one.push_back(std::move(current));
          body = std::make_unique<BlockExprAST>(std::move(one));
        } else {
          body = std::move(nsBody);
        }
        auto nsAST = std::make_unique<ModuleAST>(segs[i], std::move(body));
        nsAST->setPrecompiled(true);
        std::string prefix;
        for (size_t k = 0; k <= i; ++k) prefix += (k ? "." : "") + segs[k];
        nsAST->setVisibility(visibilityOf(prefix));
        current = std::move(nsAST);
      }
      allModuleASTs.push_back(std::move(current));
    } else {
      for (auto& ast : stubs) {
        allModuleASTs.push_back(std::move(ast));
      }
    }
  }
  PARSER_TIMER_END(process_modules);

  // Store the moon reader in the cache for later linking
  sun::LibraryCache::instance().addBundle(resolved);

  // Determine the alias if provided (from moduleRemap)
  std::optional<std::string> alias;
  if (moonImport.hasRemap() && !primaryModuleName.empty()) {
    std::string remapped = moonImport.getAliasedModule(primaryModuleName);
    if (remapped != primaryModuleName) {
      alias = remapped;
    }
  }

  // Wrap everything in a MoonScopeAST
  auto body = std::make_unique<BlockExprAST>(std::move(allModuleASTs));
  return std::make_unique<MoonScopeAST>(contentHash, primaryModuleName, alias,
                                        resolvedStr, std::move(body));
}

// Create AST stubs from protobuf module metadata
// Uses ASTDeserializer to convert proto nodes to AST
void Parser::createModuleStubs(
    const sun::moon::ModuleMetadata& metadata,
    std::vector<std::unique_ptr<ExprAST>>& collectedAST) {
  // Use ASTDeserializer to convert proto nodes. Positions inside the bundle
  // carry no file of their own; they all belong to the module's source file
  sun::serialization::ASTDeserializer deserializer(
      {.default_file_path = metadata.source_path()});

  // Build the scope path for qualified names:
  // Content hash ensures symbol isolation between library versions
  std::string contentHash = sun::getSymbolPrefix(metadata);
  std::vector<std::string> scopePath;
  if (!contentHash.empty()) {
    scopePath.push_back(contentHash);
  }
  if (!metadata.module_name().empty()) {
    // Nested modules are exported under their dotted path
    std::stringstream ss(metadata.module_name());
    std::string seg;
    while (std::getline(ss, seg, '.')) scopePath.push_back(seg);
  }

  // Collect AST stubs for this module - may be wrapped in a namespace
  std::vector<std::unique_ptr<ExprAST>> moduleAST;

  // Create AST stubs from metadata
  // IMPORTANT: Process interfaces FIRST (before classes that implement them)
  for (int i = 0; i < metadata.interfaces_size(); ++i) {
    sun::ast::ASTNode node;
    *node.mutable_interface_def() = metadata.interfaces(i);
    if (node.interface_def().has_location()) {
      *node.mutable_location() = node.interface_def().location();
    }

    auto ast = deserializer.deserialize(node);
    if (ast) {
      if (auto* ifaceDef = dynamic_cast<InterfaceDefinitionAST*>(ast.get())) {
        ifaceDef->setPrecompiled(true);
        ifaceDef->setQualifiedName(
            sun::QualifiedName(scopePath, ifaceDef->getName(), scopePath));
      }
      moduleAST.push_back(std::move(ast));
    }
  }

  // Classes (after interfaces so interface lookups work)
  for (int i = 0; i < metadata.classes_size(); ++i) {
    sun::ast::ASTNode node;
    *node.mutable_class_def() = metadata.classes(i);
    if (node.class_def().has_location()) {
      *node.mutable_location() = node.class_def().location();
    }

    auto ast = deserializer.deserialize(node);
    if (ast) {
      if (auto* classDef = dynamic_cast<ClassDefinitionAST*>(ast.get())) {
        classDef->setPrecompiled(true);
        classDef->setQualifiedName(
            sun::QualifiedName(scopePath, classDef->getName(), scopePath));
      }
      moduleAST.push_back(std::move(ast));
    }
  }

  // Module-level variables. The stub carries the type but no initializer —
  // the storage lives in the bundle's bitcode and is linked in.
  for (int i = 0; i < metadata.globals_size(); ++i) {
    sun::ast::ASTNode node;
    *node.mutable_variable_creation() = metadata.globals(i);

    auto ast = deserializer.deserialize(node);
    if (ast) {
      if (auto* varDef = dynamic_cast<VariableCreationAST*>(ast.get())) {
        varDef->setPrecompiled(true);
        varDef->setQualifiedName(
            sun::QualifiedName(scopePath, varDef->getName(), scopePath));
      }
      moduleAST.push_back(std::move(ast));
    }
  }

  // Enums (before functions, whose signatures may use enum types; after
  // classes, which enum payload types may reference)
  for (int i = 0; i < metadata.enums_size(); ++i) {
    sun::ast::ASTNode node;
    *node.mutable_enum_def() = metadata.enums(i);
    if (node.enum_def().has_location()) {
      *node.mutable_location() = node.enum_def().location();
    }

    auto ast = deserializer.deserialize(node);
    if (ast) {
      moduleAST.push_back(std::move(ast));
    }
  }

  // Functions
  for (int i = 0; i < metadata.functions_size(); ++i) {
    sun::ast::ASTNode node;
    *node.mutable_function_def() = metadata.functions(i);
    if (node.function_def().has_location()) {
      *node.mutable_location() = node.function_def().location();
    }

    auto ast = deserializer.deserialize(node);
    if (ast) {
      if (auto* funcAST = dynamic_cast<FunctionAST*>(ast.get())) {
        funcAST->setPrecompiled(true);
        funcAST->getProtoMut().setQualifiedName(
            sun::QualifiedName(scopePath, funcAST->getProto().getName(),
                               scopePath));
      }
      moduleAST.push_back(std::move(ast));
    }
  }

  // Add all stubs directly (consolidation and wrapping is done by the caller)
  for (auto& ast : moduleAST) {
    collectedAST.push_back(std::move(ast));
  }
}

// Helper to parse a type string back into TypeAnnotation.
TypeAnnotation Parser::parseTypeFromString(const std::string& typeStr) {
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
      result.elementType =
          std::make_unique<TypeAnnotation>(parseTypeFromString(inner));
      result.canError = canError;
      return result;
    }
  }
  if (cleanType.size() > 4 && cleanType.substr(0, 4) == "ptr(") {
    size_t end = cleanType.rfind(')');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(4, end - 4);
      TypeAnnotation result("ptr");
      result.elementType =
          std::make_unique<TypeAnnotation>(parseTypeFromString(inner));
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
      result.elementType =
          std::make_unique<TypeAnnotation>(parseTypeFromString(inner));
      result.canError = canError;
      return result;
    }
  }
  if (cleanType.size() > 8 && cleanType.substr(0, 8) == "raw_ptr(") {
    size_t end = cleanType.rfind(')');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(8, end - 8);
      TypeAnnotation result("raw_ptr");
      result.elementType =
          std::make_unique<TypeAnnotation>(parseTypeFromString(inner));
      result.canError = canError;
      return result;
    }
  }

  // Handle ref types: ref T or ref(T)
  if (cleanType.size() > 4 && cleanType.substr(0, 4) == "ref ") {
    std::string inner = cleanType.substr(4);
    TypeAnnotation result("ref");
    result.elementType =
        std::make_unique<TypeAnnotation>(parseTypeFromString(inner));
    result.canError = canError;
    return result;
  }
  if (cleanType.size() > 4 && cleanType.substr(0, 4) == "ref(") {
    size_t end = cleanType.rfind(')');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(4, end - 4);
      TypeAnnotation result("ref");
      result.elementType =
          std::make_unique<TypeAnnotation>(parseTypeFromString(inner));
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
        result.elementType =
            std::make_unique<TypeAnnotation>(parseTypeFromString(inner));
      } else {
        // Sized array: array<T, dim1, dim2, ...>
        std::string elemType = inner.substr(0, firstComma);
        result.elementType =
            std::make_unique<TypeAnnotation>(parseTypeFromString(elemType));

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
      result.elementType =
          std::make_unique<TypeAnnotation>(parseTypeFromString(inner));
      result.canError = canError;
      return result;
    }
  }
  if (cleanType.size() > 11 && cleanType.substr(0, 11) == "static_ptr(") {
    size_t end = cleanType.rfind(')');
    if (end != std::string::npos) {
      std::string inner = cleanType.substr(11, end - 11);
      TypeAnnotation result("static_ptr");
      result.elementType =
          std::make_unique<TypeAnnotation>(parseTypeFromString(inner));
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
                  parseTypeFromString(argStr)));
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
  std::string typeName = cleanType;
  TypeAnnotation result(typeName);
  result.canError = canError;
  return result;
}

// In parser.cpp (implementation)
std::unique_ptr<BlockExprAST> Parser::parseString(const std::string& source) {
  std::istringstream ss(source);
  lexer.resetInput(ss);  // point the lexer at a new stream; keeps the buffer
                         // and position reset without touching the shared DFA
  lexer.setEmitComments(collectComments_);
  comments_.clear();  // don't carry state across inputs
  tokenStack.clear();
  prevTok_ = Token::eof({0, 0, 0});
  getNextToken();  // Prime the first token
  return parseProgram();
}

// Parse class definition: class ClassName implements Interface1, Interface2 {
// fields and methods }
unique_ptr<ClassDefinitionAST> Parser::parseClassDefinition() {
  Position start = captureStart();
  const bool packed = curTok.kind == TokenKind::PACKED_CLASS;
  getNextToken();  // eat 'class' or 'packed_class'

  if (curTok.kind != TokenKind::IDENTIFIER) {
    throwIdentifierError(packed ? "expected class name after 'packed_class'"
                           : "expected class name after 'class'");
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
      throwIdentifierError("expected type parameter name after '<'");
      return nullptr;
    }

    expectCurrentTokenKind(TokenKind::GREATER,
                           "expected '>' after type parameters");
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

        consumeGreater("expected '>' after interface type arguments");
      }

      implementedInterfaces.push_back(std::move(iface));

      if (curTok.kind == TokenKind::COMMA) {
        getNextToken();  // eat ','
      } else {
        break;
      }
    }

    if (implementedInterfaces.empty()) {
      throwIdentifierError("expected interface name after 'implements'");
      return nullptr;
    }
  }

  expectCurrentTokenKind(TokenKind::BRACE_OPEN,
                         "expected '{' after class name");
  getNextToken();  // eat '{'

  std::vector<ClassFieldDecl> fields;
  std::vector<ClassMethodDecl> methods;

  // Parse class body (fields and methods)
  while (curTok.kind != TokenKind::BRACE_CLOSE &&
         curTok.kind != TokenKind::TOK_EOF) {
    Position memberStart = captureStart();
    sun::Visibility memberVis = parsePublic() ? sun::Visibility::Public
                                              : sun::Visibility::Private;
    bool isConstMethod = parseConstModifier();
    if (curTok.kind == TokenKind::VAR) {
      if (isConstMethod)
        parsingError(
            "'const' is not allowed on fields; only methods can be declared "
            "'const function'");
      // Parse field declaration: var name: type;
      getNextToken();  // eat 'var'

      expectCurrentTokenKind(TokenKind::IDENTIFIER,
                             "expected field name in class definition");

      Position fieldLoc = captureStart();  // Location before eating token
      fieldLoc.setEnd(curTok.end.line, curTok.end.column, curTok.end.offset);
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

      fields.push_back(
          {std::move(fieldName), std::move(fieldType), fieldLoc, memberVis});
    } else if (curTok.kind == TokenKind::FUNCTION) {
      // Parse method declaration
      auto func = parseFunction();
      if (!func) return nullptr;
      func->setVisibility(memberVis);
      if (memberVis == sun::Visibility::Public || isConstMethod)
        extendSpanStart(*func, memberStart);

      bool isConstructor = (func->getProto().getName() == "init");
      if (isConstructor && isConstMethod)
        parsingError("'init' cannot be a const method");
      func->getProtoMut().setConstMethod(isConstMethod);

      ClassMethodDecl method;
      method.function = std::move(func);
      method.isConstructor = isConstructor;
      method.isConst = isConstMethod;
      methods.push_back(std::move(method));

      // Skip optional semicolons after methods
      while (curTok.kind == TokenKind::SEMI_COLON) getNextToken();
    } else {
      parsingError(
          "expected 'var' (field) or 'function' (method) in class body");
      return nullptr;
    }
  }

  expectCurrentTokenKind(TokenKind::BRACE_CLOSE,
                         "expected '}' at end of class definition");
  getNextToken();  // eat '}'

  return finishNode(
      std::make_unique<ClassDefinitionAST>(
          std::move(className), std::move(typeParameters),
          std::move(implementedInterfaces), std::move(fields),
          std::move(methods)),
      start);
}

// Parse interface definition: interface InterfaceName<T, U> { fields and
// methods } Methods can have optional default implementations
unique_ptr<InterfaceDefinitionAST> Parser::parseInterfaceDefinition() {
  Position start = captureStart();
  getNextToken();  // eat 'interface'

  expectCurrentTokenKind(TokenKind::IDENTIFIER,
                         "expected interface name after 'interface'");

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
      throwIdentifierError("expected type parameter name after '<'");
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
    Position memberStart = captureStart();
    sun::Visibility memberVis = parsePublic() ? sun::Visibility::Public
                                              : sun::Visibility::Private;
    bool isConstMethod = parseConstModifier();
    if (curTok.kind == TokenKind::VAR) {
      if (isConstMethod)
        parsingError(
            "'const' is not allowed on fields; only methods can be declared "
            "'const function'");
      // Parse field declaration: var name: type;
      getNextToken();  // eat 'var'

      if (curTok.kind != TokenKind::IDENTIFIER) {
        throwIdentifierError("expected field name in interface definition");
        return nullptr;
      }

      Position fieldLoc = captureStart();  // Location before eating token
      fieldLoc.setEnd(curTok.end.line, curTok.end.column, curTok.end.offset);
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

      fields.push_back(
          {std::move(fieldName), std::move(fieldType), fieldLoc, memberVis});
    } else if (curTok.kind == TokenKind::FUNCTION) {
      // Parse method declaration (may have default implementation)
      // Interface methods can be:
      // 1. Just a signature: function name(args) returnType;
      // 2. Full method with default impl: function name(args) returnType { body
      // }

      Position methodStart = captureStart();
      getNextToken();  // eat 'function'

      // Allow both regular identifiers and intrinsic identifiers
      if (curTok.kind != TokenKind::IDENTIFIER &&
          curTok.kind != TokenKind::INTRINSIC_IDENTIFIER) {
        throwIdentifierError("Expected method name in interface");
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
          throwIdentifierError("expected type parameter name after '<'");
          return nullptr;
        }

        expectCurrentTokenKind(TokenKind::GREATER,
                               "expected '>' after type parameters");
        getNextToken();  // eat '>'
      }

      expectCurrentTokenKind(TokenKind::PAREN_OPEN,
                             "Expected '(' in method declaration");

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

      expectCurrentTokenKind(TokenKind::PAREN_CLOSE,
                             "Expected ')' in method declaration");
      getNextToken();  // eat ')'

      // Check for return type
      std::optional<TypeAnnotation> retType;
      if (curTok.kind != TokenKind::BRACE_OPEN &&
          curTok.kind != TokenKind::SEMI_COLON) {
        retType = parseTypeAnnotation();
      }

      // Signature span ends at the last token before the body/semicolon
      Position protoLoc = methodStart;
      protoLoc.setEnd(prevTok_.end.line, prevTok_.end.column,
                      prevTok_.end.offset);

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
      proto->setLocation(std::move(protoLoc));
      proto->setConstMethod(isConstMethod);
      auto func = finishNode(
          std::make_unique<FunctionAST>(std::move(proto), std::move(body)),
          (memberVis == sun::Visibility::Public || isConstMethod) ? memberStart
                                                                 : methodStart);
      func->setVisibility(memberVis);

      InterfaceMethodDecl method;
      method.function = std::move(func);
      method.hasDefaultImpl = hasDefaultImpl;
      method.isConst = isConstMethod;
      methods.push_back(std::move(method));

      // Skip optional semicolons after methods
      while (curTok.kind == TokenKind::SEMI_COLON) getNextToken();
    } else {
      parsingError(
          "expected 'var' (field) or 'function' (method) in interface body");
      return nullptr;
    }
  }

  expectCurrentTokenKind(TokenKind::BRACE_CLOSE,
                         "expected '}' at end of interface definition");
  getNextToken();  // eat '}'

  return finishNode(std::make_unique<InterfaceDefinitionAST>(
                        std::move(interfaceName), std::move(typeParameters),
                        std::move(fields), std::move(methods)),
                    start);
}

// Parse enum definition: enum Name { Variant1, Variant2, ... }
// Syntax: enum ColorName { Red, Green, Blue }
unique_ptr<EnumDefinitionAST> Parser::parseEnumDefinition() {
  Position start = captureStart();
  getNextToken();  // eat 'enum'

  expectCurrentTokenKind(TokenKind::IDENTIFIER,
                         "expected enum name after 'enum'");

  std::string enumName = curTok.getIdentifier().value();
  getNextToken();  // eat enum name

  // Optional type parameters: enum Option<T> { ... }
  std::vector<std::string> typeParameters;
  if (curTok.kind == TokenKind::LESS) {
    getNextToken();  // eat '<'
    while (curTok.kind == TokenKind::IDENTIFIER) {
      typeParameters.push_back(curTok.getIdentifier().value());
      getNextToken();  // eat type parameter name
      if (curTok.kind == TokenKind::COMMA) {
        getNextToken();  // eat ','
      } else {
        break;
      }
    }
    consumeGreater("expected '>' after enum type parameters");
  }

  expectCurrentTokenKind(TokenKind::BRACE_OPEN, "expected '{' after enum name");
  getNextToken();  // eat '{'

  std::vector<EnumVariantDecl> variants;
  int64_t nextValue = 0;  // Auto-incrementing value for variants

  // Parse enum variants: Variant1, Variant2, ...
  while (curTok.kind != TokenKind::BRACE_CLOSE &&
         curTok.kind != TokenKind::TOK_EOF) {
    expectCurrentTokenKind(TokenKind::IDENTIFIER,
                           "expected variant name in enum definition");

    Position variantLoc = captureStart();
    variantLoc.setEnd(curTok.end.line, curTok.end.column, curTok.end.offset);
    std::string variantName = curTok.getIdentifier().value();
    getNextToken();  // eat variant name

    // Optional payload types: Circle(f64), Rect(f64, f64)
    std::vector<TypeAnnotation> payloadTypes;
    if (curTok.kind == TokenKind::PAREN_OPEN) {
      getNextToken();  // eat '('
      if (curTok.kind == TokenKind::PAREN_CLOSE) {
        parsingError("variant payload requires at least one type");
        return nullptr;
      }
      while (true) {
        payloadTypes.push_back(parseTypeAnnotation());
        if (curTok.kind != TokenKind::COMMA) break;
        getNextToken();  // eat ','
      }
      expectCurrentTokenKind(TokenKind::PAREN_CLOSE,
                             "expected ')' after variant payload types");
      getNextToken();  // eat ')'
    }

    int64_t variantValue = nextValue++;

    // TODO: Support explicit value assignment: Red = 1
    // For now, just auto-increment

    variants.push_back({std::move(variantName), variantValue, variantLoc,
                        std::move(payloadTypes)});

    // Handle optional comma between variants
    if (curTok.kind == TokenKind::COMMA) {
      getNextToken();  // eat ','
    } else if (curTok.kind != TokenKind::BRACE_CLOSE) {
      // Allow trailing comma or no comma before '}'
      parsingError("expected ',' or '}' after enum variant");
      return nullptr;
    }
  }

  expectCurrentTokenKind(TokenKind::BRACE_CLOSE,
                         "expected '}' at end of enum definition");
  getNextToken();  // eat '}'

  if (variants.empty()) {
    parsingError("enum must have at least one variant");
    return nullptr;
  }

  return finishNode(
      std::make_unique<EnumDefinitionAST>(std::move(enumName),
                                          std::move(variants),
                                          /*precompiled=*/false,
                                          std::move(typeParameters)),
      start);
}

// Parse throw expression: throw <expr>
// Syntax: throw errorExpr;
unique_ptr<ExprAST> Parser::parseThrow() {
  Position start = captureStart();
  getNextToken();  // eat 'throw'

  // Parse the error expression being thrown
  auto errorExpr = parseExpression();
  if (!errorExpr) {
    parsingError("expected expression after 'throw'");
    return nullptr;
  }

  return finishNode(std::make_unique<ThrowExprAST>(std::move(errorExpr)),
                    start);
}

// Parse spawn expression: spawn(lambda)
// Creates an OS thread that runs the lambda and returns Thread<T>
unique_ptr<ExprAST> Parser::parseSpawn() {
  Position loc = captureStart();
  getNextToken();  // eat 'spawn'

  // Expect '('
  if (curTok.kind != TokenKind::PAREN_OPEN) {
    parsingError("expected '(' after 'spawn'");
    return nullptr;
  }
  getNextToken();  // eat '('

  // Parse the lambda expression
  auto lambdaExpr = parseExpression();
  if (!lambdaExpr) {
    parsingError("expected lambda expression in 'spawn'");
    return nullptr;
  }

  // Expect ')'
  if (curTok.kind != TokenKind::PAREN_CLOSE) {
    parsingError("expected ')' after spawn argument");
    return nullptr;
  }
  getNextToken();  // eat ')'

  return finishNode(std::make_unique<SpawnExprAST>(std::move(lambdaExpr)),
                    loc);
}

// Parse unsafe block: unsafe { ... }
unique_ptr<ExprAST> Parser::parseUnsafeBlock() {
  Position loc = captureStart();
  getNextToken();  // eat 'unsafe'

  if (curTok.kind != TokenKind::BRACE_OPEN) {
    parsingError("expected '{' after 'unsafe'");
    return nullptr;
  }

  auto body = parseBlock();
  if (!body) {
    parsingError("expected block after 'unsafe'");
    return nullptr;
  }

  return finishNode(std::make_unique<UnsafeBlockAST>(std::move(body)), loc);
}

// Parse try-catch expression: try { ... } catch (e: IError) { ... }
// Note: 'try' has already been consumed; we're at '{'
unique_ptr<ExprAST> Parser::parseTryCatch() {
  // Parse try block - we're already at '{' ('try' was consumed by the caller,
  // which re-stamps the span to include it)
  Position start = captureStart();
  auto tryBlock = parseBlock();
  if (!tryBlock) {
    parsingError("expected block after 'try'");
    return nullptr;
  }

  // Expect at least one 'catch'
  if (curTok.kind != TokenKind::CATCH) {
    parsingError("expected 'catch' after try block");
    return nullptr;
  }

  // Parse one or more catch clauses: catch (name: Type) { ... }
  std::vector<CatchClause> catchClauses;
  while (curTok.kind == TokenKind::CATCH) {
    getNextToken();  // eat 'catch'

    if (curTok.kind != TokenKind::PAREN_OPEN) {
      parsingError("expected '(' after 'catch'");
      return nullptr;
    }
    getNextToken();  // eat '('

    CatchClause catchClause;

    if (curTok.kind != TokenKind::IDENTIFIER) {
      throwIdentifierError("expected binding name in catch clause");
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

    if (curTok.kind != TokenKind::BRACE_OPEN) {
      parsingError("expected '{' to start catch body");
      return nullptr;
    }

    catchClause.body = parseBlock();
    if (!catchClause.body) return nullptr;

    catchClauses.push_back(std::move(catchClause));
  }

  return finishNode(std::make_unique<TryCatchExprAST>(
                        std::move(tryBlock), std::move(catchClauses)),
                    start);
}