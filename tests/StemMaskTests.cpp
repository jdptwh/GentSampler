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

// ---------------------------------------------------------------------------
// D4 — gent::laneZoneAt (lane x-classification: mute / solo / wave)
// See docs/STEM_VIEW_MODEL.md SS8 and REDESIGN_TASK_D.md's D4 ctest scope:
// "extract gent::laneZoneAt (int x, int w, int labW, int soloW) -> {mute,
// solo, wave} classification extracted from the [dormant] constants; tests:
// exact boundary pixels both sides of both zones, degenerate small w."
//
// Extracted verbatim from the dormant band hit-test, PluginEditor.h::mouseDown
// (pre-D4 lines 704-710, cited/verified during D4 authorship):
//   if (stemBandH > 0 && e.y >= stemBandTop && e.y < stemBandTop + stemBandH)
//   {
//       const int lane = gent::laneIndexAt (e.y, stemBandTop, stemBandH);
//       if (e.x >= w - stemSoloW - 6)
//           p.setStemSoloed (lane, ! p.isStemSoloed (lane));
//       else if (e.x <= 4 + stemLabW + 6)
//           p.setStemMuted (lane, ! p.isStemMuted (lane));
//       ...
//   }
// Boundary constants: SOLO zone x >= w - soloW - 6 (line 707); MUTE zone
// x <= 4 + labW + 6 (line 709); if-order is SOLO first, so on overlap SOLO
// wins -- these tests pin exactly that behavior.
// ---------------------------------------------------------------------------

namespace
{
// Reference re-implementation of the dormant if/else-if chain, independent of
// gent::laneZoneAt, so these tests exercise agreement between two separately
// -written expressions.
gent::LaneZone referenceLaneZoneAt (int x, int w, int labW, int soloW)
{
    if (x >= w - soloW - 6)
        return gent::LaneZone::solo;
    if (x <= 4 + labW + 6)
        return gent::LaneZone::mute;
    return gent::LaneZone::wave;
}
}

TEST_CASE ("D4.1 laneZoneAt: mute-zone boundary pixels both sides, typical geometry (w=1016, labW=60, soloW=18)")
{
    const int w = 1016, labW = 60, soloW = 18;
    const int muteEdge = 4 + labW + 6;   // == 70

    CHECK (gent::laneZoneAt (muteEdge, w, labW, soloW) == gent::LaneZone::mute);       // on the boundary: mute
    CHECK (gent::laneZoneAt (muteEdge - 1, w, labW, soloW) == gent::LaneZone::mute);   // one px inside: still mute
    CHECK (gent::laneZoneAt (muteEdge + 1, w, labW, soloW) == gent::LaneZone::wave);   // one px past: wave
    CHECK (gent::laneZoneAt (0, w, labW, soloW) == gent::LaneZone::mute);              // x=0: mute
}

TEST_CASE ("D4.2 laneZoneAt: solo-zone boundary pixels both sides, typical geometry (w=1016, labW=60, soloW=18)")
{
    const int w = 1016, labW = 60, soloW = 18;
    const int soloEdge = w - soloW - 6;   // == 992

    CHECK (gent::laneZoneAt (soloEdge, w, labW, soloW) == gent::LaneZone::solo);       // on the boundary: solo
    CHECK (gent::laneZoneAt (soloEdge + 1, w, labW, soloW) == gent::LaneZone::solo);   // one px further right: still solo
    CHECK (gent::laneZoneAt (soloEdge - 1, w, labW, soloW) == gent::LaneZone::wave);   // one px before: wave
    CHECK (gent::laneZoneAt (w - 1, w, labW, soloW) == gent::LaneZone::solo);          // rightmost px: solo
    CHECK (gent::laneZoneAt (w, w, labW, soloW) == gent::LaneZone::solo);              // exactly at w: solo (>= holds)
}

TEST_CASE ("D4.3 laneZoneAt: middle of the lane (typical geometry) classifies as wave")
{
    const int w = 1016, labW = 60, soloW = 18;
    CHECK (gent::laneZoneAt (500, w, labW, soloW) == gent::LaneZone::wave);
    CHECK (gent::laneZoneAt (71, w, labW, soloW) == gent::LaneZone::wave);    // just past mute edge
    CHECK (gent::laneZoneAt (991, w, labW, soloW) == gent::LaneZone::wave);   // just before solo edge
}

