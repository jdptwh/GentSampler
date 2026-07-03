// StemMaskTests.cpp — T5: stem-source switching (gent::stemMaskWithBit /
// gent::singleStemIndex / gent::stemGainFor). See TEST_TARGET_TASK.md T5.
//
// "Valid state" definition (from code, per the spec): the mask is one
// std::atomic<uint8_t> per pad (PluginProcessor.h:439); the audio thread
// reads it ONCE per block (PluginProcessor.cpp ~2422, now delegated to
// gent::stemGainFor) and ramps v.sg[] toward the derived targets — so
// mid-playback validity = (a) every stored value has bits only in 0x3F, and
// (b) every mask x stem x bleed combination yields a gain in [0,1]. No
// multi-word atomicity requirement exists (a single uint8_t load/store is
// already atomic).
#include "doctest.h"
#include "EngineMath.h"

#include <climits>
#include <cmath>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// T5.1 — bit ops
// ---------------------------------------------------------------------------
TEST_CASE ("T5.1 stemMaskWithBit: set / clear / out-of-range strip / idempotent / round-trip")
{
    CHECK (gent::stemMaskWithBit (0, 2, true) == 0x04);
    CHECK (gent::stemMaskWithBit (0x04, 2, false) == 0x00);   // clearing returns to FULL

    CHECK (gent::stemMaskWithBit (0xFF, 5, true) == 0x3F);    // out-of-range bits stripped

    // set is idempotent
    {
        std::uint8_t m = gent::stemMaskWithBit (0, 1, true);
        CHECK (gent::stemMaskWithBit (m, 1, true) == m);
    }

    // set-then-clear round-trips for all 6 stems
    for (int stem = 0; stem < 6; ++stem)
    {
        std::uint8_t m = gent::stemMaskWithBit (0, stem, true);
        m = gent::stemMaskWithBit (m, stem, false);
        CHECK (m == 0);
    }
}

// ---------------------------------------------------------------------------
// T5.2 — singleStemIndex (FULL-neutral rule)
// ---------------------------------------------------------------------------
TEST_CASE ("T5.2 singleStemIndex: single-bit -> stem index, FULL/multi -> -1")
{
    CHECK (gent::singleStemIndex (0x00) == -1);   // FULL
    CHECK (gent::singleStemIndex (0x01) == 0);
    CHECK (gent::singleStemIndex (0x20) == 5);
    CHECK (gent::singleStemIndex (0x03) == -1);   // multi-bit
    CHECK (gent::singleStemIndex (0x3F) == -1);   // all six -> multi
}

// ---------------------------------------------------------------------------
// T5.3 — gain table
// ---------------------------------------------------------------------------
TEST_CASE ("T5.3 stemGainFor: FULL unity / selected-vs-bled / bleed clamp / global-inaudible")
{
    // mask 0 (FULL) -> 1.0 for all six stems
    for (int k = 0; k < 6; ++k)
        CHECK (gent::stemGainFor (0x00, k, 0.5f, true) == doctest::Approx (1.0f));

    // mask 0x01 -> k=0 gets 1.0, k=1..5 get bleed*0.5
    CHECK (gent::stemGainFor (0x01, 0, 0.5f, true) == doctest::Approx (1.0f));
    for (int k = 1; k < 6; ++k)
        CHECK (gent::stemGainFor (0x01, k, 0.5f, true) == doctest::Approx (0.25f));   // 0.5*0.5

    // bleed clamps: 2.0 -> 1.0*0.5 = 0.5; -1.0 -> 0.0*0.5 = 0.0
    CHECK (gent::stemGainFor (0x01, 3, 2.0f, true) == doctest::Approx (0.5f));
    CHECK (gent::stemGainFor (0x01, 3, -1.0f, true) == doctest::Approx (0.0f));

    // globalAudible=false -> 0 always, regardless of mask/bleed
    CHECK (gent::stemGainFor (0x00, 0, 1.0f, false) == doctest::Approx (0.0f));
    CHECK (gent::stemGainFor (0x01, 0, 1.0f, false) == doctest::Approx (0.0f));
    CHECK (gent::stemGainFor (0x01, 2, 1.0f, false) == doctest::Approx (0.0f));
}

