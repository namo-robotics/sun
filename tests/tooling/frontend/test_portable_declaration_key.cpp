#include <gtest/gtest.h>

#include "semantic_analysis/analysis_results.h"
#include "semantic_analysis/declaration_table.h"
#include "semantic_analysis/portable_declaration_key.h"
#include "types/types.h"

using sun::semantic_analysis::AnalysisResults;
using sun::semantic_analysis::DeclarationKind;
using sun::semantic_analysis::DeclarationTable;
using sun::semantic_analysis::TypeRegistry;
using sun::types::TypePtr;
using sun::types::Types;

/** Keeps test fixtures and helpers local to this source file. */
namespace {
using sun::semantic_analysis::PortableDeclarationKey;
using sun::semantic_analysis::PortableTypeKey;

/** A fixed artifact identity for portable encoding regression tests. */
const std::string bundle(64, 'a');
}  // namespace

TEST(Tooling_Frontend_PortableIdentity, symbols_have_a_frozen_encoding) {
  auto key = PortableDeclarationKey::original(bundle, 6);
  EXPECT_EQ(
      key.symbol("function"),
      "_SUN1_907b3a260be289f76a3596962f74a0e6ab8e2c93163365102c2195eca4841cac");
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
  EXPECT_NE(PortableDeclarationKey::specialization(
                origin, {i32}, std::vector<PortableTypeKey>{boolean})
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
  DeclarationTable table;
  auto first = table.add(DeclarationKind::Function, "first");
  auto second = table.add(DeclarationKind::Function, "second");
  table.bindPortable(first, key);
  EXPECT_EQ(table.findPortable(key), first);
  EXPECT_NO_THROW(table.bindPortable(first, key));
  EXPECT_ANY_THROW(table.bindPortable(second, key));
  EXPECT_ANY_THROW(
      table.bindPortable(first, PortableDeclarationKey::original(bundle, 7)));
  EXPECT_FALSE(table.get(second).portableKey);
  DeclarationTable another;
  another.add(DeclarationKind::Variable, "unrelated");
  auto imported = another.add(DeclarationKind::Function, "first");
  another.bindPortable(imported, key);
  EXPECT_NE(first, imported);
  EXPECT_EQ(table.get(first).portableKey, another.get(imported).portableKey);
}

TEST(Tooling_Frontend_PortableIdentity,
     semantic_instances_ignore_allocation_order) {
  auto make = [&](bool reverse) {
    AnalysisResults registryResults;
    TypeRegistry& registry = *registryResults.types;
    auto& table = registryResults.declarations;
    if (reverse) table.add(DeclarationKind::Variable, "unrelated");
    auto source = table.add(DeclarationKind::Class, "Box");
    auto argument = table.add(DeclarationKind::Class, "Private");
    table.bindPortable(source, PortableDeclarationKey::original(bundle, 1));
    table.bindPortable(argument, PortableDeclarationKey::original(bundle, 2));
    auto nominal = registry.getClass(argument);
    nominal->addField("recursive", Types::RawPointer(nominal));
    if (reverse)
      registry.specialize({source, {}, {Types::Bool()}, std::nullopt});
    auto instance = registry.specialize({source, {}, {nominal}, std::nullopt});
    return PortableDeclarationKey::fromDeclaration(instance, table);
  };
  EXPECT_EQ(make(false), make(true));
}

TEST(Tooling_Frontend_PortableIdentity,
     semantic_packs_and_enclosing_owners_are_distinct) {
  AnalysisResults registryResults;
  TypeRegistry& registry = *registryResults.types;
  auto& table = registryResults.declarations;
  auto source = table.add(DeclarationKind::Function, "apply");
  auto owner = table.add(DeclarationKind::Class, "Owner");
  table.bindPortable(source, PortableDeclarationKey::original(bundle, 1));
  table.bindPortable(owner, PortableDeclarationKey::original(bundle, 2));
  auto absent = registry.specialize({source, {}, {}, std::nullopt});
  auto empty = registry.specialize({source, {}, {}, std::vector<TypePtr>{}});
  auto enclosed = registry.specialize({source, owner, {}, std::nullopt});
  auto key = [&](sun::semantic_analysis::DeclarationId id) {
    return PortableDeclarationKey::fromDeclaration(id, table).symbol(
        "function");
  };
  EXPECT_NE(key(absent), key(empty));
  EXPECT_NE(key(absent), key(enclosed));
  auto parameter = table.add(DeclarationKind::Parameter, "args", source);
  table.bindPortable(parameter, PortableDeclarationKey::original(bundle, 3));
  auto first = table.add(DeclarationKind::Parameter, "args.0", enclosed, {}, {},
                         parameter, "variadic-element", 0);
  auto second = table.add(DeclarationKind::Parameter, "args.1", enclosed, {},
                          {}, parameter, "variadic-element", 1);
  EXPECT_NE(key(first), key(second));
}

TEST(Tooling_Frontend_PortableIdentity,
     semantic_type_encoding_matches_identity) {
  DeclarationTable table;
  auto i32 = Types::Int32();
  auto lambda =
      std::make_shared<sun::types::LambdaType>(i32, std::vector<TypePtr>{i32});
  auto borrowed =
      std::make_shared<sun::types::LambdaType>(i32, std::vector<TypePtr>{i32});
  borrowed->setHasRefCaptures(true);
  std::vector<TypePtr> types{i32,
                             Types::Bool(),
                             Types::Slice(),
                             Types::NullPointer(),
                             Types::RawPointer(i32),
                             Types::StaticPointer(i32),
                             Types::Reference(i32),
                             Types::Reference(i32, false),
                             Types::Array(i32, {}),
                             Types::Array(i32, {2, 3}),
                             Types::Array(i32, {3, 2}),
                             Types::Array(Types::RawPointer(i32), {2}),
                             Types::Array(Types::StaticPointer(i32), {2}),
                             Types::Reference(Types::RawPointer(i32)),
                             Types::Reference(Types::StaticPointer(i32)),
                             std::make_shared<sun::types::ErrorUnionType>(i32),
                             std::make_shared<sun::types::FunctionType>(
                                 i32, std::vector<TypePtr>{i32}),
                             std::make_shared<sun::types::FunctionType>(
                                 i32, std::vector<TypePtr>{i32}, true),
                             std::make_shared<sun::types::FunctionType>(
                                 i32, std::vector<TypePtr>{i32}, false, true),
                             lambda,
                             borrowed};
  for (size_t i = 0; i < types.size(); ++i)
    for (size_t j = 0; j < types.size(); ++j) {
      SCOPED_TRACE(std::to_string(i) + "," + std::to_string(j));
      sun::semantic_analysis::SpecializationKey left{
          {}, {}, {types[i]}, std::nullopt};
      sun::semantic_analysis::SpecializationKey right{
          {}, {}, {types[j]}, std::nullopt};
      EXPECT_EQ(left == right, PortableTypeKey::fromType(*types[i], table) ==
                                   PortableTypeKey::fromType(*types[j], table));
    }
  auto lifetime = PortableTypeKey::fromType(*borrowed, table);
  borrowed->setLifetimeName("renamed");
  EXPECT_EQ(lifetime, PortableTypeKey::fromType(*borrowed, table));
  auto reference = std::make_shared<sun::types::ReferenceType>(i32);
  auto refKey = PortableTypeKey::fromType(*reference, table);
  reference->setLifetimeName("different");
  reference->setClassLifetimeArgs({"a", "b"});
  EXPECT_EQ(refKey, PortableTypeKey::fromType(*reference, table));
}

TEST(Tooling_Frontend_PortableIdentity,
     semantic_conversion_rejects_missing_and_foreign_identity) {
  AnalysisResults registryResults;
  TypeRegistry& registry = *registryResults.types;
  auto& table = registryResults.declarations;
  auto id = table.add(DeclarationKind::Class, "Private");
  auto type = registry.getClass(id);
  EXPECT_ANY_THROW(PortableTypeKey::fromType(*type, table));
  table.bindPortable(id, PortableDeclarationKey::original(bundle, 1));
  EXPECT_NO_THROW(PortableTypeKey::fromType(*type, table));
  AnalysisResults otherResults;
  TypeRegistry& other = *otherResults.types;
  auto otherId =
      otherResults.declarations.add(DeclarationKind::Class, "Private");
  EXPECT_EQ(id, otherId);
  otherResults.declarations.bindPortable(
      otherId, PortableDeclarationKey::original(bundle, 1));
  EXPECT_ANY_THROW(PortableTypeKey::fromType(*type, otherResults.declarations));
  EXPECT_ANY_THROW(
      PortableTypeKey::fromType(*Types::TypeParameter("T"), table));
}

TEST(Tooling_Frontend_PortableIdentity,
     pack_presence_and_unsafe_symbols_are_frozen) {
  auto origin = PortableDeclarationKey::original(bundle, 6);
  auto i32 = PortableTypeKey::primitive("i32");
  EXPECT_EQ(
      PortableDeclarationKey::specialization(origin, {i32}).symbol("function"),
      "_SUN1_a5c9bfc8ad33bf284e44bf31a599ccf775b4f3fa9c59dfece04efffb0a3b3222");
  EXPECT_EQ(
      PortableDeclarationKey::specialization(origin, {i32},
                                             std::vector<PortableTypeKey>{})
          .symbol("function"),
      "_SUN1_efd1d82485b55ff40f82bba5e46914854e6964bc5a2eddd07af7d7d1500d0b26");
  auto callable =
      PortableTypeKey::function(i32, {i32}, false, false, false, true);
  EXPECT_EQ(
      PortableDeclarationKey::specialization(origin, {callable})
          .symbol("function"),
      "_SUN1_9588ac0357d3c9ba9fcd51354eb6fb5e200dc1d6a20fff46ff6bc199ac815de1");
}

TEST(Tooling_Frontend_PortableIdentity,
     interned_instances_preserve_nested_pointer_layouts) {
  AnalysisResults registryResults;
  TypeRegistry& registry = *registryResults.types;
  auto source = registryResults.declarations.add(DeclarationKind::Class, "Box");
  auto raw = Types::Array(Types::RawPointer(Types::Int32()), {2});
  auto immortal = Types::Array(Types::StaticPointer(Types::Int32()), {2});
  auto first = registry.specialize({source, {}, {raw}, std::nullopt});
  auto second = registry.specialize({source, {}, {immortal}, std::nullopt});
  EXPECT_NE(first, second);
  EXPECT_EQ(first, registry.specialize({source, {}, {raw}, std::nullopt}));
  EXPECT_EQ(second,
            registry.specialize({source, {}, {immortal}, std::nullopt}));
}

/** Identifier spelling does not encode declaration semantics. */
TEST(Tooling_Frontend_PortableIdentity, keys_are_opaque) {
  for (const auto* value :
       {"arbitrary identifier", "$hash$_0", "01", "specialization"})
    EXPECT_EQ(PortableDeclarationKey::fromString(value).encoding(), value);
  EXPECT_ANY_THROW(PortableDeclarationKey::fromString(""));
}

TEST(Tooling_Frontend_PortableIdentity, imported_ownership_is_interned_once) {
  auto owner = PortableDeclarationKey::original(bundle, 1).encoding();
  auto child = PortableDeclarationKey::original(bundle, 2).encoding();
  std::vector<sun::semantic_analysis::LibraryDeclarationRecord> records{
      {owner, static_cast<uint32_t>(DeclarationKind::Module), "lib", {}, {}},
      {child, static_cast<uint32_t>(DeclarationKind::Function), "read", owner,
       owner}};
  DeclarationTable first, second;
  second.add(DeclarationKind::Variable, "unrelated");
  first.importLibraryDeclarationRecords(records);
  second.importLibraryDeclarationRecords(records);
  auto firstId = first.findPortable(PortableDeclarationKey::fromString(child));
  auto secondId =
      second.findPortable(PortableDeclarationKey::fromString(child));
  EXPECT_NE(firstId, secondId);
  EXPECT_EQ(PortableDeclarationKey::fromDeclaration(firstId, first),
            PortableDeclarationKey::fromDeclaration(secondId, second));
  auto size = first.size();
  first.importLibraryDeclarationRecords(records);
  EXPECT_EQ(first.size(), size);
  records[1].name = "conflicting";
  EXPECT_ANY_THROW(first.importLibraryDeclarationRecords(records));
  records[1].name = "read";
  records[1].owner = PortableDeclarationKey::original(bundle, 9).encoding();
  EXPECT_ANY_THROW(first.importLibraryDeclarationRecords(records));
}

/** Derived identities remain deterministic without a category suffix. */
TEST(Tooling_Frontend_PortableIdentity, derived_keys_are_opaque_hashes) {
  auto source = PortableDeclarationKey::fromString("template");
  auto instance = PortableDeclarationKey::specialization(source, {});
  EXPECT_EQ(PortableDeclarationKey::fromString(instance.encoding()), instance);
  EXPECT_EQ(instance.encoding().size(), 65);
  EXPECT_EQ(instance.encoding().find("$_"), std::string::npos);
  EXPECT_EQ(instance, PortableDeclarationKey::specialization(source, {}));
  EXPECT_NE(instance, PortableDeclarationKey::inInstance(source, source));
  EXPECT_NE(instance, PortableDeclarationKey::generated(source, "instance", 0));
}

/** Import order follows explicit links even when key and input order disagree.
 */
TEST(Tooling_Frontend_PortableIdentity, opaque_records_restore_ownership) {
  using Record = sun::semantic_analysis::LibraryDeclarationRecord;
  const auto module = static_cast<uint32_t>(DeclarationKind::Module);
  const auto function = static_cast<uint32_t>(DeclarationKind::Function);
  std::vector<Record> records{
      {"first", function, "read", "last", "last", "artifact"},
      {"last", module, "lib", {}, {}, "artifact"}};
  DeclarationTable table;
  table.importLibraryDeclarationRecords(records);
  const auto child =
      table.findPortable(PortableDeclarationKey::fromString("first"));
  const auto owner =
      table.findPortable(PortableDeclarationKey::fromString("last"));
  EXPECT_EQ(table.get(child).owner, owner);
  EXPECT_EQ(table.get(child).module, owner);
  EXPECT_EQ(table.get(child).bundleHash, "artifact");
  EXPECT_NO_THROW(table.importLibraryDeclarationRecords(records));
  EXPECT_EQ(table.importedSyntax("first", DeclarationKind::Function, "read"),
            child);
  EXPECT_ANY_THROW(
      table.importedSyntax("first", DeclarationKind::Class, "read"));
  records[0].bundleHash = "different artifact";
  EXPECT_ANY_THROW(table.importLibraryDeclarationRecords(records));
}

/** Missing and cyclic ownership links are invalid regardless of key spelling.
 */
TEST(Tooling_Frontend_PortableIdentity, invalid_ownership_graphs_are_rejected) {
  const auto kind = static_cast<uint32_t>(DeclarationKind::Module);
  DeclarationTable missing, cyclic, self;
  EXPECT_ANY_THROW(missing.importLibraryDeclarationRecords(
      {{"child", kind, "child", "absent", {}}}));
  EXPECT_ANY_THROW(cyclic.importLibraryDeclarationRecords(
      {{"a", kind, "a", "b", {}}, {"b", kind, "b", "a", {}}}));
  EXPECT_ANY_THROW(
      self.importLibraryDeclarationRecords({{"a", kind, "a", "a", {}}}));
}
