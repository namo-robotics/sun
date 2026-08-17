import json
import re
import sys

# Pre-approve test runs so quoted --gtest_filter args don't trigger permission prompts.
cmd = json.load(sys.stdin).get("tool_input", {}).get("command", "")
is_test = re.match(r'^(SUN_PATH=\S+\s+)?(\./build/tests/sun_tests|ctest)(\s|$)', cmd)
if is_test and not re.search(r'[;&|`$<>]', cmd):
    print(json.dumps({"hookSpecificOutput": {
        "hookEventName": "PreToolUse",
        "permissionDecision": "allow",
        "permissionDecisionReason": "test run",
    }}))
sys.exit(0)
