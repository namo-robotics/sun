// stage_timer.h — Optional per-stage timings for the compile pipeline
//
// Set SUN_TIMINGS=1 to print how long each stage took to stderr. When the
// variable is unset the timers still run but nothing is recorded or printed.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace sun {

class StageTimings {
 public:
  static bool enabled() {
    static const bool on = std::getenv("SUN_TIMINGS") != nullptr;
    return on;
  }

  static void add(std::string name, double milliseconds) {
    entries().emplace_back(std::move(name), milliseconds);
  }

  // One line per stage, then a total. Stages that ran more than once (the
  // entry file is parsed twice, once to find its manifest) are summed.
  static void report() {
    if (!enabled() || entries().empty()) return;

    std::vector<std::pair<std::string, double>> totals;
    double total = 0.0;
    for (const auto& [name, ms] : entries()) {
      total += ms;
      auto it = std::find_if(totals.begin(), totals.end(),
                             [&](const auto& e) { return e.first == name; });
      if (it == totals.end()) {
        totals.emplace_back(name, ms);
      } else {
        it->second += ms;
      }
    }

    std::fprintf(stderr, "--- sun stage timings ---\n");
    for (const auto& [name, ms] : totals) {
      std::fprintf(stderr, "  %-16s %8.2f ms  %5.1f%%\n", name.c_str(), ms,
                   100.0 * ms / total);
    }
    std::fprintf(stderr, "  %-16s %8.2f ms\n", "total", total);
    entries().clear();
  }

 private:
  // Deliberately never destroyed: the report runs from a static destructor,
  // and a destroyed vector would be read there.
  static std::vector<std::pair<std::string, double>>& entries() {
    static auto* e = new std::vector<std::pair<std::string, double>>();
    return *e;
  }
};

namespace detail {
// Prints the report when the process ends, whichever exit it takes
struct StageTimingsReporter {
  ~StageTimingsReporter() { StageTimings::report(); }
};
inline const StageTimingsReporter stageTimingsReporter;
}  // namespace detail

// Times the enclosing scope and records it under `name`
class ScopedStage {
 public:
  explicit ScopedStage(std::string name)
      : name_(std::move(name)), start_(std::chrono::steady_clock::now()) {}

  ~ScopedStage() {
    if (!StageTimings::enabled()) return;
    auto elapsed = std::chrono::steady_clock::now() - start_;
    StageTimings::add(
        name_, std::chrono::duration<double, std::milli>(elapsed).count());
  }

  ScopedStage(const ScopedStage&) = delete;
  ScopedStage& operator=(const ScopedStage&) = delete;

 private:
  std::string name_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace sun
