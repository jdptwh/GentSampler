#!/usr/bin/env bash
# build_test.sh — PRIMARY gate command for GentSampler (called from agent.config
# VERIFY_CMD; gate.sh v4 itself is stock and must not be edited).
# Steps: clear stray pluginval processes (Windows only) → CMake configure if
# missing → Release build → ctest. Any failure exits non-zero and gate.sh
# bounces it to the agent.
#
# MACOS_PORT_SPEC.md Phase 2 (Rule 10 — agent.config stays the single source
# of truth for the gate COMMAND; this script owns the OS-branch INSIDE it).
# Windows path below is UNCHANGED byte-for-byte from the pre-port version;
# the mac branch is single-config (Ninja/Makefiles, not Xcode) per the spec's
# verification-commands section, universal2 via CMAKE_OSX_ARCHITECTURES.
set -uo pipefail

if [[ "$(uname)" == "Darwin" ]]; then
  # Configure once if the build tree is missing (first run on a clean checkout).
  if [ ! -f build/CMakeCache.txt ]; then
    echo "[gate:primary] no build/CMakeCache.txt — running CMake configure first" >&2
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" >&2 || {
      echo "[gate:primary] CMake configure failed." >&2
      exit 1
    }
  fi

  cmake --build build --parallel || exit 1
  ctest --test-dir build --output-on-failure --no-tests=error || exit 1
else
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
fi
