"""PreToolUse hook: auto-approve Bash commands for this workspace.

Permission prefix rules cannot cover compound commands (`cd x && ...`),
`$?` expansions, heredocs, or quoted `;` reliably, so instead of allow-listing
shapes we deny-list what is actually risky and let everything else run.
Anything matching a risky pattern falls through to the normal prompt.
"""
import json
import re
import sys

cmd = json.load(sys.stdin).get("tool_input", {}).get("command", "")

RISKY = [
    # git: only diff/status/log/show style reads are fine (project rule)
    r"\bgit\s+(?!(diff|status|log|show|rev-parse|describe|blame|branch\s*$|remote\s+-v)\b)",
    r"\bsudo\b",
    r"\bsu\b\s",
    # destructive file ops outside scratch/build dirs
    r"\brm\s+(-\S+\s+)*(?!(\./)?(tmp|build|dist)/)\S*(/|~|\.\.)",
    r"\brm\s+(-\S+\s+)*(?!(\./)?(tmp|build|dist)/)\S*\*",
    r"\bmv\s+(?!.*\btmp/)",
    r"\bchmod\s+(-R|777)",
    r"\bchown\b",
    r"\bmkfs\b|\bdd\s+if=|>\s*/dev/sd|\bshutdown\b|\breboot\b",
    r"\bkill\s+-9\s+-1\b|\bpkill\b|\bkillall\b",
    # remote code execution / package installs
    r"\b(curl|wget)\b[^|]*\|\s*(ba)?sh\b",
    r"\bapt(-get)?\s+install\b|\bpip3?\s+install\b|\bnpm\s+(i|install)\b",
    # writing outside the workspace
    r">\s*/(etc|usr|bin|sbin|lib|boot|home)/",
    r"\bcrontab\b|\bsystemctl\b",
]

if not any(re.search(p, cmd) for p in RISKY):
    print(json.dumps({"hookSpecificOutput": {
        "hookEventName": "PreToolUse",
        "permissionDecision": "allow",
        "permissionDecisionReason": "workspace command (allow_bash.py)",
    }}))
sys.exit(0)
