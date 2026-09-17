#include <gtest/gtest.h>

#include "semantic_analysis/declaration_table.h"
#include "semantic_analysis/portable_declaration_key.h"

namespace {
using sun::PortableDeclarationKey;
using sun::PortableTypeKey;

/** A fixed artifact identity for portable encoding regression tests. */
const std::string bundle(64, 'a');
}  // namespace

TEST(Tooling_Frontend_PortableIdentity, symbols_have_a_frozen_encoding) {
  auto key = PortableDeclarationKey::original(bundle, 6);
  EXPECT_EQ(
      key.symbol("function"),
      "_SUN1_c2db7def6b72feb55dc8e30d5b9d1623b06a78b8f26480ed2004205b5fca0372");
  EXPECT_NE(key.symbol("function"), key.symbol("type"));
  EXPECT_EQ(key, PortableDeclarationKey::original(bundle, 6));
}

TEST(Tooling_Frontend_PortableIdentity, artifact_and_declaration_both_matter) {
  auto key = PortableDeclarationKey::original(bundle, 6);
  EXPECT_NE(key.symbol("function"),
            PortableDeclarationKey::original(bundle, 7).symbol("function"));
  EXPECT_NE(key.symbol("function"),
            PortableDeclarationKey::original(std::string(64, 'b'), 6)
                .symbol("function"));
  EXPECT_ANY_THROW(PortableDeclarationKey::original("short", 6));
  EXPECT_ANY_THROW(PortableDeclarationKey::original(bundle, 0));
  EXPECT_ANY_THROW(PortableDeclarationKey().symbol("function"));
}

TEST(Tooling_Frontend_PortableIdentity,
     instances_use_structural_type_arguments) {
  auto origin = PortableDeclarationKey::original(bundle, 6);
  auto i32 = PortableTypeKey::primitive("i32");
  auto boolean = PortableTypeKey::primitive("bool");
  auto a = PortableDeclarationKey::specialization(origin, {i32});
  auto b = PortableDeclarationKey::specialization(origin, {boolean});
  EXPECT_NE(a.symbol("function"), b.symbol("function"));
  EXPECT_EQ(a, PortableDeclarationKey::specialization(origin, {i32}));
  EXPECT_NE(PortableDeclarationKey::specialization(origin, {i32}, {boolean})
                .symbol("function"),
            PortableDeclarationKey::specialization(origin, {i32, boolean})
                .symbol("function"));
  auto local = PortableDeclarationKey::original(bundle, 8);
  EXPECT_NE(PortableDeclarationKey::inInstance(local, a).symbol("type"),
            PortableDeclarationKey::inInstance(local, b).symbol("type"));
}

TEST(Tooling_Frontend_PortableIdentity,
     type_properties_cannot_be_lost_in_spelling) {
  auto i32 = PortableTypeKey::primitive("i32");
  EXPECT_FALSE(PortableTypeKey::reference(i32, true) ==
               PortableTypeKey::reference(i32, false));
  EXPECT_FALSE(PortableTypeKey::pointer(i32, true) ==
               PortableTypeKey::pointer(i32, false));
  EXPECT_FALSE(PortableTypeKey::array(i32, {3}) ==
               PortableTypeKey::array(i32, {5}));
  EXPECT_FALSE(PortableTypeKey::array(i32, {2, 3}) ==
               PortableTypeKey::array(i32, {23}));
  EXPECT_FALSE(PortableTypeKey::function(i32, {i32}, true) ==
               PortableTypeKey::function(i32, {i32}, false));
  EXPECT_FALSE(
      PortableTypeKey::nominal(PortableDeclarationKey::original(bundle, 2)) ==
      PortableTypeKey::nominal(PortableDeclarationKey::original(bundle, 3)));
  EXPECT_ANY_THROW(PortableTypeKey::primitive("Point"));
}

TEST(Tooling_Frontend_PortableIdentity, generated_keys_delimit_role_and_slot) {
  auto origin = PortableDeclarationKey::original(bundle, 6);
  EXPECT_NE(PortableDeclarationKey::generated(origin, "wrapper", 12)
                .symbol("function"),
            PortableDeclarationKey::generated(origin, "wrapper1", 2)
                .symbol("function"));
  EXPECT_ANY_THROW(PortableDeclarationKey::generated(origin, "", 1));
}

TEST(Tooling_Frontend_PortableIdentity,
     one_portable_key_has_one_session_identity) {
  auto key = PortableDeclarationKey::original(bundle, 6);
  sun::DeclarationTable table;
  auto first = table.add(sun::DeclarationKind::Function, "first");
  auto second = table.add(sun::DeclarationKind::Function, "second");
  table.bindPortable(first, key);
  EXPECT_EQ(table.findPortable(key), first);
  EXPECT_NO_THROW(table.bindPortable(first, key));
  EXPECT_ANY_THROW(table.bindPortable(second, key));
  EXPECT_ANY_THROW(
      table.bindPortable(first, PortableDeclarationKey::original(bundle, 7)));
  EXPECT_FALSE(table.get(second).portableKey);
  sun::DeclarationTable another;
  another.add(sun::DeclarationKind::Variable, "unrelated");
  auto imported = another.add(sun::DeclarationKind::Function, "first");
  another.bindPortable(imported, key);
  EXPECT_NE(first, imported);
  EXPECT_EQ(table.get(first).portableKey, another.get(imported).portableKey);
}