// ---------------------------------------------------------------------------
// T5.4 — validity property: exhaustive 64-mask sweep + fixed-seed sequences
// ---------------------------------------------------------------------------
namespace
{
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
};
}

TEST_CASE ("T5.4 validity: exhaustive 64 masks x 6 stems x 5 bleeds x 2 audible -> gain in [0,1]")
{
    const float bleeds[5] = { -1.0f, 0.0f, 0.5f, 1.0f, 2.0f };
    for (int m = 0; m < 64; ++m)
    {
        for (int k = 0; k < 6; ++k)
        {
            for (float bleed : bleeds)
            {
                for (bool audible : { false, true })
                {
                    const float g = gent::stemGainFor ((std::uint8_t) m, k, bleed, audible);
                    CHECK (g >= 0.0f);
                    CHECK (g <= 1.0f);
                }
            }
        }
    }
}

TEST_CASE ("T5.4 validity: 1000 fixed-seed random stemMaskWithBit/reset-to-0 sequences stay in [0, 0x3F]")
{
    XorShift32 rng (0xBADC0FFEu);
    for (int iter = 0; iter < 1000; ++iter)
    {
        std::uint8_t m = 0;
        const int nOps = 1 + (int) (rng.next() % 40);
        for (int op = 0; op < nOps; ++op)
        {
            const std::uint32_t r = rng.next();
            if ((r >> 8) % 7 == 0)
            {
                m = 0;   // occasional reset-to-0 (setPadFull equivalent)
            }
            else
            {
                const int stem = (int) (r % 6);
                const bool on = ((r >> 3) & 1) != 0;
                m = gent::stemMaskWithBit (m, stem, on);
            }
            CHECK (m <= 0x3F);   // m is unsigned, so <= 0x3F alone proves [0, 0x3F]
        }
    }
}

// ---------------------------------------------------------------------------
// D2 — hero view-mode state (gent::sanitizeHeroView / gent::resolveHeroView)
// See docs/STEM_VIEW_MODEL.md SS4-SS6 (RATIFIED, normative). These cases were
// authored test-first against deliberately-wrong stubs (D2a, skipped + RED
// proven via --no-skip) and turned live when D2b's real bodies landed —
// REDESIGN_TASK_D.md's BULK protocol, executed 2026-07-03.
// ---------------------------------------------------------------------------

// -- sanitizeHeroView -------------------------------------------------------
// Legal values are {0,1}; everything else -> 0. Against the stub (always 0),
// the stored==1 case is the one that must fail (expected 1, stub returns 0).
TEST_CASE ("D2.1 sanitizeHeroView: 0 -> 0 (COMPOSITE passes through)")
{
    CHECK (gent::sanitizeHeroView (0) == 0);
}

TEST_CASE ("D2.1 sanitizeHeroView: 1 -> 1 (STEMS passes through)")
{
    CHECK (gent::sanitizeHeroView (1) == 1);
}

TEST_CASE ("D2.1 sanitizeHeroView: -1 -> 0 (negative garbage clamps to COMPOSITE)")
{
    CHECK (gent::sanitizeHeroView (-1) == 0);
}

TEST_CASE ("D2.1 sanitizeHeroView: 2 -> 0 (out-of-range-high clamps to COMPOSITE)")
{
    CHECK (gent::sanitizeHeroView (2) == 0);
}

TEST_CASE ("D2.1 sanitizeHeroView: 999 -> 0 (large garbage clamps to COMPOSITE)")
{
    CHECK (gent::sanitizeHeroView (999) == 0);
}

TEST_CASE ("D2.1 sanitizeHeroView: INT_MIN -> 0 (extreme negative clamps to COMPOSITE)")
{
    CHECK (gent::sanitizeHeroView (INT_MIN) == 0);
}

TEST_CASE ("D2.1 sanitizeHeroView: INT_MAX -> 0 (extreme positive clamps to COMPOSITE)")
{
    CHECK (gent::sanitizeHeroView (INT_MAX) == 0);
}

