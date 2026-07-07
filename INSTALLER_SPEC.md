# INSTALLER_SPEC — DRAFT (spec-drafter output, NOT yet planner-owned, NOT Joe-approved)

(Full draft body supplied by the spec-drafter this session; the planner replaces
this file with the owned FINAL. Summary of the draft for review context:)

**Objective** — double-clickable Windows installer for the CUDA-free Release
payload, correct elevation to {commoncf}\VST3, built from existing artefacts
with zero CMake/build.bat changes, proven on Joe's clean machine (closing
PACKAGING P3's deferred ledger rows).

**Tasks:** T1 payload definition + fail-closed CUDA allowlist pre-flight;
T2 installer/GentSampler.iss authoring (Inno recommended; admin privileges;
version 1.1.0; uninstall entry); T3 scripts/make_installer.bat (manual step,
emits dist/GentSamplerSetup-1.1.0.exe, dist/ gitignored); T4
INSTALLER_TEST_CHECKLIST.md + Joe's clean-machine run = the acceptance gate;
T5 "before public release" doc (code-signing + JUCE purchase gates, text only).

**Recommendations:** VST3-only payload (Standalone as optional component);
Inno Setup over NSIS/WiX/zip+bat; unsigned for the private test; models never
touched by the installer (Documents\GentSampler\models is install-independent
— verified in PluginProcessor.cpp).

**Open questions for the planner (7):** A Standalone-in-payload; B tooling
authorization (Inno vs zip+bat); 3 version-string sync (manual comment vs
findstr parse in make_installer.bat); 4 dist/ gitignored; 5 who records T4's
closure of PACKAGING P3's deferred row; 6 UAC-evidence method (Joe visual vs
captured artifact); 7 scripts/ dir vs repo root.

Tiers: T1-T5 IMPLEMENTER (T5 BULK-candidate declined per licensing precedent);
T2 budget 2; T4 = Joe-run, no autonomous loop. Ground rules carried from
PACKAGING incl. the Program Files approval discipline for local installer test
runs.
