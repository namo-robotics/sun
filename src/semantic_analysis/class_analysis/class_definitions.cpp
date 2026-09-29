/** Checks class definitions within the semantic session. */
#include <algorithm>
#include <set>

#include "semantic_analysis/class_analysis/field_initialization.h"
#include "semantic_analysis/declaration_analysis/declaration_rules.h"
#include "semantic_analysis/item_refs.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "semantic_analysis/symbol_names.h"
#include "support/error.h"

using sun::ast::PrototypeAST;
using sun::support::logAndThrowError;
using sun::types::TypePtr;
using sun::types::Types;

/** Resolves declarations and checks the types and meaning of Sun programs. */
namespace sun::semantic_analysis {
void ClassAnalyzer::analyzeClassDefinition(
    sun::ast::ClassDefinitionAST& classDef) {
  const std::string& baseName = classDef.getName();

  // Partial classes: add methods to the primary class.
  if (classDef.isPartial()) {
    analyzePartialClass(classDef, classDef);
    return;
  }

  const QualifiedName& qualifiedClass = classDef.getQualifiedName();

  // Forbid redefinition of a class in the same scope
  if (ctx_.declarations().isDeclared(baseName, ctx_.scope())) {
    logAndThrowError("Redefinition of class '" + baseName + "'",
                     classDef.getLocation());
  }

  // Validate class name
  validateNotReserved(classDef.getName(), "Class name", classDef.getLocation());

  // Check for redefinition of builtin types
  if (ctx_.types()->isBuiltinTypeName(classDef.getName())) {
    logAndThrowError(
        "Cannot redefine builtin type '" + classDef.getName() + "'",
        classDef.getLocation());
  }

  // Validate field names and reject duplicates. Names alone are compared, so
  // this runs before the generic early-return below: a template's field types
  // have no meaning until a specialization substitutes them, but a repeated
  // name is wrong at the declaration either way.
  validateFieldNames(classDef.getFields(), "Field name",
                     "class '" + baseName + "'");

  // Validate method names
  for (const auto& methodDecl : classDef.getMethods()) {
    const std::string& methodName = methodDecl.function->getProto().getName();
    validateNotReserved(methodName, "Method name",
                        methodDecl.function->getLocation());
  }

  // Lifetime declarations must be distinct, and every lifetime a field
  // names must be the builtin 'this or declared on the class
  rejectDuplicateLifetimes(classDef.getLifetimeParameters(),
                           " on class '" + baseName + "'");
  {
    SemanticContext::LifetimeScopeGuard lifetimes(ctx_, true);
    for (const auto& lp : classDef.getLifetimeParameters()) {
      ctx_.declareLifetime(lp.name);
    }
    for (const auto& field : classDef.getFields()) {
      sema_.declarations().checkAnnotationLifetimes(field.type, field.location);
    }
  }

  // Register in generic class table if this is a generic class or has
  // generic methods (needed for instantiateGenericMethod to find the def)
  if (classDef.isGeneric() || classDef.hasGenericMethods()) {
    GenericClassInfo genericInfo;
    genericInfo.AST = &classDef;
    genericInfo.typeParameters = classDef.getTypeParameters();
    genericInfo.definitionScope = ctx_.scope()->shared_from_this();
    genericInfo.qualifiedName = qualifiedClass;
    ctx_.currentScope().declareGenericClass(baseName, genericInfo);

    // Generic class templates are not analyzed further until instantiated
    if (classDef.isGeneric()) {
      classDef.setResolvedType(Types::Void());
      return;
    }
  }

  // Create the class type with the qualified name
  auto classType =
      ctx_.types()->getClass(classDef.getDeclarationId(), qualifiedClass);

  // Layout must be decided before any getStructType() call memoizes it
  classType->setPacked(classDef.isPacked());
  classType->visibility = classDef.getVisibility();
  classType->setLifetimeParams(
      sun::ast::lifetimeParameterNames(classDef.getLifetimeParameters()));

  // Register the class BEFORE processing fields to allow self-referential
  // types (e.g., var next: raw_ptr<Node> inside class Node)
  ctx_.currentScope().declareClass(baseName, classType);

  // Fields and method signatures are normally registered by the
  // declaration pre-pass (registerClassShape); classes analyzed outside a
  // pre-passed block register them here.
  bool shapeRegistered =
      ctx_.declarations().hasClassShape(classDef.getDeclarationId());
  if (!shapeRegistered) {
    sema_.pipeline().declarations().registerClassShape(classDef, qualifiedClass,
                                                       classType);
  }

  // Inherit interface fields BEFORE analyzing methods
  // This adds interface fields to the class, which methods may access
  sema_.interfaces().inheritInterfaceFields(classDef, classType);

  // Merge methods from any pending class extensions
  // Extensions are collected during import processing and merged here
  // so all methods (primary + extensions) can call each other
  const auto* extensions = ctx_.declarations().pendingExtensions(baseName);
  if (extensions) {
    for (sun::ast::ClassDefinitionAST* extDef : *extensions) {
      // Validate: check for duplicate methods
      for (const auto& extMethod : extDef->getMethods()) {
        const std::string& methodName =
            extMethod.function->getProto().getName();
        // Check against primary's methods
        for (const auto& primaryMethod : classDef.getMethods()) {
          if (primaryMethod.function->getProto().getName() == methodName) {
            logAndThrowError("Method '" + methodName +
                                 "' already defined in class '" + baseName +
                                 "'",
                             extMethod.function->getLocation());
          }
        }
        // Check against other extension methods already merged
        // (The getMutableMethods approach handles this via sequential
        // merge)
      }
      // Merge extension methods into the primary class definition
      for (auto& extMethod : extDef->getMutableMethods()) {
        classDef.getMutableMethods().push_back(std::move(extMethod));
      }
    }
    // Clear the pending extensions for this class (they're now merged)
    ctx_.declarations().clearPendingExtensions(baseName);
  }

  // Save old class context and set new one
  auto savedClass = ctx_.getCurrentClass();
  ctx_.setCurrentClass(classType);

  // Enter a Class scope to contain all method scopes in the tree
  ctx_.enterClassScope(qualifiedClass);

  // The class's lifetime names are usable in every member signature
  {
    SemanticContext::LifetimeScopeGuard lifetimes(ctx_);
    for (const auto& lp : classDef.getLifetimeParameters()) {
      ctx_.declareLifetime(lp.name);
    }

    // Install defaults before checking bodies that may call them.
    sema_.interfaces().validateInterfaceImplementation(classDef, classType);

    // PASS 1: Make the (already registered) method signatures resolvable
    // by source name inside the class scope
    for (const auto& methodDecl : classDef.getMethods()) {
      const PrototypeAST& proto = methodDecl.function->getProto();
      std::string methodNameForScope = proto.getName();
      std::vector<TypePtr> methodParamTypes;
      methodParamTypes.push_back(classType);  // this parameter
      for (const auto& pt : proto.getResolvedParamTypes()) {
        methodParamTypes.push_back(pt);
      }
      TypePtr returnType = proto.hasResolvedReturnType()
                               ? proto.getResolvedReturnType()
                               : Types::Void();
      FunctionInfo methodInfo{returnType, methodParamTypes, {}};
      methodInfo.declarationId = proto.getDeclarationId();
      ctx_.currentScope().declareFunction(methodNameForScope, methodInfo,
                                          ctx_.currentLocation());
    }

    // PASS 2: Analyze all method bodies using their assigned names.
    for (const auto& methodDecl : classDef.getMethods()) {
      const auto& proto = methodDecl.function->getProto();
      auto* member = classType->getMethodForArgs(proto.getName(),
                                                 proto.getResolvedParamTypes());
      if (member && member->defaultImplementation) {
        auto module = ctx_.results()
                          .declarations.get(member->defaultImplementation)
                          .module;
        auto* scope =
            module ? ctx_.lookupModuleScope(module) : &ctx_.rootScope();
        SemanticContext::ScopeSwitchGuard definitionScope(ctx_, scope);
        std::vector<std::string> names;
        std::vector<TypePtr> arguments;
        for (const auto& [name, argument] : proto.getTypeBindings()) {
          names.push_back(name);
          arguments.push_back(argument);
        }
        if (proto.isGeneric()) {
          if (!names.empty()) ctx_.enterTypeParamScope(names, arguments);
          auto info =
              sema_.declarations().getFunctionInfo(*methodDecl.function);
          sema_.declarations().applyFunctionInfoToProto(
              const_cast<PrototypeAST&>(proto), info);
          sema_.bodies().analyzeFunction(*methodDecl.function);
        } else
          sema_.bodies().analyzeMethodWithBindings(*methodDecl.function,
                                                   classType, names, arguments);
      } else
        sema_.bodies().analyzeFunction(*methodDecl.function);
    }

    // PASS 3: check constructors, now that every method body is analyzed — the
    // walk follows constructor calls into helper bodies, and what it finds
    // there (a bound method reference, say) is only marked once the helper has
    // been analyzed. A precompiled class carries signatures without bodies; its
    // constructors were checked when the bundle was built.
    if (!classDef.isPrecompiled()) {
      for (const auto& methodDecl : classDef.getMethods()) {
        if (!methodDecl.isConstructor) continue;
        sun::semantic_analysis::checkFieldInitialization(
            *methodDecl.function, *classType, classDef.getMethods());
      }
    }
  }
  ctx_.exitScope();  // Class scope

  // Restore old class context
  ctx_.setCurrentClass(savedClass);

  // Track symbol for redefinition detection
  ctx_.declarations().noteDeclared(baseName, ctx_.scope());

  // Store primary AST for partial class merging (if a partial appears
  // later)
  ctx_.currentScope().declareClassDefinition(baseName, classDef);

  // Set resolved type to the class type so codegen can get the qualified
  // name
  classDef.setResolvedType(classType);
}

void ClassAnalyzer::analyzePartialClass(sun::ast::ClassDefinitionAST& classDef,
                                        ExprAST& expr) {
  const std::string& baseName = classDef.getName();

  auto existingClass = ctx_.lookupClass(baseName);
  if (existingClass) {
    classDef.setTargetDeclarationId(existingClass->getDeclarationId());
    // Primary already analyzed — validate and merge methods now
    for (const auto& extMethod : classDef.getMethods()) {
      const std::string& methodName = extMethod.function->getProto().getName();
      if (existingClass->getMethod(methodName)) {
        logAndThrowError("Method '" + methodName +
                             "' already defined in class '" + baseName + "'",
                         extMethod.function->getLocation());
      }
    }

    // Register and analyze extension methods on the existing class
    auto savedClass = ctx_.getCurrentClass();
    ctx_.setCurrentClass(existingClass);

    // Enter a Class scope to contain extension method scopes
    ctx_.enterClassScope(existingClass->getQualifiedName());

    // Register all extension methods first
    for (const auto& methodDecl : classDef.getMethods()) {
      FunctionInfo methodInfo =
          sema_.declarations().getFunctionInfo(*methodDecl.function);
      PrototypeAST& proto =
          const_cast<PrototypeAST&>(methodDecl.function->getProto());

      // Apply computed info to prototype
      sema_.declarations().applyFunctionInfoToProto(proto, methodInfo);

      auto& method = existingClass->addMethod(
          proto.getName(), methodInfo.returnType, methodInfo.paramTypes,
          methodDecl.isConstructor, proto.getTypeParameterNames(),
          proto.canThrow());
      method.declarationId = proto.getDeclarationId();
      if (proto.getName() == "deinit")
        existingClass->deinitializer = method.declarationId;
      method.visibility = methodVisibility(*methodDecl.function);
      method.isConst = methodDecl.isConst;
      method.isUnsafe = methodDecl.function->getProto().isUnsafeMethod();
      std::string methodNameForScope = proto.getName();
      std::vector<TypePtr> methodParamTypes;
      methodParamTypes.push_back(existingClass);
      for (const auto& pt : methodInfo.paramTypes) {
        methodParamTypes.push_back(pt);
      }
      methodInfo.paramTypes = std::move(methodParamTypes);
      ctx_.currentScope().declareFunction(methodNameForScope, methodInfo,
                                          ctx_.currentLocation());
    }

    // Analyze extension method bodies
    for (const auto& methodDecl : classDef.getMethods()) {
      sema_.bodies().analyzeFunction(*methodDecl.function);
    }
    // The parser rejects constructors in a partial class, so this is only a
    // backstop — and like the primary path it runs after every body is
    // analyzed, since the walk follows calls into them
    if (!classDef.isPrecompiled()) {
      for (const auto& methodDecl : classDef.getMethods()) {
        if (!methodDecl.isConstructor) continue;
        sun::semantic_analysis::checkFieldInitialization(
            *methodDecl.function, *existingClass, classDef.getMethods());
      }
    }

    ctx_.exitScope();  // Class scope

    // Merge methods into primary AST so codegen generates them
    for (auto* s = ctx_.scope(); s != nullptr; s = s->parent) {
      auto it = std::find_if(s->classDefinitions.begin(),
                             s->classDefinitions.end(), [&](const auto& entry) {
                               return entry.second->getDeclarationId() ==
                                      classDef.getTargetDeclarationId();
                             });
      if (it != s->classDefinitions.end()) {
        for (auto& extMethod : classDef.getMutableMethods()) {
          it->second->getMutableMethods().push_back(std::move(extMethod));
        }
        break;
      }
    }

    ctx_.setCurrentClass(savedClass);
  } else {
    // Primary not yet seen — stash for merging when primary is analyzed
    ctx_.declarations().deferExtension(baseName, &classDef);
  }
  expr.setResolvedType(Types::Void());
}

}  // namespace sun::semantic_analysis
