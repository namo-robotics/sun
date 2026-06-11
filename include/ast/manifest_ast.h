#pragma once

#include <memory>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"

struct ManifestMoonDependency {
  std::string path;
  std::optional<std::string> hash;
  std::optional<std::string> rename;
};

struct ManifestSunDependency {
  std::string path;
  std::optional<std::string> hash;
};

class ManifestAST : public ExprAST {
  std::vector<ManifestSunDependency> suns;
  std::vector<ManifestMoonDependency> moons;

 public:
  ManifestAST(std::vector<ManifestSunDependency> suns,
              std::vector<ManifestMoonDependency> moons)
      : suns(std::move(suns)), moons(std::move(moons)) {}

  ASTNodeType getType() const override { return ASTNodeType::MANIFEST; }
  std::string toString() const override { return "manifest"; }

  const std::vector<ManifestSunDependency>& getSuns() const { return suns; }
  const std::vector<ManifestMoonDependency>& getMoons() const { return moons; }
  std::string dotLabel() const override { return "Manifest"; }
};
