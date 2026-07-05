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
set -uo pipefail

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
