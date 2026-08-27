#pragma once

#include <memory>
#include <string>

#include "ast/block_expr_ast.h"
#include "ast/expr_ast.h"

struct ManifestMoonDependency {
  std::string path;                 // local path; empty when url is set
  std::optional<std::string> url;   // downloaded to the moon cache
  std::optional<std::string> hash;  // lowercase hex SHA-256 of the file
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

// A native static library (.a) carried inside the .moon being built, so
// importers link against it without naming -l flags. Only meaningful when
// building a bundle.
struct ManifestArchiveDependency {
  std::string path;
};

// Dependencies that only apply when compiling for one operating system,
// from a `target: { <os>: { ... } }` block. The manifest never decides the
// target — --target (or the host) does, at compile time; a block simply says
// which sources belong to builds for that OS. This is how the stdlib carries
// target_linux.sun and target_darwin.sun in one manifest.
struct ManifestTargetBlock {
  std::string os;  // "linux", "macos" or "windows"
  std::vector<ManifestSunDependency> suns;
  std::vector<ManifestMoonDependency> moons;
  std::vector<ManifestProtoDependency> protos;
  std::vector<ManifestArchiveDependency> archives;
};

class ManifestAST : public ExprAST {
  std::vector<ManifestSunDependency> suns;
  std::vector<ManifestMoonDependency> moons;
  std::vector<ManifestProtoDependency> protos;
  std::vector<ManifestArchiveDependency> archives;
  std::vector<ManifestTargetBlock> targets;

 public:
  ManifestAST(std::vector<ManifestSunDependency> suns,
              std::vector<ManifestMoonDependency> moons,
              std::vector<ManifestProtoDependency> protos = {},
              std::vector<ManifestArchiveDependency> archives = {},
              std::vector<ManifestTargetBlock> targets = {})
      : suns(std::move(suns)),
        moons(std::move(moons)),
        protos(std::move(protos)),
        archives(std::move(archives)),
        targets(std::move(targets)) {}

  ASTNodeType getType() const override { return ASTNodeType::MANIFEST; }
  std::string toString() const override { return "manifest"; }

  const std::vector<ManifestSunDependency>& getSuns() const { return suns; }
  const std::vector<ManifestMoonDependency>& getMoons() const { return moons; }
  const std::vector<ManifestProtoDependency>& getProtos() const {
    return protos;
  }
  const std::vector<ManifestArchiveDependency>& getArchives() const {
    return archives;
  }
  const std::vector<ManifestTargetBlock>& getTargets() const {
    return targets;
  }
  std::string dotLabel() const override { return "Manifest"; }
};
