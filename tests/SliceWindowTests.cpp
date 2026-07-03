// SliceWindowTests.cpp — T2: slice-window math (gent::resolveCueEndEdit /
// applyCueEdit / effectiveCueEnd / resolveEndDragTarget). See
// TEST_TARGET_TASK.md T2 for the extraction plan and acceptance criteria.
#include "doctest.h"
#include "EngineMath.h"

#include <cstdint>

// ---------------------------------------------------------------------------
// T2.1 — resolveCueEndEdit
// ---------------------------------------------------------------------------
TEST_CASE ("T2.1 resolveCueEndEdit: collapse / minimum window / reset-to-auto")
{
    CHECK (gent::resolveCueEndEdit (532, 500) == gent::kOpenSlice);   // cue+32 collapses
    CHECK (gent::resolveCueEndEdit (533, 500) == 533);                // cue+33 is the minimum real window
    CHECK (gent::resolveCueEndEdit (-1, 500) == -1);                  // negative -> auto reset
}

// ---------------------------------------------------------------------------
// T2.2 — applyCueEdit
// ---------------------------------------------------------------------------
TEST_CASE ("T2.2 applyCueEdit: end auto-clear / boundary / clamp to sample start")
{
    {
        const auto r = gent::applyCueEdit (980, 1000);   // start pushed within 32 of end
        CHECK (r.end == -1);
    }
    {
        const auto r = gent::applyCueEdit (967, 1000);   // boundary: 1000 > 967+32 -> end survives
        CHECK (r.end == 1000);
    }
    {
        const auto r = gent::applyCueEdit (-50, 12345);
        CHECK (r.cue == 0);                              // clamp to sample start
    }
}

// ---------------------------------------------------------------------------
// T2.3 — effectiveCueEnd
// ---------------------------------------------------------------------------
TEST_CASE ("T2.3 effectiveCueEnd: OPEN / clamp / slice-mode scan / sentinels")
{
    const int len = 10000;

    // OPEN -> len-1
    CHECK (gent::effectiveCueEnd (500, gent::kOpenSlice, len, false, {}) == len - 1);

    // explicit end beyond sample -> clamped to len-1
    CHECK (gent::effectiveCueEnd (500, len + 5000, len, false, {}) == len - 1);

    // auto (end <= cue) + sliceMode -> smallest cue strictly greater than this pad's cue
    {
        std::array<int, 16> cues {};
        cues.fill (-1);
        cues[0] = 500;    // this pad's own cue (also present in allCues, must not self-match)
        cues[3] = 800;     // nearest next cue
        cues[7] = 900;
        CHECK (gent::effectiveCueEnd (500, -1, len, true, cues) == 800);
    }

    // auto + sliceMode + no later cue -> len-1
    {
        std::array<int, 16> cues {};
        cues.fill (-1);
        cues[0] = 500;
        CHECK (gent::effectiveCueEnd (500, -1, len, true, cues) == len - 1);
    }

    // cue == -1 -> -1 (unassigned)
    CHECK (gent::effectiveCueEnd (-1, -1, len, false, {}) == -1);

    // len == 0 -> 0
    CHECK (gent::effectiveCueEnd (0, -1, 0, false, {}) == 0);
}

// ---------------------------------------------------------------------------
// T2.4 — resolveEndDragTarget
// ---------------------------------------------------------------------------
TEST_CASE ("T2.4 resolveEndDragTarget: tolerance collapse / pass-through / min-window floor")
{
    CHECK (gent::resolveEndDragTarget (1000, 1150, 200) == 1000);   // within tolerance -> collapse
    CHECK (gent::resolveEndDragTarget (1000, 1201, 200) == 1201);
    CHECK (gent::resolveEndDragTarget (1000, 1010, 0) == 1033);     // min-window floor (cue+33)
}