TEST_CASE ("D4.4 laneZoneAt: degenerate small w -- zones overlap, SOLO wins (matches dormant if/else-if order)")
{
    // w small enough that the solo boundary (w - soloW - 6) falls AT or BEFORE
    // the mute boundary (4 + labW + 6) -- every x classifies as either solo or
    // mute, never wave, and wherever both conditions would be true, solo must
    // win because the dormant code tests solo FIRST (if / else if).
    const int labW = 60, soloW = 18;
    const int muteEdge = 4 + labW + 6;   // 70

    // pick w so soloEdge (w - soloW - 6) == muteEdge exactly -> total overlap
    const int wExactOverlap = muteEdge + soloW + 6;   // == 94
    CHECK (wExactOverlap - soloW - 6 == muteEdge);    // sanity: soloEdge == muteEdge here

    for (int x = 0; x <= wExactOverlap; ++x)
    {
        const auto zone = gent::laneZoneAt (x, wExactOverlap, labW, soloW);
        // every x in [0, w] must be mute or solo, never wave, in total overlap
        CHECK (zone != gent::LaneZone::wave);
        // at the exact shared boundary pixel (muteEdge == soloEdge), solo wins
        if (x == muteEdge)
            CHECK (zone == gent::LaneZone::solo);
    }

    // even smaller w (soloEdge strictly less than muteEdge -- solo zone
    // consumes the whole mute zone too): solo wins everywhere it's true,
    // and since soloEdge <= muteEdge here, that's the entire non-negative
    // x range up to w.
    const int wSmaller = 40;   // soloEdge = 40-18-6 = 16; muteEdge = 70 (>= any x in [0,40])
    for (int x = 0; x <= wSmaller; ++x)
    {
        const auto zone = gent::laneZoneAt (x, wSmaller, labW, soloW);
        // for wSmaller, muteEdge (70) >= every x in range, so the mute test
        // would ALSO be true for every x -- solo tested first must still win
        // wherever the solo test is true (x >= 16); below that, mute wins
        // (matching the dormant if/else-if: solo checked first, mute is the
        // else-if fallback).
        const int soloEdgeSmaller = wSmaller - soloW - 6;   // 16
        if (x >= soloEdgeSmaller)
            CHECK (zone == gent::LaneZone::solo);
        else
            CHECK (zone == gent::LaneZone::mute);
    }
}

TEST_CASE ("D4.5 laneZoneAt: agreement with independent reference re-implementation, dense sweep incl. degenerate w")
{
    const int labW = 60, soloW = 18;
    const int ws[6] = { 1016, 500, 200, 94, 40, 10 };
    for (int w : ws)
        for (int x = -20; x <= w + 20; ++x)
            CHECK (gent::laneZoneAt (x, w, labW, soloW) == referenceLaneZoneAt (x, w, labW, soloW));
}

