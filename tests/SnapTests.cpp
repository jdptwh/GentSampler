// SnapTests.cpp — T3: snap capture math (gent::nearestTransientIn /
// gent::applySnapThreshold / gent::selectSnapCursor). See TEST_TARGET_TASK.md
// T3 for the extraction plan, acceptance criteria, and the flagged
// snapCursor-vs-resolveSnap discrepancy.
//
// DISCREPANCY FLAG (from the spec, encoded not "fixed"): resolveSnap
// (PluginEditor.h, handle-drag snap) has NO grid/transient competition — grid
// is used iff gridStepSamples() > 0, transients only otherwise. The
// nearest-point competition (grid line vs all 16 placed cues vs transients)
// lives in snapCursor (PluginProcessor.cpp, live-cursor placement) instead.
// T3.1/T3.3/T3.5 below test resolveSnap's actual behavior (via
// applySnapThreshold + nearestTransientIn); T3.4 tests snapCursor's actual
// behavior (via selectSnapCursor). Both are real, current, and different.
#include "doctest.h"
#include "EngineMath.h"

// ---------------------------------------------------------------------------
// T3.1 — applySnapThreshold
// ---------------------------------------------------------------------------
TEST_CASE ("T3.1 applySnapThreshold: 6*spp capture threshold")
{
    const int p = 10000;
    CHECK (gent::applySnapThreshold (p, p + 60, 10.0) == p + 60);   // exactly 6*spp engages
    CHECK (gent::applySnapThreshold (p, p + 61, 10.0) == p);        // never beyond
    CHECK (gent::applySnapThreshold (p, p + 1, 0.0) == p);          // zero-spp degenerate
}

// ---------------------------------------------------------------------------
// T3.2 — resolveSnap's early-outs (snap-disabled, Alt-bypass) require a live
// GentSamplerAudioProcessor to construct (they read p.snapEnabled directly),
// which this logic-only test binary deliberately cannot do (ground rule 1:
// no JUCE link, no processor construction). Per TEST_TARGET_TASK.md T3.2,
// this is a comment-only acceptance criterion: those two early-out lines
// (PluginEditor.h:140-141 in the spec's line numbering; the guard now sits
// at the top of resolveSnap, see PluginEditor.h) are covered by REVIEWER
// INSPECTION (T6), not by a unit test here.
// ---------------------------------------------------------------------------
TEST_CASE ("T3.2 resolveSnap early-outs: reviewer-inspection only (see comment above)")
{
    // no-op: documents the coverage boundary; see file header + PluginEditor.h.
    CHECK (true);
}

// ---------------------------------------------------------------------------
// T3.3 — nearestTransientIn
// ---------------------------------------------------------------------------
TEST_CASE ("T3.3 nearestTransientIn: 50ms cap / empty / earlier-element tie-break")
{
    const double sr = 44100.0;   // maxDist = (int)(44100*0.05) = 2205

    CHECK (gent::nearestTransientIn ({ 44100 }, 44100 + 2204, sr) == 44100);   // within cap
    CHECK (gent::nearestTransientIn ({ 44100 }, 44100 + 2206, sr) == 44100 + 2206);   // beyond cap: unchanged
    CHECK (gent::nearestTransientIn ({}, 12345, sr) == 12345);   // empty vector: unchanged

    // equidistant transients: the EARLIER vector element wins (strict '<')
    {
        const int pos = 5000;
        const std::vector<int> ts { pos - 100, pos + 100 };   // both d=100, first wins
        CHECK (gent::nearestTransientIn (ts, pos, sr) == pos - 100);
    }
}

// ---------------------------------------------------------------------------
// T3.4 — selectSnapCursor (snapCursor's actual candidate competition)
// ---------------------------------------------------------------------------
TEST_CASE ("T3.4 selectSnapCursor: grid-always-wins / stay-free rule / nearest-cue-beats-grid")
{
    const double sr = 44100.0;   // 0.05*sr = 2205

    // with grid: nearest of {gridCand, all 16 cues} wins regardless of distance
    // (grid path has no 50ms bail)
    {
        std::array<int, 16> cues {};
        cues.fill (-1);
        const int pos = 10000;
        const int gridCand = pos + 50000;   // far away, but grid path never bails
        CHECK (gent::selectSnapCursor (pos, true, gridCand, cues, -1, sr) == gridCand);
    }

    // without grid: a transient/cue farther than 0.05*sr from pos -> pos unchanged
    {
        std::array<int, 16> cues {};
        cues.fill (-1);
        const int pos = 10000;
        const int transientCand = pos + 3000;   // farther than 2205
        CHECK (gent::selectSnapCursor (pos, false, 0, cues, transientCand, sr) == pos);
    }

    // a placed cue at d=50 beats a grid line at d=100 when there IS a grid
    // (both compete on nearest distance)
    {
        std::array<int, 16> cues {};
        cues.fill (-1);
        const int pos = 10000;
        cues[0] = pos + 50;
        const int gridCand = pos + 100;
        CHECK (gent::selectSnapCursor (pos, true, gridCand, cues, -1, sr) == pos + 50);
    }
}

// ---------------------------------------------------------------------------
// T3.5 — composition: effective transient capture in resolveSnap is
// min(6px threshold, 50ms cap)
// ---------------------------------------------------------------------------
TEST_CASE ("T3.5 composition: applySnapThreshold(nearestTransientIn(...)) == min(6px, 50ms)")
{
    const double sr = 44100.0;
    const int p = 20000;

    // transient at 40ms (well inside the 50ms cap): with a large spp (loose 6px
    // threshold in samples), the composed snap engages
    {
        const int transientAt40ms = p + (int) (0.040 * sr);
        const double spp = 1000.0;   // 6*spp = 6000 samples, comfortably >= 40ms's distance
        const int cand = gent::nearestTransientIn ({ transientAt40ms }, p, sr);
        CHECK (cand == transientAt40ms);   // within the 50ms cap
        CHECK (gent::applySnapThreshold (p, cand, spp) == transientAt40ms);   // and within 6*spp
    }

    // transient at 60ms (beyond the 50ms cap): nearestTransientIn itself never
    // returns it, so the composed snap never engages regardless of spp
    {
        const int transientAt60ms = p + (int) (0.060 * sr);
        const int cand = gent::nearestTransientIn ({ transientAt60ms }, p, sr);
        CHECK (cand == p);   // beyond the 50ms cap: nearestTransientIn returns p unchanged
        for (double spp : { 1.0, 100.0, 100000.0 })
            CHECK (gent::applySnapThreshold (p, cand, spp) == p);   // never snaps
    }
}
