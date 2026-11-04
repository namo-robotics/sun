/** Checks diagnostic provenance in source and serialized interface claims. */
#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <sstream>

#include "driver/driver.h"
#include "parsing/parser.h"
#include "semantic_analysis/semantic_analyzer.h"
#include "serialization/ast_deserializer.h"
#include "serialization/ast_serializer.h"
#include "support/display_path.h"

/** Builds syntax fixtures without requiring valid interface definitions. */
namespace {
/** Parses a class whose interface is unavailable to the consumer. */
std::unique_ptr<sun::ast::BlockExprAST> missingInterface() {
  std::istringstream input;
  sun::parsing::Parser parser(input);
  parser.setFilePath("library.sun");
  return parser.parseString(
      "\n\n/** Requires an absent interface. */\n"
      "class Broken implements Missing {}\n");
}

/** Runs semantic checks and preserves structured diagnostics for assertions. */
std::optional<sun::support::SunError> failureFor(sun::ast::BlockExprAST& tree) {
  auto results = std::make_shared<sun::semantic_analysis::AnalysisResults>();
  sun::semantic_analysis::SemanticAnalyzer analyzer(results);
  try {
    analyzer.pipeline().run(tree);
  } catch (const sun::support::SunError& error) {
    return error;
  }
  return std::nullopt;
}
}  // namespace

/** Unknown interfaces point to the actual token and identify their class. */
TEST(Tooling_Diagnostics_Locations, implemented_interface_source) {
  auto tree = missingInterface();
  auto error = failureFor(*tree);
  ASSERT_TRUE(error);
  ASSERT_TRUE(error->getLocation());
  EXPECT_EQ(error->getLocation()->filePath, "library.sun");
  EXPECT_EQ(error->getLocation()->line, 4);
  EXPECT_EQ(error->getLocation()->column, 25);
  EXPECT_NE(error->getMessage().find("class 'Broken'"), std::string::npos);
}

/** Serializing declarations preserves interface-token diagnostic locations. */
TEST(Tooling_Diagnostics_Locations, implemented_interface_round_trip) {
  auto tree = missingInterface();
  auto encoded = sun::serialization::ASTSerializer().serialize(*tree);
  auto decoded = sun::serialization::ASTDeserializer().deserialize(encoded);
  auto* block = dynamic_cast<sun::ast::BlockExprAST*>(decoded.get());
  ASSERT_NE(block, nullptr);
  auto error = failureFor(*block);
  ASSERT_TRUE(error);
  ASSERT_TRUE(error->getLocation());
  EXPECT_EQ(error->getLocation()->filePath, "library.sun");
  EXPECT_EQ(error->getLocation()->line, 4);
  EXPECT_EQ(error->getLocation()->column, 25);
}

/** Old declarations use the owning class when interface spans are absent. */
TEST(Tooling_Diagnostics_Locations, legacy_interface_uses_class_location) {
  auto tree = missingInterface();
  auto encoded = sun::serialization::ASTSerializer().serialize(*tree);
  auto* declaration = encoded.mutable_block_expr()->mutable_body(0);
  declaration->mutable_class_def()
      ->mutable_implemented_interfaces(0)
      ->clear_location();
  auto decoded = sun::serialization::ASTDeserializer().deserialize(encoded);
  auto* block = dynamic_cast<sun::ast::BlockExprAST*>(decoded.get());
  ASSERT_NE(block, nullptr);
  auto error = failureFor(*block);
  ASSERT_TRUE(error);
  ASSERT_TRUE(error->getLocation());
  EXPECT_EQ(error->getLocation()->filePath, "library.sun");
  EXPECT_EQ(error->getLocation()->line, 4);
  EXPECT_EQ(error->getLocation()->column, 1);
}

/** Bundles without positions name their origin without inventing line one. */
TEST(Tooling_Diagnostics_Locations, locationless_bundle_names_origin) {
  auto tree = missingInterface();
  auto encoded = sun::serialization::ASTSerializer({.include_location = false})
                     .serialize(*tree);
  auto decoded = sun::serialization::ASTDeserializer(
                     {.default_file_path = "/dependencies/legacy.moon"})
                     .deserialize(encoded);
  auto* block = dynamic_cast<sun::ast::BlockExprAST*>(decoded.get());
  ASSERT_NE(block, nullptr);
  auto error = failureFor(*block);
  ASSERT_TRUE(error);
  EXPECT_NE(std::string(error->what()).find("/dependencies/legacy.moon"),
            std::string::npos);
  EXPECT_EQ(std::string(error->what()).find("1:1"), std::string::npos);
  EXPECT_NE(error->getMessage().find("class 'Broken'"), std::string::npos);
}

