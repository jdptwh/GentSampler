#!/usr/bin/env python3
"""verdict_lint.py (v4) — validates the reviewer's structured verdict.

The reviewer writes .claude/state/verdict.json after every review. This script
schema-checks it so the lead agent branches on machine-validated fields instead
of parsing prose. Stdlib only — no dependencies.

Exit codes:
  0  valid verdict            (lead branches on .verdict / .escalate)
  1  file missing             (reviewer skipped the artifact — re-dispatch)
  2  malformed / schema fail  (reviewer must re-emit; details on stderr)

Usage: python3 .claude/hooks/verdict_lint.py [path]   (default .claude/state/verdict.json)
"""
import json, sys

PATH = sys.argv[1] if len(sys.argv) > 1 else ".claude/state/verdict.json"

REQUIRED = {
    "task":       str,   # task/spec name being reviewed
    "verdict":    str,   # "PASS" | "FAIL"
    "findings":   list,  # [{file, line, issue, fix, nit}] — may be empty on PASS
    "escalate":   bool,  # True → planner arbitration required
    "escalate_reason": str,  # "" when escalate is False
    "gates_rerun": bool, # reviewer re-ran verification commands itself (Gate Checklist #1)
    "review_cycle": int, # 1-based; lead compares against MAX_REVIEW_CYCLES
}
FINDING_KEYS = {"file": str, "line": int, "issue": str, "fix": str, "nit": bool}

def fail(msg: str) -> None:
    print(f"[verdict_lint] FAIL: {msg}", file=sys.stderr)
    sys.exit(2)

try:
    with open(PATH, encoding="utf-8") as f:
        v = json.load(f)
except FileNotFoundError:
    print(f"[verdict_lint] MISSING: {PATH} — reviewer did not emit a verdict artifact.", file=sys.stderr)
    sys.exit(1)
except json.JSONDecodeError as e:
    fail(f"invalid JSON: {e}")

if not isinstance(v, dict):
    fail("top level must be an object")

for key, typ in REQUIRED.items():
    if key not in v:
        fail(f"missing required key: {key}")
    if not isinstance(v[key], typ):
        fail(f"key '{key}' must be {typ.__name__}, got {type(v[key]).__name__}")

if v["verdict"] not in ("PASS", "FAIL"):
    fail(f"verdict must be PASS or FAIL, got {v['verdict']!r}")

if v["verdict"] == "FAIL" and not v["findings"]:
    fail("FAIL verdict requires at least one finding")

if v["escalate"] and not v["escalate_reason"].strip():
    fail("escalate=true requires a non-empty escalate_reason")

if not v["gates_rerun"]:
    fail("gates_rerun=false — reviewer must re-run verification itself (Gate Checklist #1)")

if v["review_cycle"] < 1:
    fail("review_cycle must be >= 1")

for i, fnd in enumerate(v["findings"]):
    if not isinstance(fnd, dict):
        fail(f"findings[{i}] must be an object")
    for key, typ in FINDING_KEYS.items():
        if key not in fnd or not isinstance(fnd[key], typ):
            fail(f"findings[{i}].{key} missing or not {typ.__name__}")

nits = sum(1 for f in v["findings"] if f["nit"])
print(f"[verdict_lint] OK — {v['verdict']} · cycle {v['review_cycle']} · "
      f"{len(v['findings'])} finding(s) ({nits} nit) · escalate={v['escalate']}")
sys.exit(0)
