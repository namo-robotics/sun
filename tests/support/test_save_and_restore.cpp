#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <type_traits>

#include "support/save_and_restore.h"

/** Nested guards restore the immediately enclosing value. */
TEST(SaveAndRestore, RestoresNestedReplacements) {
  bool value = false;
  {
    sun::support::SaveAndRestore<bool> outer(value, true);
    EXPECT_TRUE(value);
    {
      sun::support::SaveAndRestore<bool> inner(value, false);
      EXPECT_FALSE(value);
    }
    EXPECT_TRUE(value);
  }
  EXPECT_FALSE(value);
}

/** Saved objects are restored when an exception interrupts their mutation. */
TEST(SaveAndRestore, RestoresObjectsOnException) {
  std::string value = "original";
  try {
    sun::support::SaveAndRestore<std::string> guard(value);
    value = "changed";
    throw std::runtime_error("interrupted");
  } catch (const std::runtime_error&) {
  }
  EXPECT_EQ(value, "original");
}

static_assert(!std::is_copy_constructible_v<sun::support::SaveAndRestore<bool>>);
static_assert(!std::is_move_constructible_v<sun::support::SaveAndRestore<bool>>);
static_assert(!std::is_copy_assignable_v<sun::support::SaveAndRestore<bool>>);
static_assert(!std::is_move_assignable_v<sun::support::SaveAndRestore<bool>>);