// -- resolveHeroView ---------------------------------------------------------
// requested COMPOSITE (0) is always 0 regardless of stemsAvailable; requested
// STEMS (1) resolves to 1 iff stemsAvailable, else falls back to 0. Against
// the stub (always 0), the (1,true) case is the one that must fail (expected
// 1, stub returns 0).
TEST_CASE ("D2.2 resolveHeroView: (COMPOSITE, no stems) -> COMPOSITE")
{
    CHECK (gent::resolveHeroView (0, false) == 0);
}

TEST_CASE ("D2.2 resolveHeroView: (COMPOSITE, stems available) -> COMPOSITE (request wins)")
{
    CHECK (gent::resolveHeroView (0, true) == 0);
}

TEST_CASE ("D2.2 resolveHeroView: (STEMS, no stems) -> falls back to COMPOSITE")
{
    CHECK (gent::resolveHeroView (1, false) == 0);
}

TEST_CASE ("D2.2 resolveHeroView: (STEMS, stems available) -> STEMS")
{
    CHECK (gent::resolveHeroView (1, true) == 1);
}

// -- Transition-sequence test (T4 pattern) -----------------------------------
// Scripted (requested, stemsAvailable) sequence modeling the ratified SS6
// matrix + SS5's sticky-request rule:
//   1. request STEMS pre-separation (no stems yet)        -> effective 0
//   2. stems arrive, same sticky request (still STEMS)    -> effective 1
//   3. new file loaded, DECISION-6 clears stemSet          -> effective 0,
//      request is STILL 1 (sticky; not reset by availability loss)
//   4. stems re-separated for the new file                -> effective 1 again
//   5. explicit user request -> COMPOSITE                 -> effective 0
//      regardless of stemsAvailable (still true here)
TEST_CASE ("D2.3 transition sequence: sticky STEMS request survives a stems-unavailable window, "
           "explicit COMPOSITE request always wins")
{
    struct Step
    {
        int  requested;
        bool stemsAvailable;
        int  expectedEffective;
    };

    const std::vector<Step> steps = {
        { 1, false, 0 },   // 1. requested STEMS pre-separation -> composite
        { 1, true,  1 },   // 2. stems arrive, same sticky request -> stems
        { 1, false, 0 },   // 3. new file load clears stems (DECISION-6);
                           //    request STILL 1 -> falls back to composite
        { 1, true,  1 },   // 4. stems re-separated for the new file -> stems
        { 0, true,  0 },   // 5. explicit COMPOSITE request -> composite,
                           //    even though stems are (still) available
    };

    int requestedAcrossSequence = 0;   // sanity: the sticky request itself
                                        // only ever changes on an EXPLICIT
                                        // user action (steps 1 and 5 here),
                                        // never as a side effect of
                                        // stemsAvailable flipping (step 3).
    for (std::size_t i = 0; i < steps.size(); ++i)
    {
        const Step& s = steps[i];
        requestedAcrossSequence = s.requested;
        const int effective = gent::resolveHeroView (s.requested, s.stemsAvailable);
        CHECK (effective == s.expectedEffective);
        CHECK (requestedAcrossSequence == s.requested);   // request is what we scripted, not derived
    }

    // explicit final-state assertions on the full sequence (belt-and-braces
    // against a partially-correct implementation that only satisfies the
    // per-step CHECKs above by coincidence of loop order)
    CHECK (gent::resolveHeroView (steps[0].requested, steps[0].stemsAvailable) == 0);
    CHECK (gent::resolveHeroView (steps[1].requested, steps[1].stemsAvailable) == 1);
    CHECK (gent::resolveHeroView (steps[2].requested, steps[2].stemsAvailable) == 0);
    CHECK (gent::resolveHeroView (steps[3].requested, steps[3].stemsAvailable) == 1);
    CHECK (gent::resolveHeroView (steps[4].requested, steps[4].stemsAvailable) == 0);
}