// ---------------------------------------------------------------------------
// T2.5 — invariant property test: 1000 fixed-seed pseudorandom sequences of
// applyCueEdit / resolveCueEndEdit / resolveEndDragTarget over a fake 16-pad
// state must never produce a stored state where end can pass cue (end >= 0
// && end <= cue+32 is the collapse zone — if it survives storage un-collapsed,
// that's the bug this property test exists to catch).
// ---------------------------------------------------------------------------
namespace
{
// tiny fixed-seed xorshift so the sequence is reproducible without <random>
struct XorShift32
{
    std::uint32_t s;
    explicit XorShift32 (std::uint32_t seed) : s (seed ? seed : 0x9e3779b9u) {}
    std::uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int nextInRange (int lo, int hi)   // inclusive
    {
        const std::uint32_t span = (std::uint32_t) (hi - lo + 1);
        return lo + (int) (next() % span);
    }
};
}

TEST_CASE ("T2.5 property: 1000 fixed-seed sequences never leave end <= cue+32 stored, "
           "and effectiveCueEnd - cue is sentinel or >= 1")
{
    XorShift32 rng (0xC0FFEEu);
    const int len = 100000;

    for (int iter = 0; iter < 1000; ++iter)
    {
        std::array<int, 16> cue {}, end {};
        cue.fill (-1);
        end.fill (-1);

        // seed a handful of pads with initial cues so effectiveCueEnd's slice-mode
        // scan has real neighbours to find
        const int nSeeded = rng.nextInRange (1, 16);
        for (int i = 0; i < nSeeded; ++i)
            cue[(size_t) i] = rng.nextInRange (0, len - 100);

        // apply a short random sequence of edits to a random pad
        const int nOps = rng.nextInRange (5, 20);
        for (int op = 0; op < nOps; ++op)
        {
            const int pad = rng.nextInRange (0, 15);
            const int action = rng.nextInRange (0, 2);

            if (action == 0)   // applyCueEdit (move start)
            {
                const int samplePos = rng.nextInRange (-50, len);
                const auto r = gent::applyCueEdit (samplePos, end[(size_t) pad]);
                cue[(size_t) pad] = r.cue;
                end[(size_t) pad] = r.end;
            }
            else if (action == 1)   // resolveCueEndEdit (move end)
            {
                const int samplePos = rng.nextInRange (-5, len + 5000);
                end[(size_t) pad] = gent::resolveCueEndEdit (samplePos, cue[(size_t) pad]);
            }
            else   // resolveEndDragTarget -> feeds into resolveCueEndEdit, as the
                   // production drag path does (applyEndHandleDrag calls setCueEnd)
            {
                const int proposed = rng.nextInRange (-50, len + 5000);
                const int tol = rng.nextInRange (0, 300);
                const int dragTarget = gent::resolveEndDragTarget (cue[(size_t) pad], proposed, tol);
                end[(size_t) pad] = gent::resolveCueEndEdit (dragTarget, cue[(size_t) pad]);
            }

            // INVARIANT: a REAL stored window (end >= 0, not the OPEN sentinel)
            // can never have end <= cue+32 (cue can never pass end)
            const int e = end[(size_t) pad];
            const int c = cue[(size_t) pad];
            if (e >= 0)
                CHECK (e > c + 32);

            // effectiveCueEnd - cue is either the sentinel path (cue<0 -> -1) or
            // a real window of at least 1 sample
            const int eff = gent::effectiveCueEnd (c, e, len, false, cue);
            if (c < 0)
                CHECK (eff == -1);
            else
                CHECK (eff - c >= 1);

            // A real explicit window (assigned cue, e > c, within sample bounds)
            // must pass through effectiveCueEnd unchanged — the displayed LEN
            // (eff - c) equals the stored window (e - c). This is the
            // len == end - cue invariant asserted for real, not by construction.
            if (c >= 0 && e > c && e <= len - 1)
                CHECK (gent::effectiveCueEnd (c, e, len, false, cue) - c == e - c);
        }
    }
}