// ---------------------------------------------------------------------------
// D4.6 — composition: laneIndexAt x laneZoneAt over a scripted click grid
// reproducing today's toggle targets for the same relative geometry (D4's own
// ctest scope: "Composition test: laneIndexAt x laneZoneAt over a scripted
// click grid reproduces today's toggle targets for the same relative
// geometry (behavior-identical claim for the zones that survive)").
//
// Models a STEMS-view click at (x, y) against the full-lane geometry D4 wires
// in mouseDown: lane = laneIndexAt(y, 0, waveBottom); zone = laneZoneAt(x, w,
// labW, soloW). Scripts one click per lane at the mute plate and one at the
// solo box, verifying each lands on the EXPECTED lane index with the EXPECTED
// zone -- i.e. clicking lane k's plate toggles lane k's mute (not some other
// lane), and same for solo, across the full 6-lane run.
// ---------------------------------------------------------------------------
TEST_CASE ("D4.6 composition: laneIndexAt x laneZoneAt reproduces per-lane mute/solo toggle targets")
{
    const int w = 1016, labW = 60, soloW = 18;
    const int waveBottom = 160;   // typical hero wave-area height (1040x700 layout)
    const double laneH = (double) waveBottom / 6.0;

    for (int lane = 0; lane < 6; ++lane)
    {
        const int rowMidY = (int) (laneH * lane + laneH * 0.5);   // click at the lane's vertical center

        // click at the mute plate's horizontal center
        const int muteX = labW / 2;
        CHECK (gent::laneIndexAt (rowMidY, 0, waveBottom) == lane);
        CHECK (gent::laneZoneAt (muteX, w, labW, soloW) == gent::LaneZone::mute);

        // click at the solo box's horizontal center (near the lane's right edge)
        const int soloX = w - soloW / 2;
        CHECK (gent::laneIndexAt (rowMidY, 0, waveBottom) == lane);
        CHECK (gent::laneZoneAt (soloX, w, labW, soloW) == gent::LaneZone::solo);

        // click in the middle of the lane's wave column: same lane, wave zone
        // (falls through to the shared hit-test paths, not a toggle)
        const int waveX = w / 2;
        CHECK (gent::laneIndexAt (rowMidY, 0, waveBottom) == lane);
        CHECK (gent::laneZoneAt (waveX, w, labW, soloW) == gent::LaneZone::wave);
    }
}

// ---------------------------------------------------------------------------
// D5 — async lifecycle x view, the FULL docs/STEM_VIEW_MODEL.md SS6 matrix,
// as scripted predicate sequences over the pure functions (resolveHeroView /
// sanitizeHeroView / the new gent::ctaEnabledFor). Per REDESIGN_TASK_D.md's
// D5 ctest scope ("every row of the matrix is a named TEST_CASE asserting
// the effective view + seg-enabled flag pair") and the D1 addendum (Joe
// ruling 2026-07-03): the PAINT-BRANCH selector is the sanitized REQUEST,
// not resolveHeroView -- requesting STEMS always enters the STEMS branch
// (placeholder-or-real-lanes is a CONTENT question inside that branch, not a
// branch-visibility gate). The seg itself is ALWAYS enabled in every row
// (DECISION-3: "seg enabled always") -- what varies row to row is (a) the
// effective/resolved view fed to resolveHeroView, and (b) whether the STEMS
// branch, once entered, shows real lanes vs a placeholder, and (c) whether
// that placeholder's CTA click-target is enabled (gent::ctaEnabledFor).
//
// Row numbering matches docs/STEM_VIEW_MODEL.md SS6's table exactly.
// ---------------------------------------------------------------------------

// Row 1 — no source loaded. hasStems()==false regardless of any lifecycle
// flag; both requests resolve the SAME way (COMPOSITE effective) since a
// missing source is handled by WaveformView::paint's own early return
// (PluginEditor.h ~280-312) BEFORE the heroReq branch is ever reached --
// modeled here as "no source" being indistinguishable from "no stems" at
// the resolveHeroView layer (both are stemsAvailable==false), the doc's own
// framing ("Rows 1, 2, 3, 4, and 6 all share stemsAvailable == false at the
// resolveHeroView layer").
TEST_CASE ("D5.1 matrix row 1 (no source loaded): both requests resolve to COMPOSITE, "
           "CTA enabled (no job running)")
{
    const bool hasStems = false, separating = false, downloading = false;
    CHECK (gent::resolveHeroView (gent::sanitizeHeroView (0), hasStems) == 0);
    CHECK (gent::resolveHeroView (gent::sanitizeHeroView (1), hasStems) == 0);
    CHECK (gent::ctaEnabledFor (separating, downloading) == true);
}