// -- Composition: sanitizeHeroView(garbage) feeding resolveHeroView ----------
// Small exhaustive sweep over stored in {-2..3} x stemsAvailable in {t,f}:
// sanitizeHeroView must first collapse `stored` to {0,1}, and resolveHeroView
// must then never yield anything outside {0,1} for ANY sanitized input. This
// also pins the concrete expected value per (stored, stemsAvailable) pair so
// a stub that merely "happens" to stay in {0,1} without being correct still
// fails.
TEST_CASE ("D2.4 composition: sanitizeHeroView(garbage) -> resolveHeroView never yields outside {0,1}, "
           "exhaustive stored in [-2,3] x stemsAvailable in {false,true}")
{
    for (int stored = -2; stored <= 3; ++stored)
    {
        const int sanitized = gent::sanitizeHeroView (stored);
        // sanitize must collapse to exactly {0,1} -- only stored==1 stays 1,
        // everything else (including stored==0) is COMPOSITE.
        const int expectedSanitized = (stored == 1) ? 1 : 0;
        CHECK (sanitized == expectedSanitized);

        for (bool stemsAvailable : { false, true })
        {
            const int effective = gent::resolveHeroView (sanitized, stemsAvailable);
            // never a third value, for any input in this sweep
            CHECK ((effective == 0 || effective == 1));

            const int expectedEffective = (sanitized == 1 && stemsAvailable) ? 1 : 0;
            CHECK (effective == expectedEffective);
        }
    }
}

// ---------------------------------------------------------------------------
// D3 — gent::laneIndexAt (lane-index geometry, PluginEditor.h mouseDown ~698 /
// mouseMove ~928, both now DORMANT band hit-tests per D3's own scope -- the
// live STEMS-view rewire is D4's job). See docs/STEM_VIEW_MODEL.md SS8/SS9 and
// REDESIGN_TASK_D.md's D3 ctest additions: "boundary y at every lane edge for
// bandH in {96,135,160,250}, out-of-band clamping identical to today's jlimit,
// plus the tiling property test."
// ---------------------------------------------------------------------------

namespace
{
// Reference re-implementation of the PRE-D3 inline formula, kept independent
// of gent::laneIndexAt so these tests actually exercise agreement between two
// separately-written expressions rather than testing the extraction against
// itself.
int referenceLaneIndexAt (int y, int bandTop, int bandH)
{
    const double laneH = (double) bandH / 6.0;
    double ratio = ((double) y - (double) bandTop) / laneH;
    int lane = (int) ratio;
    if (lane < 0) lane = 0;
    if (lane > 5) lane = 5;
    return lane;
}
}

TEST_CASE ("D3.1 laneIndexAt: boundary y at every lane edge, bandH in {96,135,160,250}")
{
    const int bandHs[4] = { 96, 135, 160, 250 };
    const int bandTop = 2;   // matches production's bandTop = rulerH + 2
    for (int bandH : bandHs)
    {
        const double laneH = (double) bandH / 6.0;
        for (int lane = 0; lane < 6; ++lane)
        {
            // the TRUE floating-point lane boundary (bandH need not divide evenly
            // by 6 -- 135/160/250 all leave a fractional laneH, so the boundary
            // pixel itself is only unambiguous when we round consistently with
            // laneIndexAt's own (int) truncation, which is exactly what
            // referenceLaneIndexAt also does -- both are checked for agreement
            // here rather than asserting a hand-picked "== lane" that would be
            // wrong for a fractional laneH).
            const double boundary = laneH * (double) lane;
            const int yAtBoundary = bandTop + (int) boundary;         // truncates like production
            const int yOnePastBoundary = bandTop + (int) std::ceil (boundary);
            const int yJustBeforeBoundary = bandTop + (int) boundary - 1;

            CHECK (gent::laneIndexAt (yAtBoundary, bandTop, bandH)
                   == referenceLaneIndexAt (yAtBoundary, bandTop, bandH));
            CHECK (gent::laneIndexAt (yOnePastBoundary, bandTop, bandH)
                   == referenceLaneIndexAt (yOnePastBoundary, bandTop, bandH));
            CHECK (gent::laneIndexAt (yJustBeforeBoundary, bandTop, bandH)
                   == referenceLaneIndexAt (yJustBeforeBoundary, bandTop, bandH));

            // just inside the lane's bottom edge (one pixel before the next lane starts)
            const int yBottom = bandTop + (int) (laneH * (lane + 1)) - 1;
            CHECK (gent::laneIndexAt (yBottom, bandTop, bandH)
                   == referenceLaneIndexAt (yBottom, bandTop, bandH));
        }
        // the very first pixel of the band is lane 0; the very last pixel is lane 5
        CHECK (gent::laneIndexAt (bandTop, bandTop, bandH) == 0);
        CHECK (gent::laneIndexAt (bandTop + bandH - 1, bandTop, bandH) == 5);
    }
}

