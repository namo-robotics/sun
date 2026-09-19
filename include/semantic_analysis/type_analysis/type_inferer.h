/** Computes types from supplied facts without accessing a semantic session. */
#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "types/types.h"

/** Pure type computation shared by semantic checking operations. */
namespace sun::semantic_analysis::type_analysis {
/** Describes a failure without emitting a diagnostic or changing compiler
 * state. */
struct InferenceFailure {
  /** Identifies the missing fact or incompatible type shape. */
  enum class Kind {
    MissingType,
    EmptyArray,
    NotArray,
    IndexDimensions,
    BranchMismatch,
    NotCallable,
    UnboundParameter
  };
  Kind kind;
  sun::types::TypePtr left;
  sun::types::TypePtr right;
  std::string parameter;
};
/** Either a computed value or a failure for the semantic caller to report. */
template <class T>
using InferenceResult = std::variant<T, InferenceFailure>;
/** A computed expression type or an explanation of why it is unavailable. */
using TypeResult = InferenceResult<sun::types::TypePtr>;

/** Stateless operations; supplied type descriptions are never modified. */
class TypeInferer {
 public:
  /** Choose a literal's default type, or retain its resolved suffix type. */
  static TypeResult number(bool integer, uint64_t magnitude, bool negative,
                           sun::types::TypePtr suffix = nullptr);
  /** Select the promoted numeric type, preserving existing signedness rules. */
  static TypeResult numeric(const sun::types::TypePtr& left,
                            const sun::types::TypePtr& right);
  /** Compute a unary result after the caller has checked the operator. */
  static TypeResult unary(const sun::types::TypePtr& operand, bool logicalNot);
  /** Build a reference after semantic checking has validated its target. */
  static TypeResult reference(const sun::types::TypePtr& target,
                              bool mutableReference);
  /** Flatten an array's dimensions and apply the supplied compatible hint. */
  static TypeResult array(const sun::types::TypePtr& firstElement, size_t count,
                          sun::types::TypePtr expectedElement,
                          bool useExpectedElement);
  /** Determine an array element type, checking the supplied index count. */
  static TypeResult index(const sun::types::TypePtr& target, size_t count);
  /** Select a branch result using compatibility established by the caller. */
  static TypeResult branches(const sun::types::TypePtr& left,
                             const sun::types::TypePtr& right, bool leftToRight,
                             bool rightToLeft);
  /** Read the result from the selected callable signature. */
  static TypeResult call(const sun::types::TypePtr& callable);
};
}  // namespace sun::semantic_analysis::type_analysis
