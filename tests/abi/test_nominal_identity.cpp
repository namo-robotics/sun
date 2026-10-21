/** Exercises revision-independent nominal origins without weakening code
 * identity. */
#include <gtest/gtest.h>

#include "abi/nominal_identity.h"
#include "semantic_analysis/portable_declaration_key.h"
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