// Row 2 — source loaded, never separated. COMPOSITE request paints the full
// composite wave (untouched by any of this); STEMS request enters the STEMS
// branch (addendum: request selects the branch) and resolves its CONTENT to
// the placeholder (resolveHeroView(1,false)==0). CTA is enabled -- no job is
// running, so the placeholder's click-target is live (row 2's whole point is
// "click SEPARATE STEMS to fill the map").
TEST_CASE ("D5.2 matrix row 2 (loaded, never separated): STEMS request enters STEMS branch, "
           "content resolves to placeholder, CTA enabled")
{
    const bool hasStems = false, separating = false, downloading = false;
    const int reqComposite = gent::sanitizeHeroView (0);
    const int reqStems     = gent::sanitizeHeroView (1);
    CHECK (reqComposite == 0);   // COMPOSITE request -> composite branch, unaffected
    CHECK (reqStems == 1);       // STEMS request -> STEMS branch is entered (addendum)
    CHECK (gent::resolveHeroView (reqStems, hasStems) == 0);   // content: placeholder, not real lanes
    CHECK (gent::ctaEnabledFor (separating, downloading) == true);
}

// Row 3 — downloading models (first run). hasStems() still false (background
// job hasn't produced stems yet); STEMS request still enters the branch and
// still resolves to the placeholder CONTENT slot, but the CTA must now be
// INERT (a job is already running) -- this is the row the D5 gap-check in
// REDESIGN_TASK_D.md specifically calls out ("during downloading/separating,
// should the CTA chip still read SEPARATE STEMS?").
TEST_CASE ("D5.3 matrix row 3 (downloading models): STEMS branch entered, placeholder content, "
           "CTA INERT (job already running)")
{
    const bool hasStems = false, separating = true, downloading = true;
    CHECK (gent::sanitizeHeroView (1) == 1);
    CHECK (gent::resolveHeroView (gent::sanitizeHeroView (1), hasStems) == 0);
    CHECK (gent::ctaEnabledFor (separating, downloading) == false);
}

// Row 4 — separating N% (downloading has already finished, model init/
// inference is running). Same shape as row 3 minus the downloading flag.
TEST_CASE ("D5.4 matrix row 4 (separating N%): STEMS branch entered, placeholder content, "
           "CTA INERT (job already running)")
{
    const bool hasStems = false, separating = true, downloading = false;
    CHECK (gent::resolveHeroView (gent::sanitizeHeroView (1), hasStems) == 0);
    CHECK (gent::ctaEnabledFor (separating, downloading) == false);
}

// Row 5 — stems ready. hasStems()==true, no job running; STEMS request now
// resolves to REAL lanes (resolveHeroView==1); CTA predicate is irrelevant in
// this row (no placeholder is painted at all once stemsReady==true) but is
// asserted enabled anyway since neither busy flag is set, for completeness
// against the predicate's own contract (it does not know about hasStems).
TEST_CASE ("D5.5 matrix row 5 (stems ready): STEMS request resolves to REAL lanes, "
           "COMPOSITE request unaffected, no job running")
{
    const bool hasStems = true, separating = false, downloading = false;
    CHECK (gent::resolveHeroView (gent::sanitizeHeroView (1), hasStems) == 1);
    CHECK (gent::resolveHeroView (gent::sanitizeHeroView (0), hasStems) == 0);   // COMPOSITE request still wins
    CHECK (gent::ctaEnabledFor (separating, downloading) == true);
}

// Row 6 — separation failed. Both busy flags are false again (doStemJob's
// error paths reset `separating`/`downloadingModels` before returning,
// PluginProcessor.cpp's setStatus(...)+return blocks) and hasStems() is
// false (no stemSet was ever published). STEMS branch still enters and
// resolves to the placeholder CONTENT slot (same as row 2); CTA is enabled
// again so the user can retry -- a prior failure does not lock out a retry,
// same convention as sepStemsBtn (only ever disabled by busy, never by a
// past failure).
TEST_CASE ("D5.6 matrix row 6 (separation failed): STEMS branch entered, placeholder content, "
           "CTA re-enabled for retry (not busy)")
{
    const bool hasStems = false, separating = false, downloading = false;
    CHECK (gent::resolveHeroView (gent::sanitizeHeroView (1), hasStems) == 0);
    CHECK (gent::ctaEnabledFor (separating, downloading) == true);
}

