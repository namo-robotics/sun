// tests/stdlib/sys/test_time.cpp - sun.time
//
// Timing assertions are lower bounds only. A loaded CI machine can stall for
// an arbitrarily long time, so an upper bound would be a flake generator.

#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>
#include <string>

#include "driver/execution_utils.h"

TEST(Stdlib_Sys_Time, duration_conversions) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.time;

    function main() i32 {
        var d = create_duration_millis(1500);
        if (d.as_secs() != 1) { return 1; }
        if (d.as_millis() != 1500) { return 2; }
        if (d.as_micros() != 1500000) { return 3; }
        if (d.as_nanos() != 1500000000) { return 4; }
        if (d.subsec_nanos() != 500000000) { return 5; }
        if (d.as_secs_f64() < 1.49 or d.as_secs_f64() > 1.51) { return 6; }

        if (create_duration_seconds(2).as_millis() != 2000) { return 7; }
        if (create_duration_micros(2500).as_nanos() != 2500000) { return 8; }
        if (create_duration_nanos(1).as_nanos() != 1) { return 9; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Sys_Time, monotonic_clock_does_not_go_backwards) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.time;

    function main() i32 {
        var first = now();
        var second = now();
        // since() clamps at zero, so a backwards clock would show as zero
        // while a forward one is >= 0 either way; check ordering directly.
        if (second.since(first).as_nanos() < 0) { return 1; }
        if (first.since(second).as_nanos() != 0) { return 2; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Sys_Time, sleep_waits_at_least_the_requested_time) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.time;

    function main() i32 {
        var start = now();
        sleep(create_duration_millis(50));
        // Lower bound only, with slack for clock granularity.
        if (start.elapsed().as_millis() < 40) { return 1; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Sys_Time, unix_time_agrees_with_the_host_clock) {
  auto before = static_cast<int64_t>(std::time(nullptr));
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.time;

    function main() i64 {
        return read_unix_time();
    }
  )");
  auto after = static_cast<int64_t>(std::time(nullptr));
  EXPECT_GE(sun::toDouble(value), static_cast<double>(before - 5));
  EXPECT_LE(sun::toDouble(value), static_cast<double>(after + 5));
}

TEST(Stdlib_Sys_Time, unix_time_millis_is_consistent_with_seconds) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.time;

    function main() i32 {
        var secs = read_unix_time();
        var ms = read_unix_time_millis();
        var derived = ms / 1000;
        if (derived < secs - 2) { return 1; }
        if (derived > secs + 2) { return 2; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Sys_Time, utc_breaks_a_fixed_timestamp_into_calendar_fields) {
  // 1700000000 = 2023-11-14 22:13:20 UTC, a Tuesday.
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.time;

    function main() i32 {
        var d = convert_to_utc(1700000000);
        if (d.year != 2023) { return 1; }
        if (d.month != 11) { return 2; }   // 1-based, not tm_mon
        if (d.day != 14) { return 3; }
        if (d.hour != 22) { return 4; }
        if (d.minute != 13) { return 5; }
        if (d.second != 20) { return 6; }
        if (d.weekday != 2) { return 7; }  // Tuesday, Sunday = 0
        if (d.utc_offset != 0) { return 8; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Sys_Time, to_unix_utc_round_trips) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.time;

    function main() i32 {
        var stamp: i64 = 1700000000;
        if (to_unix_utc(convert_to_utc(stamp)) != stamp) { return 1; }
        // And an epoch date built by hand
        var d = DateTime(1970, 1, 1, 0, 0, 0);
        if (to_unix_utc(d) != 0) { return 2; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Sys_Time, format_renders_a_fixed_timestamp) {
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.time;

    function main() i32 {
        var a = make_heap_allocator();
        var d = convert_to_utc(1700000000);
        var text = format(a, d, "%Y-%m-%d %H:%M:%S");
        if (not text.equals_literal("2023-11-14 22:13:20")) { return 1; }
        var day = format(a, d, "%Y-%m-%d");
        if (not day.equals_literal("2023-11-14")) { return 2; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}

TEST(Stdlib_Sys_Time, local_uses_the_timezone) {
  // Pinning TZ makes this deterministic wherever it runs.
  setenv("TZ", "UTC", 1);
  tzset();
  auto value = executeStringWithStdlib(R"(
    using sun;
    using sun.time;

    function main() i32 {
        var d = convert_to_local(1700000000);
        if (d.year != 2023) { return 1; }
        if (d.hour != 22) { return 2; }
        if (to_unix_local(d) != 1700000000) { return 3; }
        return 0;
    }
  )");
  EXPECT_EQ(value, 0);
}
