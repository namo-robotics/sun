#pragma once

#include <memory>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"

struct ManifestMoonDependency {
  std::string path;                  // local path; empty when url is set
  std::optional<std::string> url;    // downloaded to the moon cache
  std::optional<std::string> hash;   // lowercase hex SHA-256 of the file
  std::optional<std::string> rename;
};

struct ManifestSunDependency {
  std::string path;
  std::optional<std::string> hash;
};

// A .proto schema imported natively: the compiler synthesizes Sun classes
// (one per message) into a module named after the proto package.
struct ManifestProtoDependency {
  std::string path;
};

class ManifestAST : public ExprAST {
  std::vector<ManifestSunDependency> suns;
  std::vector<ManifestMoonDependency> moons;
  std::vector<ManifestProtoDependency> protos;

 public:
  ManifestAST(std::vector<ManifestSunDependency> suns,
              std::vector<ManifestMoonDependency> moons,
              std::vector<ManifestProtoDependency> protos = {})
      : suns(std::move(suns)),
        moons(std::move(moons)),
        protos(std::move(protos)) {}

  ASTNodeType getType() const override { return ASTNodeType::MANIFEST; }
  std::string toString() const override { return "manifest"; }

  const std::vector<ManifestSunDependency>& getSuns() const { return suns; }
  const std::vector<ManifestMoonDependency>& getMoons() const { return moons; }
  const std::vector<ManifestProtoDependency>& getProtos() const {
    return protos;
  }
  std::string dotLabel() const override { return "Manifest"; }
};
