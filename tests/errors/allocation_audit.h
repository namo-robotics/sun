/** Defines an explicit interval in which heap allocations must fail. */
#pragma once

/** Instruments runtime scenarios without involving the unit-test framework. */
namespace sun::tests::exceptions {
/** Rejects and counts allocations until finish or destruction; only one audit
 * may be active. */
class AllocationAudit {
 public:
  /** Starts an audit after fixture loading and worker startup have completed.
   */
  explicit AllocationAudit(const char* scenario) noexcept;
  /** Restores allocation even when a scenario returns early. */
  ~AllocationAudit();
  /** Stops denial, reports the first allocation attempt, and returns whether
   * none occurred. */
  bool finish() noexcept;
  /** Each audit exclusively controls the process-wide allocator hooks. */
  AllocationAudit(const AllocationAudit&) = delete;
  /** Audit ownership cannot be reassigned. */
  AllocationAudit& operator=(const AllocationAudit&) = delete;

 private:
  const char* scenario_;
};
}  // namespace sun::tests::exceptions
