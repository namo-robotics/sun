#pragma once

#include <type_traits>
#include <utility>

/** Shared utilities used throughout the compiler. */
namespace sun::support {

/** Saves a copy of a value and restores it when the guard leaves scope.
 * The referenced value must outlive the guard and support nonthrowing move
 * assignment so restoration is safe during exception unwinding. */
template <typename T>
class SaveAndRestore {
  static_assert(std::is_nothrow_move_assignable_v<T>,
                "restoring a value must not throw");

 public:
  /** Saves the current value before the caller changes it. */
  explicit SaveAndRestore(T& value) : value_(value), saved_(value) {}

  /** Saves the current value and installs a temporary replacement. */
  SaveAndRestore(T& value, T replacement) : SaveAndRestore(value) {
    value_ = std::move(replacement);
  }

  /** Restores the saved value, including when an exception leaves the scope. */
  ~SaveAndRestore() { value_ = std::move(saved_); }

  /** Prevents two guards from owning the same saved state. */
  SaveAndRestore(const SaveAndRestore&) = delete;
  /** Prevents replacing ownership of saved state. */
  SaveAndRestore& operator=(const SaveAndRestore&) = delete;

 private:
  T& value_;
  T saved_;
};

}  // namespace sun::support
