# MACOS_PKG_SPEC.md — Unsigned macOS .pkg Installer (friend-install friction removal)

> STATUS: **FINAL — planner-authored directly 2026-07-07 (Fable; drafter skipped per
> ROUTING Rule 8, WAVE3 precedent: small scope, planner holds all context). Awaiting
> Joe approval. Follow-on to MACOS_PORT_SPEC.md (Phase-2 checkpoints 3+4 GREEN, CI run
> #4 @ 564d42e: build + 124/124 ctest + pluginval 5 + auval PASS + universal2 asserts).**

## SPEC: macOS .pkg INSTALLER (unsigned, private testing)

**Objective** — Replace the friend-install flow (unzip → manual copy into
`~/Library/Audio/Plug-Ins` → Terminal `xattr` quarantine strip) with a single CI-built,
UNSIGNED `GentSampler-<version>.pkg` that installs both bundles to the system plugin
domains via the macOS Installer. Key win: files installed by the macOS Installer do NOT
carry the `com.apple.quarantine` xattr — the checklist's Terminal step DIES. Residual
friction is one Gatekeeper override on the pkg file itself (right-click → Open, or on
macOS 15+ System Settings → Privacy & Security → "Open Anyway"). Signing/notarization
remains the deferred ship gate — unchanged, not touched, not partially implemented.

**File plan**

- `.github/workflows/macos-build.yml` (modify — the only functional change): new
  packaging step AFTER the existing ad-hoc re-sign step (ordering is load-bearing:
  the pkg payload must contain the FINAL re-signed binaries, never pre-sign copies):
  1. Parse `<version>` from CMakeLists `project(... VERSION x.y.z)` — FAIL-CLOSED:
     regex must match `[0-9]+\.[0-9]+\.[0-9]+` or the step exits 1 (precedent:
     `installer/make_installer.bat` `/DAppVer` discipline; never hardcode).
  2. `pkgbuild --analyze` each bundle → edit the component plist to set
     `BundleIsRelocatable = false` → `pkgbuild --component` with `--component-plist`,
     `--identifier com.gentsampler.pkg.au` / `com.gentsampler.pkg.vst3` (STABLE
     forever — receipts/upgrade tracking, same discipline as the Inno AppId),
     `--version <version>`, `--install-location /Library/Audio/Plug-Ins/Components`
     (AU) / `/Library/Audio/Plug-Ins/VST3` (VST3). Relocatability MUST be forced
     false: a relocatable component pkg "updates" a stray copy of the bundle
     wherever Spotlight last saw one instead of installing to the declared path —
     the classic pkgbuild trap, pre-wired here so it never burns a loop.
  3. `productbuild --package au.pkg --package vst3.pkg GentSampler-<version>.pkg`.
  4. MACHINE GATE (see criterion 3): `pkgutil --expand-full` + payload asserts.
  5. Upload the .pkg as a SECOND artifact (`GentSampler-macOS-pkg`) — Joe's relay
     becomes: download one artifact, forward one file. The existing ditto-zip
     artifact is UNCHANGED and remains the fallback path.
  Thin-wrapper discipline holds (Rule 10): no gate content moves out of the hooks;
  the existing build/ctest/pluginval/auval/lipo steps are byte-untouched.
- `MACOS_TEST_CHECKLIST.md` (modify): install section rewritten to the pkg flow
  (criterion 5); the manual-copy + xattr flow moves to a FALLBACK APPENDIX — kept
  verbatim as the recovery path if the pkg misbehaves. New pre-install row: delete
  any `~/Library/Audio/Plug-Ins/{Components,VST3}/GentSampler.*` left from a prior
  zip-flow session — the pkg installs to `/Library` (system domain) and a stale
  user-domain copy would shadow/duplicate in Logic's scan. Appendix one-liner for
  uninstall: delete both bundles + `sudo pkgutil --forget <identifier>`.
- `MACOS_PKG_SPEC.md` (this file) — committed with the change.
- NOTHING else. No `Source/`, no `CMakeLists.txt`, no hooks, no `installer/`
  (Windows), no `.claude/` changes.

**Acceptance criteria** (each individually checkable)

1. Version injection is fail-closed: the workflow step exits non-zero if the
   CMakeLists version regex does not match — provable by reading the step; the pkg
   filename and both `pkgbuild --version` values carry the parsed version.
2. Both component pkgs are built with `BundleIsRelocatable = false` (checkable in
   the committed workflow: the `--component-plist` edit is present for both).
3. CI machine gate for the pkg, in the same workflow run: `pkgutil --expand-full
   GentSampler-<version>.pkg` then assert (a) `pkgutil --payload-files` / the
   expanded payload lists `GentSampler.component` and `GentSampler.vst3` at exactly
   the declared install locations; (b) `codesign -dv` succeeds on BOTH extracted
   plugin binaries (proves the payload holds the ad-hoc re-signed finals, not
   pre-sign copies — the step ordering made observable); (c) `lipo -archs` on both
   extracted binaries reports `x86_64 arm64`. Any assert fails → the CI run fails.
4. The run uploads the second artifact `GentSampler-macOS-pkg`; the existing
   `GentSampler-macOS-universal2` artifact is unchanged (same name, same contents
   recipe).
5. Checklist rewrite per file plan: pkg flow primary (download → right-click →
   Open — with the macOS 15+ "Open Anyway" alternative spelled out → Installer →
   admin password prompt, expected: system-domain install → restart Logic /
   Plug-in Manager rescan); stale user-domain cleanup row present; manual+xattr
   flow preserved in the fallback appendix; `/Library` vs `~/Library` domain
   change noted with "Logic scans both — which is why the cleanup row exists".
6. All existing CI gates still green in the same run (build, 124/124 ctest,
   pluginval 5, auval, lipo asserts, dylib-absent degradation) — the pkg step is
   additive, never a substitute.
7. Windows surface untouched: `bash .claude/hooks/build_test.sh` +
   `bash .claude/hooks/pluginval_gate.sh` green on the Windows box (no Source/
   CMake change, so this is a regression tripwire, not a formality — DoD requires
   it regardless).

**Verification commands**

- Windows (unchanged): `bash .claude/hooks/build_test.sh` +
  `bash .claude/hooks/pluginval_gate.sh`.
- CI: the next `workflow_dispatch` run on the pushed commit — all existing gates
  plus the criterion-3 pkg asserts, all in-workflow (no mac hardware locally; the
  runner IS the verification machine).
- Human (deferred to the friend session, whichever flow Joe picks): pkg installs
  via Installer with admin prompt, plugin appears in Logic after rescan, NO xattr
  step needed.

**Out of scope**

- Signing/notarization (BACKLOG "BEFORE PUBLIC RELEASE" ship gate — unchanged).
- DMG, drag-install, or any other packaging format.
- Windows installer (`installer/`) — untouched.
- Any `Source/` or `CMakeLists.txt` change.
- The friend Zoom session itself (MACOS_PORT_SPEC.md checkpoints 5-6 — this spec
  only changes what lands in his hands).
- Uninstaller tooling beyond the appendix one-liner.

**Tier assignment** — IMPLEMENTER, one dispatch (workflow step + checklist rewrite
together; they are one coherent change). Not BULK: productbuild/pkgbuild behavior
(relocatability, payload layout) needs judgment even though the machine gate is
strong. No BULK slices identified.

**Loop budget** — Default (`.claude/agent.config`): MAX_IMPL_ATTEMPTS=3,
MAX_REVIEW_CYCLES=2. Note R-b (CI minutes): each attempt costs a mac CI run —
attempts should be locally lint-checked (yaml + shellcheck-by-eye) before dispatching
a run, and the pkg steps must run AFTER the cheap asserts so a pkg-step failure
still yields the full diagnostic from one run.

**Checkpoints**

1. Single checkpoint: workflow step + checklist rewrite + this spec committed;
   Windows hooks green; next CI run GREEN end-to-end including criterion 3 —
   commit + CLAUDE.md "Current state" update. (One commit; resume point per
   Rule 9 is the pre-change CI-green state at 564d42e.)

**Risks**

- R-a — Unsigned-pkg Gatekeeper UX: double-click is refused; right-click → Open is
  the documented override, and macOS 15 (Sequoia) tightened overrides — the
  checklist documents BOTH paths (right-click → Open; System Settings → Privacy &
  Security → "Open Anyway"). If the friend's macOS refuses both, the fallback
  appendix (zip + xattr) is the recovery path — that is why it survives.
- R-b — Each iteration costs a mac CI run (R10 in MACOS_PORT_SPEC.md: ~8-12
  runs/month on the free tier). Mitigation in Loop budget.
- R-c — pkgbuild relocatability trap — pre-wired closed (file plan step 2 /
  criterion 2). Recorded so nobody "simplifies" the component-plist away.
- R-d — System-domain install prompts for an admin password; if the friend's
  account is non-admin the install fails — checklist pre-flight row (admin
  account? yes/no) added with the fallback appendix as the no-admin path.
- R-e — Stale user-domain copy from a prior zip session shadows or duplicates the
  system-domain install in Logic — closed by the mandatory cleanup row
  (criterion 5).
- R-f — productbuild synthesized distribution quirks (title/arch requirements
  metadata). Accepted: no custom distribution XML in scope; if the plain
  `--package` composition misbehaves in the criterion-3 asserts, that is an
  implementer finding, not a license to hand-craft distribution XML without
  planner sign-off.

**OPEN QUESTION FOR JOE (one line)** — Should the friend session WAIT for this pkg
(recommended: it exists within one CI run of approval) or proceed on the zip flow if
the session lands first? The checklist keeps both paths, so either answer works —
your call on scheduling, not on scope.

---

**Provenance:** planner-authored directly 2026-07-07 (Rule 8; context: MACOS_PORT_SPEC.md
A1/A2, CI run #4 @ 564d42e facts lead-verified, installer/make_installer.bat version-
injection precedent, INSTALLER_TEST_CHECKLIST.md checklist precedent). Joe mandate:
simplest professional install WITHOUT Apple Developer purchase; .pkg explicitly
authorized 2026-07-07.
