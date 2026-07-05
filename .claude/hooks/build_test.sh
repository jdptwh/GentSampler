#!/usr/bin/env bash
# build_test.sh — PRIMARY gate command for GentSampler (called from agent.config
# VERIFY_CMD; gate.sh v4 itself is stock and must not be edited).
# Steps: clear stray pluginval processes → CMake configure if missing → Release
# build → ctest. Any failure exits non-zero and gate.sh bounces it to the agent.
set -uo pipefail

# A finished/killed pluginval can linger and hold the .vst3 open, failing the
# next link with LNK1104 — clear any strays before building.
taskkill //F //IM pluginval.exe >/dev/null 2>&1 || true

# Configure once if the build tree is missing (first run on a clean checkout).
# NOTE: build/ is a junction to D:\GentSamplerBuild — paths unchanged.
if [ ! -f build/CMakeCache.txt ]; then
  echo "[gate:primary] no build/CMakeCache.txt — running CMake configure first" >&2
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64 >&2 || {
    echo "[gate:primary] CMake configure failed." >&2
    exit 1
  }
fi

cmake --build build --config Release --parallel || exit 1
ctest --test-dir build -C Release --output-on-failure --no-tests=error || exit 1
