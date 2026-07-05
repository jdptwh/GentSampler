#!/usr/bin/env bash
# gate.sh v4 — deterministic verification gate (loop 3). Runs on agent completion
# via Claude Code hooks (settings.json). A non-zero exit blocks the stop and
# bounces the failure back to the agent — no human or top-tier tokens spent on
# machine-catchable failures. This is NOT a model; it is the floor under every tier.
#
# v4: commands now live in .claude/agent.config (single source of truth).
# Precedence: env var > agent.config > default (empty = slot skipped).
#   GATE 1  primary   — main correctness check (code: tests/build · docs:
#                       structure validator · data: schema check)
#   GATE 2  secondary — second check (lint / pluginval / integration)
#   GATE 3  surface   — UI smoke (Playwright) or another end-to-end check

set -uo pipefail

[ -f CLAUDE.md ] || exit 0   # only gate repos that opted in

# ---- Layered config: env > agent.config > default ---------------------------
_env_verify="${CLAUDE_VERIFY_CMD-}"; _env_lint="${CLAUDE_LINT_CMD-}"; _env_ui="${CLAUDE_UI_VERIFY_CMD-}"
[ -f .claude/agent.config ] && . .claude/agent.config
PRIMARY_CMD="${_env_verify:-${VERIFY_CMD:-}}"
SECONDARY_CMD="${_env_lint:-${LINT_CMD:-}}"
SURFACE_CMD="${_env_ui:-${UI_VERIFY_CMD:-}}"
# ------------------------------------------------------------------------------

run_gate () {
  local label="$1" cmd="$2"
  [ -z "$cmd" ] && return 0
  echo "[gate:$label] running: $cmd" >&2
  if bash -c "$cmd" >&2; then
    echo "[gate:$label] PASS" >&2
    return 0
  else
    echo "[gate:$label] FAIL — fix before completing. Do not mark this task done." >&2
    echo "[gate:$label] Resume protocol (ROUTING.md Rule 9): inspect git state before retrying — never replay the prompt blind." >&2
    exit 2   # exit 2 = block the stop, feed stderr back to the agent
  fi
}

run_gate "primary"   "$PRIMARY_CMD"
run_gate "secondary" "$SECONDARY_CMD"
run_gate "surface"   "$SURFACE_CMD"

# A repo with no verification surface runs on reviewer judgment alone, which
# ROUTING.md Rule 2 calls a defect.
if [ -z "$PRIMARY_CMD$SECONDARY_CMD$SURFACE_CMD" ]; then
  echo "[gate] WARNING: no verification surface configured — reviewer-only. Build one (see ROUTING.md Rule 2)." >&2
fi

echo "[gate] ALL PASS" >&2
exit 0
