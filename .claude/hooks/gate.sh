#!/usr/bin/env bash
# gate.sh — stacked automatic verification gates. (GentSampler)
# Wired to Claude Code hooks (see settings.json). Runs on agent completion;
# a non-zero exit blocks the stop and bounces the failure back to the agent.
# No human or Fable tokens spent on machine-catchable failures.
#
# Gates, run in order. Each is skipped if its command is empty.
#   GATE 1  build     — Release build of the plugin        (VERIFY_CMD)
#   GATE 2  pluginval — VST3 validation, auto-skipped if pluginval not on PATH
#   GATE 3  lint      — none configured                    (LINT_CMD)
#   GATE 4  ui        — n/a (JUCE editor, not a web UI)    (UI_VERIFY_CMD)
# Keep these in sync with the "Verification command" section of CLAUDE.md.

set -uo pipefail

# ---- EDIT PER PROJECT (or export as env vars) ------------------------------
VERIFY_CMD="${CLAUDE_VERIFY_CMD:-cmake --build build --config Release --parallel}"
LINT_CMD="${CLAUDE_LINT_CMD:-}"                       # no linter configured for this repo
UI_VERIFY_CMD="${CLAUDE_UI_VERIFY_CMD:-}"             # JUCE editor — no Playwright equivalent

# GATE 2 — pluginval (second gate). Self-detecting: runs only when pluginval
# is on PATH; otherwise skipped with a notice. Install: winget install pluginval
# or drop pluginval.exe on PATH. Validates the freshly built VST3.
# Installed 2026-07-02 to %LOCALAPPDATA%\Programs\pluginval — appended here so
# hook shells that predate the user-PATH registry change still find it.
PATH="$PATH:${LOCALAPPDATA:-}/Programs/pluginval"
PLUGIN_ARTIFACT="build/GentSampler_artefacts/Release/VST3/GentSampler.vst3"
PLUGINVAL_CMD="${CLAUDE_PLUGINVAL_CMD:-}"
if [ -z "$PLUGINVAL_CMD" ] && command -v pluginval >/dev/null 2>&1; then
  PLUGINVAL_CMD="pluginval --strictness-level 5 --skip-gui-tests --validate \"$PLUGIN_ARTIFACT\""
fi
# -----------------------------------------------------------------------------

[ -f CLAUDE.md ] || exit 0   # only gate repos that opted in

# Configure once if the build tree is missing (first run on a clean checkout).
if [ ! -f build/CMakeCache.txt ]; then
  echo "[gate] no build/CMakeCache.txt — running CMake configure first" >&2
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64 >&2 || {
    echo "[gate:configure] FAIL — CMake configure failed." >&2
    exit 2
  }
fi

run_gate () {
  local label="$1" cmd="$2"
  [ -z "$cmd" ] && return 0
  echo "[gate:$label] running: $cmd" >&2
  if bash -c "$cmd" >&2; then
    echo "[gate:$label] PASS" >&2
    return 0
  else
    echo "[gate:$label] FAIL — fix before completing. Do not mark this task done." >&2
    exit 2   # exit 2 = block the stop, feed stderr back to the agent
  fi
}

run_gate "build" "$VERIFY_CMD"

if [ -n "$PLUGINVAL_CMD" ]; then
  run_gate "pluginval" "$PLUGINVAL_CMD"
else
  echo "[gate:pluginval] pluginval not on PATH — SKIPPED (install it to enable the second gate)" >&2
fi

run_gate "lint" "$LINT_CMD"
run_gate "ui"   "$UI_VERIFY_CMD"

echo "[gate] ALL PASS" >&2
exit 0