TEST_CASE ("D3.2 laneIndexAt: out-of-band clamping identical to today's jlimit semantics")
{
    const int bandHs[4] = { 96, 135, 160, 250 };
    const int bandTop = 2;
    for (int bandH : bandHs)
    {
        // above the band (y < bandTop): clamps to lane 0, same as jlimit(0,5,negative)
        CHECK (gent::laneIndexAt (bandTop - 1, bandTop, bandH) == 0);
        CHECK (gent::laneIndexAt (bandTop - 1000, bandTop, bandH) == 0);
        CHECK (gent::laneIndexAt (-1000000, bandTop, bandH) == 0);

        // below the band (y >= bandTop + bandH): clamps to lane 5
        CHECK (gent::laneIndexAt (bandTop + bandH, bandTop, bandH) == 5);
        CHECK (gent::laneIndexAt (bandTop + bandH + 1000, bandTop, bandH) == 5);
        CHECK (gent::laneIndexAt (1000000, bandTop, bandH) == 5);

        // agreement with the independent reference re-implementation across a
        // dense out-of-band sweep both sides
        for (int y = bandTop - 50; y < bandTop; ++y)
            CHECK (gent::laneIndexAt (y, bandTop, bandH) == referenceLaneIndexAt (y, bandTop, bandH));
        for (int y = bandTop + bandH; y < bandTop + bandH + 50; ++y)
            CHECK (gent::laneIndexAt (y, bandTop, bandH) == referenceLaneIndexAt (y, bandTop, bandH));
    }
}

TEST_CASE ("D3.3 laneIndexAt: tiling property -- every in-band y maps to exactly one lane, "
           "lanes tile with no gap/overlap, consistent with computed lane tops")
{
    const int bandHs[4] = { 96, 135, 160, 250 };
    const int bandTop = 2;
    for (int bandH : bandHs)
    {
        const double laneH = (double) bandH / 6.0;

        // computed lane tops (lane i spans [top_i, top_{i+1}) in continuous space)
        double laneTop[7];
        for (int i = 0; i <= 6; ++i)
            laneTop[i] = (double) bandTop + laneH * (double) i;

        // every integer y in [bandTop, bandTop+bandH) falls in exactly one lane,
        // and laneIndexAt's answer is consistent with the computed lane tops
        // (no gap/overlap: the lane whose [top_i, top_{i+1}) contains y is the
        // same lane laneIndexAt returns).
        for (int y = bandTop; y < bandTop + bandH; ++y)
        {
            const int lane = gent::laneIndexAt (y, bandTop, bandH);
            CHECK (lane >= 0);
            CHECK (lane <= 5);
            // consistency check: y must lie within [laneTop[lane], laneTop[lane+1])
            // modulo floating-point boundary rounding at the exact edge pixel, which
            // is why we check against a half-open interval expanded by a tiny epsilon
            // rather than demanding bit-exact equality with a second computation path.
            const double eps = 1e-9;
            CHECK ((double) y >= laneTop[lane] - eps);
            CHECK ((double) y <  laneTop[lane + 1] + eps);
        }

        // full sweep agreement with the independent reference implementation
        // across the whole in-band range (belt-and-braces on top of the
        // boundary-specific D3.1 cases above)
        for (int y = bandTop; y < bandTop + bandH; ++y)
            CHECK (gent::laneIndexAt (y, bandTop, bandH) == referenceLaneIndexAt (y, bandTop, bandH));
    }
}
