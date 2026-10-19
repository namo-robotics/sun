#pragma once

#include <map>
#include <set>
#include <string>

/** Validates the emitted boundaries retained in bundle contracts. */
namespace sun::abi {
/** Collects canonical boundaries and rejects conflicting named requirements. */
class BoundaryRequirements {
 public:
  /** Adds a record, rejecting malformed encoding or conflicting named boundaries. */
  void add(const std::string& record);
  /** Returns deduplicated records in canonical order. */
  const std::set<std::string>& records() const { return records_; }

 private:
  std::map<std::string, std::string> named_;
  std::set<std::string> records_;
};
}  // namespace sun::abi
