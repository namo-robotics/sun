// sun_value.h — Runtime value type for Sun language
// Represents any primitive value returned from program execution

#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <variant>

namespace sun {

// Represents a void return (no value)
struct VoidValue {
  bool operator==(const VoidValue&) const { return true; }
};

// SunValue can hold any primitive type that main() might return
using SunValue = std::variant<VoidValue,   // void
                              bool,        // bool
                              int8_t,      // i8
                              int16_t,     // i16
                              int32_t,     // i32
                              int64_t,     // i64
                              float,       // f32
                              double,      // f64
                              std::string  // string
                              >;

// Helper to check if value is void
inline bool isVoid(const SunValue& v) {
  return std::holds_alternative<VoidValue>(v);
}

// Helper to get numeric value as double (for backward compatibility)
inline double toDouble(const SunValue& v) {
  return std::visit(
      [](auto&& arg) -> double {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, VoidValue>) {
          return 0.0;
        } else if constexpr (std::is_same_v<T, bool>) {
          return arg ? 1.0 : 0.0;
        } else if constexpr (std::is_same_v<T, std::string>) {
          return 0.0;  // strings can't convert to double
        } else {
          return static_cast<double>(arg);
        }
      },
      v);
}

// Helper to print a SunValue
inline std::ostream& operator<<(std::ostream& os, const SunValue& v) {
  std::visit(
      [&os](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, VoidValue>) {
          os << "void";
        } else if constexpr (std::is_same_v<T, bool>) {
          os << (arg ? "true" : "false");
        } else if constexpr (std::is_same_v<T, int8_t>) {
          os << static_cast<int>(arg);  // print as number, not char
        } else {
          os << arg;
        }
      },
      v);
  return os;
}

// Comparison helpers for testing
template <typename T>
bool operator==(const SunValue& v, T expected) {
  if constexpr (std::is_same_v<T, int>) {
    // Allow comparison with int - check both i32 and i64
    if (auto* p = std::get_if<int32_t>(&v)) return *p == expected;
    if (auto* p = std::get_if<int64_t>(&v))
      return *p == static_cast<int64_t>(expected);
    return false;
  } else if constexpr (std::is_floating_point_v<T>) {
    // Allow comparison with any float type
    if (auto* p = std::get_if<float>(&v))
      return *p == static_cast<float>(expected);
    if (auto* p = std::get_if<double>(&v))
      return *p == static_cast<double>(expected);
    return false;
  } else if constexpr (std::is_same_v<T, const char*> ||
                       std::is_same_v<T, char*>) {
    // Allow comparison with C strings - compare against std::string
    if (auto* p = std::get_if<std::string>(&v)) return *p == expected;
    return false;
  } else {
    if (auto* p = std::get_if<T>(&v)) return *p == expected;
    return false;
  }
}

template <typename T>
bool operator==(T expected, const SunValue& v) {
  return v == expected;
}

}  // namespace sun
