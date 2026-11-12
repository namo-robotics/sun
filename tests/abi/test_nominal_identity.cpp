/** Exercises revision-independent nominal origins without weakening code
 * identity. */
#include <gtest/gtest.h>

#include "abi/nominal_identity.h"
#include "driver/manifest_processor.h"
#include "parsing/parser.h"
#include "semantic_analysis/portable_declaration_key.h"
#include "serialization/ast_deserializer.h"
#include "serialization/ast_serializer.h"
#include "support/error.h"

/** Keeps the identity fixture names local to this translation unit. */
namespace {
using sun::abi::NominalIdentity;
using sun::abi::NominalKind;
using sun::semantic_analysis::PortableDeclarationKey;
}  // namespace

/** Revisions may share a nominal origin while their code symbols stay isolated.
 */
TEST(ABI_NominalIdentity, RevisionsKeepDistinctImplementationSymbols) {
  const NominalIdentity first("org.example/shapes", NominalKind::Class,
                              {"shapes", "Point"});
  const NominalIdentity revised("org.example/shapes", NominalKind::Class,
                                {"shapes", "Point"});
  EXPECT_EQ(first, revised);
  // The new revision also moves Point's source ordinal. Neither artifact
  // hash nor declaration ordering participates in the nominal origin.
  const auto original =
      PortableDeclarationKey::original(std::string(64, 'a'), 3);
  const auto replacement =
      PortableDeclarationKey::original(std::string(64, 'b'), 9);
  EXPECT_NE(original, replacement);
  EXPECT_NE(original.symbol("type"), replacement.symbol("type"));
  EXPECT_NE(original.symbol("function"), replacement.symbol("function"));
}

/** Matching names from unrelated library namespaces must not equate their
 * types. */
TEST(ABI_NominalIdentity, LibraryPathAndKindArePartOfIdentity) {
  const NominalIdentity point("org.example/shapes", NominalKind::Class,
                              {"shapes", "Point"});
  EXPECT_NE(point, NominalIdentity("other/shapes", NominalKind::Class,
                                   {"shapes", "Point"}));
  EXPECT_NE(point, NominalIdentity("org.example/shapes", NominalKind::Class,
                                   {"other", "Point"}));
  EXPECT_NE(point, NominalIdentity("org.example/shapes", NominalKind::Interface,
                                   {"shapes", "Point"}));
  EXPECT_NE(point, NominalIdentity("org.example/shapes", NominalKind::Enum,
                                   {"shapes", "Point"}));
}

/** Concatenation, dotted names, and embedded zeros cannot merge path
 * components. */
TEST(ABI_NominalIdentity, ComponentsHaveUnambiguousBoundaries) {
  EXPECT_NE(NominalIdentity("lib", NominalKind::Class, {"ab", "c"}),
            NominalIdentity("lib", NominalKind::Class, {"a", "bc"}));
  EXPECT_NE(NominalIdentity("lib", NominalKind::Class, {"a.b", "c"}),
            NominalIdentity("lib", NominalKind::Class, {"a", "b.c"}));
  EXPECT_NE(
      NominalIdentity("lib", NominalKind::Class, {std::string("a\0b", 3)}),
      NominalIdentity("lib", NominalKind::Class, {"a", "b"}));
  EXPECT_NE(NominalIdentity("lib:a", NominalKind::Class, {"b"}),
            NominalIdentity("lib", NominalKind::Class, {"a:b"}));
}

/** Missing origins are errors rather than implicit global namespaces. */
TEST(ABI_NominalIdentity, RejectsIncompleteOrigins) {
  EXPECT_THROW((NominalIdentity("", NominalKind::Class, {"Point"})),
               sun::support::SunError);
  EXPECT_THROW((NominalIdentity("lib", NominalKind::Class, {})),
               sun::support::SunError);
  EXPECT_THROW((NominalIdentity("lib", NominalKind::Class, {"shapes", ""})),
               sun::support::SunError);
  EXPECT_THROW(
      (NominalIdentity("lib", static_cast<NominalKind>(100), {"Point"})),
      sun::support::SunError);
}

/** Freezes the framed encoding so later changes require an explicit revision.
 */
TEST(ABI_NominalIdentity, CanonicalEncodingHasAnExplicitRevision) {
  const NominalIdentity point("org.example/shapes", NominalKind::Class,
                              {"shapes", "Point"});
  EXPECT_EQ(point.encoding(),
            "sun.nominal.v1:"
            "a1e597408853a63976613306c1b2d339e051a32b07c528f33ae83131f805425d");
}

