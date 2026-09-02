#!/usr/bin/env bash
# Runs the built example and asserts expected output. Exit 0 = pass.
set -euo pipefail
DIR=$(dirname "$0")
cd "$DIR"

# The production binary runs main and knows nothing of the tests.
out=$(./main)
echo "$out"
echo "$out" | grep -q "midpoint(2, 10) = 6"

# The test binary runs every test (in parallel by default) and exits 0
# when all pass. Results print in declaration order either way.
tests=$(./main_test)
echo "$tests"
echo "$tests" | grep -q "PASS distance.halves"
echo "$tests" | grep -q "PASS distance.findsTheMidpoint"
echo "$tests" | grep -q "PASS startsWithOneEntry"
echo "$tests" | grep -q "PASS distance.midpointOfEqualPointsIsThatPoint"
echo "$tests" | grep -q "4 passed, 0 failed"

# --test-sequential runs them one after another instead.
./main_test --test-sequential | grep -q "4 passed, 0 failed"

# --test-filter picks which tests run: exact name, module prefix, or a
# trailing star.
./main_test --test-filter distance.halves | grep -q "1 passed, 0 failed"
./main_test --test-filter "distance.*" | grep -q "3 passed, 0 failed"
./main_test --test-filter no.such.test | grep -q "0 passed, 0 failed"
