#!/usr/bin/env bash
# pluginval_gate.sh — SECONDARY gate command for GentSampler (called from
# agent.config LINT_CMD; gate.sh v4 itself is stock and must not be edited).
# Self-detecting: runs only when pluginval is on PATH, otherwise skips with a
# notice. Install: winget install pluginval, or drop pluginval.exe on PATH.
#
# NO RETRY (2026-07-05, v4 migration): the old retry-once existed for a
# "transient pluginval flake" that the 2026-07-04 landmine proved was the
# cold-open CUDA-preload hang (since fixed). A pluginval failure is REAL —
# especially an "Open plugin (cold)" timeout, which means blocking work is on
# the construct path. Diagnose it; do not re-add a retry. See CLAUDE.md.
#
# MACOS_PORT_SPEC.md Phase 2 (Rule 10): OS-branch on `uname` INSIDE this
# script; agent.config stays untouched. Windows path below is UNCHANGED
# byte-for-byte. The mac branch keeps the SAME auto-skip semantics LOCALLY —
# CI is the layer that asserts pluginval is present before invoking this hook
# (criterion 7); this script never enforces that itself on any platform.
set -uo pipefail

if [[ "$(uname)" == "Darwin" ]]; then
  # Locate a mac pluginval binary: env-var override first (this is the var the
  # macos-build.yml CI workflow exports after downloading the official release),
  # then a conventional local-install location, then PATH.
  PLUGIN_ARTIFACT="build/GentSampler_artefacts/Release/VST3/GentSampler.vst3"

  if [ -n "${PLUGINVAL_BIN:-}" ] && [ -x "${PLUGINVAL_BIN:-}" ]; then
    PLUGINVAL="${PLUGINVAL_BIN}"
  elif [ -x "/Applications/pluginval.app/Contents/MacOS/pluginval" ]; then
    PLUGINVAL="/Applications/pluginval.app/Contents/MacOS/pluginval"
  elif command -v pluginval >/dev/null 2>&1; then
    PLUGINVAL="$(command -v pluginval)"
  else
    echo "[gate:secondary] pluginval not found (PLUGINVAL_BIN unset, not at the" >&2
    echo "  conventional /Applications/pluginval.app location, not on PATH) — SKIPPED" >&2
    echo "  (install it or set PLUGINVAL_BIN to enable this gate)" >&2
    exit 0
  fi

  # Same 10-min stall guard as Windows (pluginval has stalled indefinitely once)
  # — but stock macOS has NO `timeout` (GNU coreutils; CI run #3 failed exit 127
  # on this) and /bin/bash is 3.2 (no safe empty-array expansion under set -u),
  # so: plain explicit branches. Unguarded fallback is acceptable — the CI
  # job-level timeout is the backstop there; a local mac dev can install
  # coreutils (gtimeout) for the guard.
  if command -v timeout >/dev/null 2>&1; then
    timeout 600 "$PLUGINVAL" --strictness-level 5 --skip-gui-tests --validate "$PLUGIN_ARTIFACT"
  elif command -v gtimeout >/dev/null 2>&1; then
    gtimeout 600 "$PLUGINVAL" --strictness-level 5 --skip-gui-tests --validate "$PLUGIN_ARTIFACT"
  else
    "$PLUGINVAL" --strictness-level 5 --skip-gui-tests --validate "$PLUGIN_ARTIFACT"
  fi
  exit $?
else
  # Installed 2026-07-02 to %LOCALAPPDATA%\Programs\pluginval — appended here so
  # hook shells that predate the user-PATH registry change still find it.
  PATH="$PATH:${LOCALAPPDATA:-}/Programs/pluginval"
  PLUGIN_ARTIFACT="build/GentSampler_artefacts/Release/VST3/GentSampler.vst3"

  if ! command -v pluginval >/dev/null 2>&1; then
    echo "[gate:secondary] pluginval not on PATH — SKIPPED (install it to enable this gate)" >&2
    exit 0
  fi

  # timeout: pluginval has stalled indefinitely once (static memory, ~0 CPU);
  # 10 min is ~3x a healthy strictness-5 run on this plugin.
  timeout 600 pluginval --strictness-level 5 --skip-gui-tests --validate "$PLUGIN_ARTIFACT"
  rc=$?
  # Don't leave a lingering pluginval holding the .vst3 open for the next link.
  taskkill //F //IM pluginval.exe >/dev/null 2>&1 || true
  exit $rc
fi