/** Library IDs preserve arbitrary valid text and enforce a byte limit. */
TEST(ABI_NominalIdentity, LibraryIdsPreserveTextAndLimitBytes) {
  for (const auto& id : {std::string("org.example/shapes"),
                         std::string("04B519E8-4339-4A5E-BAC1-743376951068"),
                         std::string(" \t名前\n"), std::string("a\0b", 3),
                         std::string(512, 'x')}) {
    EXPECT_NO_THROW(sun::abi::validateLibraryId(id));
  }
  EXPECT_THROW(sun::abi::validateLibraryId(""), sun::support::SunError);
  EXPECT_THROW(sun::abi::validateLibraryId(std::string(513, 'x')),
               sun::support::SunError);
  EXPECT_THROW(sun::abi::validateLibraryId(std::string("\xff", 1)),
               sun::support::SunError);
  std::string unicode;
  for (int i = 0; i < 256; ++i) {
    unicode += "é";
  }
  EXPECT_NO_THROW(sun::abi::validateLibraryId(unicode));
  EXPECT_THROW(sun::abi::validateLibraryId(unicode + "x"),
               sun::support::SunError);
  EXPECT_NE(NominalIdentity("Shapes", NominalKind::Class, {"Point"}),
            NominalIdentity("shapes", NominalKind::Class, {"Point"}));
  EXPECT_NE(NominalIdentity("shapes ", NominalKind::Class, {"Point"}),
            NominalIdentity("shapes", NominalKind::Class, {"Point"}));
  EXPECT_NE(NominalIdentity("é", NominalKind::Class, {"Point"}),
            NominalIdentity("e\xcc\x81", NominalKind::Class, {"Point"}));
}

/** Source manifests declare dependencies rather than artifact identities. */
TEST(ABI_NominalIdentity, ManifestIdentityIsRejected) {
  for (const auto& source :
       {"manifest { id: \"library\" }", "manifest { id: \"\" }"}) {
    auto parser = sun::parsing::Parser::createStringParser(source);
    EXPECT_THROW(parser.parseProgram(), sun::support::SunError);
  }
  auto parser = sun::parsing::Parser::createStringParser("manifest {}");
  EXPECT_NO_THROW(parser.parseProgram());
}

/** Default and explicit local build names use the same package qualification.
 */
TEST(ABI_NominalIdentity, PackagePrefixesBuildIdentity) {
  sun::driver::SunConfig config;
  /** Restores the source configuration even if identity validation throws. */
  struct ConfigScope {
    const sun::driver::SunConfig* previous;
    /** Installs the fixture's project configuration. */
    explicit ConfigScope(const sun::driver::SunConfig& value)
        : previous(
              sun::driver::ManifestProcessor::exchangeSourceConfig(&value)) {}
    /** Restores the previous project configuration. */
    ~ConfigScope() {
      sun::driver::ManifestProcessor::exchangeSourceConfig(previous);
    }
  } scope(config);
  EXPECT_EQ(sun::driver::ManifestProcessor::artifactIdentity("source.sun"),
            "source");
  config.packageId = "namo-robotics/sun";
  EXPECT_EQ(sun::driver::ManifestProcessor::artifactIdentity("source.sun"),
            "namo-robotics/sun/source");
  EXPECT_EQ(
      sun::driver::ManifestProcessor::artifactIdentity("source.sun", "stdlib"),
      "namo-robotics/sun/stdlib");
  config.packageId = std::string(512, 'p');
  EXPECT_THROW(sun::driver::ManifestProcessor::artifactIdentity("source.sun"),
               sun::support::SunError);
}

/** Existing bundles may still contain serialized manifest identity metadata. */
TEST(ABI_NominalIdentity, LegacyManifestIdentityStillDeserializes) {
  sun::proto::ast::ASTNode node;
  node.mutable_manifest()->set_id("legacy/library");
  sun::serialization::ASTDeserializer deserializer;
  auto restored = deserializer.deserialize(node);
  const auto& manifest = static_cast<const sun::ast::ManifestAST&>(*restored);
  EXPECT_EQ(manifest.getId(), "legacy/library");
  sun::serialization::ASTSerializer serializer;
  EXPECT_EQ(serializer.serialize(manifest).manifest().id(), "legacy/library");
  node.mutable_manifest()->set_id("");
  EXPECT_THROW(deserializer.deserialize(node), sun::support::SunError);
}
