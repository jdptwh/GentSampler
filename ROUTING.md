# ROUTING.md — Model Routing Doctrine
# Drop this in the repo root. CLAUDE.md references it. The lead agent follows it.

## The Ladder

| Tier | Model | Role | When |
|------|-------|------|------|
| GATE | Fable 5 (`claude-fable-5`) | Planner / Reviewer / Arbiter | Plans, decision gates, 2nd failures |
| WORK | Sonnet 4.6 (`sonnet`) | Implementer | Everything requiring judgment |
| BULK | Haiku 4.5 (`haiku`) | Grunt | Only machine-verifiable output |

## Rule 1 — Route by VERIFIABILITY, not difficulty

Ask one question before assigning a task: **"If this is wrong, what catches it?"**

- **A machine catches it instantly** (compiler, test suite, linter, schema validator,
  template diff) → BULK tier is allowed.
- **A model or human must read it carefully to catch it** (logic, integration,
  architecture, story structure, anything judgment-shaped) → WORK tier minimum.
- **Nothing downstream catches it and the blast radius is large** → GATE tier.

Cheap models on hard-to-verify work is the failure mode. Never do it.

## Rule 2 — Fable touches a task in exactly three cases

1. **Plan mode.** Producing the spec before any implementation starts.
2. **Gates.** Architecture review, pre-merge review of high-risk changes,
   story-structure passes, engineering failure analysis.
3. **Second failure.** Sonnet has failed the same task twice → escalate with
   both failed attempts attached as context.

Fable never types boilerplate, never formats, never summarizes, never writes
code its own plan already fully specified.

## Rule 3 — Spec quality is the first-pass lever

Every Fable plan must be an executable spec, not a vibe. Minimum contents:

- **File-level plan**: which files change, created, deleted.
- **Acceptance criteria**: concrete, checkable statements ("test X passes",
  "endpoint returns Y shape", "scene doc contains beats A/B/C").
- **Out of scope**: what NOT to touch.
- **Verification command**: the exact command that proves the work is done.

The implementer executes the spec. It does not reinterpret it. If the spec is
ambiguous, the implementer stops and asks — it does not guess.

## Rule 4 — Escalation and de-escalation

- BULK fails once → retry once with the error attached. Fails again → WORK tier.
- WORK fails twice on the same task → GATE tier with full failure context.
- Never re-run the same tier with the same prompt expecting a different result.
- After a GATE fix, the corrected pattern gets written back into CLAUDE.md so
  the failure class doesn't recur.

## Rule 5 — Context hygiene (the silent budget killer)

- `/clear` between unrelated tasks. Stale history is resent every turn.
- One task per subagent dispatch. No "and also..." riders.
- CLAUDE.md carries durable state; the conversation carries only the live task.

## Gate Checklist (what the reviewer verifies before PASS)

1. Verification command from the spec runs green.
2. Every acceptance criterion is explicitly checked off.
3. No changes outside the spec's declared file list.
4. No new dependencies unless the spec authorized them.
5. Diff is readable — if the reviewer can't follow it, it fails on that alone.

Verdict format: `PASS` or `FAIL` + findings as `file:line — issue — one-line fix`.
Reviewer never fixes anything itself. The lead decides what to apply.

## Rule 6 — UI work gets a third gate

Unit tests cannot see a broken interface. Any task with UI acceptance criteria
must pass three stacked gates before PASS:

1. **Tests** — logic (VERIFY_CMD)
2. **Lint** — hygiene (LINT_CMD)
3. **Playwright smoke** — the built interface actually works: core flow
   click-through, element presence, zero console errors (UI_VERIFY_CMD)

The smoke spec lives in the repo (e.g. `e2e/smoke.spec.ts`) and grows one test
per UI acceptance criterion. If a UI task has no smoke coverage, that is a
spec defect — the planner adds the test as a work item.

Note for GentSampler: the UI is a JUCE plugin editor, not a web page —
Playwright does not apply. The equivalent third gate here is pluginval
(GATE 2 in gate.sh), which opens the editor and exercises the plugin
lifecycle in a real host harness.

## Rule 7 — Autonomy contract

The loop runs hands-off between plan approval and reviewer verdict:

- Human touchpoints are exactly two: approve the SPEC, accept the PASS.
- Permissions allowlist (settings.json) pre-approves test/build/lint/git
  commands so agents never stall waiting for a human click. Deny list blocks
  push, destructive deletes, network fetches, and secrets reads — the loop
  can iterate freely but cannot leave the repo or wreck it.
- Auto-accept edits during execution (shift+tab). Review happens at the
  reviewer gate, not per-keystroke.
- Design intent enters the loop as a Claude Design handoff bundle referenced
  in the spec — never as prose descriptions of what the UI should look like.