/** Display policy changes diagnostics without changing stored source identity.
 */
TEST(Tooling_Diagnostics_Locations, relative_display_preserves_location) {
  auto base = std::filesystem::current_path() / "tmp" / "display paths";
  sun::support::DisplayPathScope scope(base);
  const auto path = (base / "src" / "broken.sun").string();
  sun::support::SunError error(sun::support::SunError::Kind::Type,
                               "Unknown type",
                               sun::support::Position(7, 12, 0, path));
  EXPECT_NE(std::string(error.what()).find("src/broken.sun:7:12"),
            std::string::npos);
  EXPECT_EQ(error.getLocation()->filePath, path);
  EXPECT_EQ(sun::support::displayPath(base / "src" / ".." / "main.sun"),
            "main.sun");
  EXPECT_EQ(sun::support::displayPath(base.parent_path() / "elsewhere.sun"),
            (base.parent_path() / "elsewhere.sun").string());
  EXPECT_EQ(sun::support::displayPath(base.string() + "-other/main.sun"),
            base.string() + "-other/main.sun");
  {
    sun::support::DisplayPathScope nested(base / "src");
    EXPECT_EQ(sun::support::displayPath(path), "broken.sun");
  }
  EXPECT_EQ(sun::support::displayPath(path), "src/broken.sun");
}

/** Each interface and nested argument keeps its own span through serialization.
 */
TEST(Tooling_Diagnostics_Locations, generic_arguments_round_trip) {
  std::istringstream input;
  sun::parsing::Parser parser(input);
  parser.setFilePath("generic.sun");
  const std::string source =
      "/** Describes a constrained generic value. */\n"
      "class Box<T> implements First, Second<Missing> {}";
  auto tree = parser.parseString(source);
  auto encoded = sun::serialization::ASTSerializer().serialize(*tree);
  auto decoded = sun::serialization::ASTDeserializer().deserialize(encoded);
  auto* block = dynamic_cast<sun::ast::BlockExprAST*>(decoded.get());
  ASSERT_NE(block, nullptr);
  auto* cls =
      dynamic_cast<sun::ast::ClassDefinitionAST*>(block->getBody()[0].get());
  ASSERT_NE(cls, nullptr);
  const auto& interfaces = cls->getImplementedInterfaces();
  ASSERT_EQ(interfaces.size(), 2u);
  ASSERT_TRUE(interfaces[0].location);
  ASSERT_TRUE(interfaces[1].location);
  EXPECT_EQ(interfaces[0].location->offset, source.find("First"));
  EXPECT_EQ(interfaces[1].location->offset, source.find("Second"));
  ASSERT_EQ(interfaces[1].typeArguments.size(), 1u);
  const auto& argument = interfaces[1].typeArguments[0];
  EXPECT_EQ(argument.span.filePath, "generic.sun");
  EXPECT_EQ(argument.span.offset, source.find("Missing"));
  auto stripped = sun::serialization::ASTSerializer({.include_location = false})
                      .serialize(*tree);
  const auto& claim =
      stripped.block_expr().body(0).class_def().implemented_interfaces(1);
  EXPECT_FALSE(claim.has_location());
  EXPECT_FALSE(claim.type_arguments(0).has_location());
}

/** Generic instantiation retains the interface span from its template. */
TEST(Tooling_Diagnostics_Locations, generic_interface_source) {
  const std::string source =
      "/** Requires an absent generic interface. */\n"
      "class Broken<T> implements Missing<T> {}\n"
      "/** Instantiates the invalid class. */\n"
      "function main() i32 { var value = Broken<i32>(); return 0; }";
  auto driver = sun::driver::Driver::createForJIT();
  auto program = driver->analyzeString(source, "generic.sun");
  ASSERT_TRUE(program.error);
  ASSERT_TRUE(program.error->getLocation());
  EXPECT_EQ(program.error->getLocation()->filePath, "generic.sun");
  EXPECT_EQ(program.error->getLocation()->offset, source.find("Missing"));
  EXPECT_NE(program.error->getMessage().find("class 'Broken'"),
            std::string::npos);
}

/** Synthetic source names are identifiers, not files relative to a directory.
 */
TEST(Tooling_Diagnostics_Locations, anonymous_source_names_stay_synthetic) {
  sun::support::DisplayPathScope scope(std::filesystem::current_path() / "tmp");
  EXPECT_EQ(sun::support::displayPath("<string:7>"), "<string:7>");
  EXPECT_EQ(sun::support::displayPath(""), "");
}