// Row 7 — stems present from restore (kit/session reload with hasStems()==
// true at load time). Sticky-request rule applies exactly as row 5: if the
// restored request was STEMS, it resolves to real lanes immediately with no
// further user action; if the restored request was COMPOSITE, stems being
// ready changes nothing (composite request always wins, matching D2.2's own
// "(COMPOSITE, stems available) -> COMPOSITE (request wins)" case).
TEST_CASE ("D5.7 matrix row 7 (stems present from restore): sticky request resolves immediately "
           "against the restored hasStems()==true state, no further user action needed")
{
    const bool hasStems = true;   // restored StemSet is non-null at load time
    // restored request == STEMS (persisted heroView==1, e.g. via applyStateTree)
    CHECK (gent::resolveHeroView (gent::sanitizeHeroView (1), hasStems) == 1);
    // restored request == COMPOSITE -- stays composite despite stems being ready
    CHECK (gent::resolveHeroView (gent::sanitizeHeroView (0), hasStems) == 0);
}

// -- DECISION-6 kill-case, as a resolveHeroView sequence ---------------------
// Standalone relaunch: persisted request == STEMS, but the restored source is
// missing/empty (silent-source landmine) so hasStems()==false at restore
// time -- must not crash, must render the row-1/row-2 empty/placeholder
// state, request stays sticky, and STEMS engages BY ITSELF the moment
// separation completes for the (new) current source -- no second user click
// required. This is the exact sequence REDESIGN_TASK_D.md's D5 AC names as
// the kill-case demo, expressed here as a pure resolveHeroView trace (the
// same shape as D2.3's transition-sequence test, extended to explicitly name
// the "missing source on relaunch" scenario rather than "new file loaded").
TEST_CASE ("D5.8 DECISION-6 kill-case: persisted STEMS + missing/empty restored source "
           "-> composite/placeholder renders, request sticky, STEMS engages after separation completes")
{
    const int restoredRequest = gent::sanitizeHeroView (1);   // persisted heroView==1 survives restore
    CHECK (restoredRequest == 1);

    // immediately after restore: source is missing/empty, so DECISION-6's
    // loadFile-side clear (or the restore path's own pre-existing state) means
    // hasStems()==false -- no crash, effective view falls back to composite/
    // placeholder-content, but the STICKY REQUEST itself is untouched.
    CHECK (gent::resolveHeroView (restoredRequest, /*hasStems*/ false) == 0);

    // the request never changes across this window (no code path writes
    // heroView except an explicit user click or a fresh restore) --
    // resolveHeroView is re-evaluated against the SAME stored request.
    const int requestAfterFailedRestore = restoredRequest;
    CHECK (requestAfterFailedRestore == 1);

    // once the user loads a real file and separation completes for it,
    // hasStems() flips true against the SAME sticky request -- STEMS engages
    // automatically, no further click needed (SS5/SS6's "no auto-view-flip on
    // separation completion" note: this isn't a special-cased flip, it's the
    // natural consequence of stemsAvailable changing under a constant
    // requested value).
    CHECK (gent::resolveHeroView (requestAfterFailedRestore, /*hasStems*/ true) == 1);
}

// ---------------------------------------------------------------------------
// D5 — gent::ctaEnabledFor (STEMS-placeholder CTA click-target affordance)
// See docs/STEM_VIEW_MODEL.md SS6 rows 2-4/6 and the D5 gap-check in
// REDESIGN_TASK_D.md. Pure OR of the two lifecycle busy-flags read on the
// message thread (WaveformView::paint, same predicates stemStatusLbl already
// reads at PluginEditor.cpp ~1590-1607) -- exhaustive over all 4 boolean
// combinations since the domain is tiny.
// ---------------------------------------------------------------------------
TEST_CASE ("D5.9 ctaEnabledFor: exhaustive over all 4 (separating, downloadingModels) combinations")
{
    CHECK (gent::ctaEnabledFor (false, false) == true);    // idle (row 2 / row 6) -> enabled
    CHECK (gent::ctaEnabledFor (true,  false) == false);   // separating (row 4) -> inert
    CHECK (gent::ctaEnabledFor (false, true)  == false);   // downloading (row 3) -> inert
    CHECK (gent::ctaEnabledFor (true,  true)  == false);   // both set (transient overlap) -> inert
}
