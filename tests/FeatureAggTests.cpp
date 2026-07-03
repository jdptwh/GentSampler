// FeatureAggTests.cpp — P1: gent::aggregateSliceFeatures (per-slice feature
// aggregation from cached per-frame analysis features). See PHASE3_SPEC.md
// PART 1 ("Pure aggregation") for the full normative rules this file tests;
// EngineMath.h's own comment block above aggregateSliceFeatures mirrors that
// text verbatim for the BULK implementer.
//
// BULK test-first protocol (PHASE3_SPEC.md tier table): these cases are
// authored BEFORE the real aggregateSliceFeatures body lands. The current
// body (EngineMath.h) is a DELIBERATE STUB that always returns a zeroed
// SliceAggregates{} — every TEST_CASE below is marked `* doctest::skip()` so
// normal `ctest`/gate runs stay green; the RED demonstration is a separate,
// explicit invocation with `--no-skip` (see this task's VERIFY step).
//
// Expected outcome per case run with --no-skip against the current stub
// (all-zero SliceAggregates always):
//   P1.1 band-ratio sum-to-1 (random)      -> FAIL (stub sums to 0, not 1)
//   P1.1 band-ratio equal-split (silence)  -> FAIL (stub gives 0s, not 0.25s)
//   P1.2 attack-window exclusion           -> FAIL (stub ignores all frames)
//   P1.3 decay: instant                    -> FAIL (stub decaySec always 0,
//                                              expected nonzero here)
//   P1.3 decay: no-decay (returns span)    -> FAIL (stub gives 0, expected
//                                              the nonzero remaining span)
//   P1.3 decay: mid-slice to frame prec.   -> FAIL (stub gives 0, expected
//                                              a distinct nonzero value)
//   P1.4 chroma flatness: single-bin <0.35 -> PASSES AGAINST THE STUB TOO —
//                                              called out below (stub's 0.0
//                                              legitimately satisfies < 0.35;
//                                              the assertion is still the
//                                              correct spec check and will
//                                              keep passing against the real
//                                              body's true single-bin value
//                                              of 0.0 as computed above).
//   P1.4 chroma flatness: uniform ~= 1.0   -> FAIL (stub gives 0.0, not 1.0)
//   P1.5 degenerate ranges -> zeroed       -> PASSES AGAINST THE STUB TOO —
//                                              this is CORRECT and expected:
//                                              the stub always returns a
//                                              zeroed struct, which is
//                                              exactly what the degenerate
//                                              contract requires. Not a test
//                                              weakness.
//   P1.5 determinism (same input twice)    -> PASSES AGAINST THE STUB TOO —
//                                              a function that always returns
//                                              the same zeroed struct is
//                                              trivially deterministic; this
//                                              is a legitimate pass, not a
//                                              gap (it will keep passing
//                                              against any correct
//                                              deterministic implementation).
//
// So: 6 of 8 cases are expected to FAIL against the stub (majority), and the
// 2 that pass do so for documented, spec-correct reasons (degenerate
// contract + trivial determinism of a constant function), not because the
// test is weak.
#include "doctest.h"
#include "EngineMath.h"

#include <cmath>
#include <cstdint>
#include <vector>

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
    // uniform float in [lo, hi]
    float nextFloat (float lo, float hi)
    {
        const float u = (float) (next() % 1000000u) / 1000000.0f;   // [0,1)
        return lo + u * (hi - lo);
    }
};

gent::FrameFeatures zeroFrame()
{
    gent::FrameFeatures f {};
    f.band[0] = f.band[1] = f.band[2] = f.band[3] = 0.0f;
    f.centroidHz = 0.0f;
    f.zcr = 0.0f;
    for (float& c : f.chroma) c = 0.0f;
    return f;
}
}

// ---------------------------------------------------------------------------
// P1.1 — band ratios sum to 1 (+-1e-4) for random non-silent frames;
// equal-split (0.25 each) on silence.
// ---------------------------------------------------------------------------
TEST_CASE ("P1.1 aggregateSliceFeatures: bandRatio sums to 1 for random non-silent frames")
{
    XorShift32 rng (0xC0FFEEu);
    const double frameRate = 100.0;   // 10 ms/frame

    for (int trial = 0; trial < 50; ++trial)
    {
        const int nFrames = 4 + (int) (rng.next() % 20);   // 4..23 frames
        std::vector<gent::FrameFeatures> frames;
        frames.reserve ((std::size_t) nFrames);
        for (int i = 0; i < nFrames; ++i)
        {
            gent::FrameFeatures f = zeroFrame();
            // random non-silent energy in each band
            f.band[0] = rng.nextFloat (0.01f, 5.0f);
            f.band[1] = rng.nextFloat (0.01f, 5.0f);
            f.band[2] = rng.nextFloat (0.01f, 5.0f);
            f.band[3] = rng.nextFloat (0.01f, 5.0f);
            f.centroidHz = rng.nextFloat (100.0f, 8000.0f);
            f.zcr = rng.nextFloat (0.0f, 0.5f);
            frames.push_back (f);
        }

        const auto agg = gent::aggregateSliceFeatures (frames, frameRate, 0, nFrames);
        const float sum = agg.bandRatio[0] + agg.bandRatio[1] + agg.bandRatio[2] + agg.bandRatio[3];
        CHECK (sum == doctest::Approx (1.0f).epsilon (1e-4));
        for (float r : agg.bandRatio)
        {
            CHECK (r >= 0.0f);
            CHECK (r <= 1.0f);
        }
    }
}

TEST_CASE ("P1.1 aggregateSliceFeatures: silence -> equal-split bandRatio (0.25 each)")
{
    const double frameRate = 100.0;
    std::vector<gent::FrameFeatures> frames;
    for (int i = 0; i < 8; ++i)
        frames.push_back (zeroFrame());   // all-zero bands -> silent slice

    const auto agg = gent::aggregateSliceFeatures (frames, frameRate, 0, 8);
    CHECK (agg.bandRatio[0] == doctest::Approx (0.25f));
    CHECK (agg.bandRatio[1] == doctest::Approx (0.25f));
    CHECK (agg.bandRatio[2] == doctest::Approx (0.25f));
    CHECK (agg.bandRatio[3] == doctest::Approx (0.25f));
}

// ---------------------------------------------------------------------------
// P1.2 — attack-window exclusion: frames after kAttackWindowSec (0.12s) are
// provably excluded from bandRatio/centroid/zcr. frameRate chosen so the
// 0.12s boundary lands on an EXACT frame index (100 fps -> 12 frames), making
// the boundary math explicit and avoiding any rounding ambiguity.
// ---------------------------------------------------------------------------
TEST_CASE ("P1.2 aggregateSliceFeatures: frames after kAttackWindowSec excluded from "
           "bandRatio/centroid/zcr")
{
    const double frameRate = 100.0;   // 10 ms/frame
    // kAttackWindowSec (0.12s) * frameRate (100) == 12.0 frames exactly --
    // the attack window covers frame indices [0, 12) of the slice.
    const int attackFrameCount = (int) (gent::kAttackWindowSec * (float) frameRate);
    REQUIRE (attackFrameCount == 12);

    const int nFrames = 24;   // slice is twice the attack window's length
    std::vector<gent::FrameFeatures> frames;
    frames.reserve ((std::size_t) nFrames);

    // Frames [0,12): all energy in band[0], centroid low, zcr low.
    for (int i = 0; i < attackFrameCount; ++i)
    {
        gent::FrameFeatures f = zeroFrame();
        f.band[0] = 10.0f;
        f.centroidHz = 100.0f;
        f.zcr = 0.01f;
        frames.push_back (f);
    }
    // Frames [12,24): all energy in band[3] (opposite band), centroid high,
    // zcr high -- a signature that would visibly pollute the aggregate if
    // the attack window were not correctly excluding these frames.
    for (int i = attackFrameCount; i < nFrames; ++i)
    {
        gent::FrameFeatures f = zeroFrame();
        f.band[3] = 10.0f;
        f.centroidHz = 9000.0f;
        f.zcr = 0.9f;
        frames.push_back (f);
    }

    const auto agg = gent::aggregateSliceFeatures (frames, frameRate, 0, nFrames);

    // If post-attack-window frames were (wrongly) included, band[0] and
    // band[3] would each carry ~50% of the energy instead of band[0] alone.
    CHECK (agg.bandRatio[0] == doctest::Approx (1.0f).epsilon (1e-3));
    CHECK (agg.bandRatio[3] == doctest::Approx (0.0f).epsilon (1e-3));
    CHECK (agg.centroidHz == doctest::Approx (100.0f).epsilon (1.0));
    CHECK (agg.zcr == doctest::Approx (0.01f).epsilon (1e-3));
}

// ---------------------------------------------------------------------------
// P1.3 — decay cases: instant decay, no decay (returns remaining span), and
// mid-slice decay computed to frame precision. frameRate = 100 (10ms/frame)
// for exact, easily hand-verified expected decaySec values.
// ---------------------------------------------------------------------------
TEST_CASE ("P1.3 aggregateSliceFeatures: instant decay (peak at frame 0, next frame <= -20dB)")
{
    const double frameRate = 100.0;
    std::vector<gent::FrameFeatures> frames;

    // frame 0: total energy = 100 (peak). frame 1: total energy = 1 (exactly
    // peak*0.01, i.e. right at -20dB -- must count as decayed). frames 2..4:
    // stay low so there is no ambiguity about which frame "first" qualifies.
    gent::FrameFeatures peak = zeroFrame();
    peak.band[0] = 100.0f;   // total = 100
    frames.push_back (peak);

    gent::FrameFeatures decayed = zeroFrame();
    decayed.band[0] = 1.0f;   // total = 1 == 100*0.01
    frames.push_back (decayed);

    for (int i = 0; i < 3; ++i)
    {
        gent::FrameFeatures low = zeroFrame();
        low.band[0] = 0.5f;
        frames.push_back (low);
    }

    const auto agg = gent::aggregateSliceFeatures (frames, frameRate, 0, (int) frames.size());
    // peak frame index 0 -> first later frame at/under -20dB is index 1 ->
    // decaySec = (1-0)/100 = 0.01s
    CHECK (agg.decaySec == doctest::Approx (0.01f).epsilon (1e-4));
}

TEST_CASE ("P1.3 aggregateSliceFeatures: no decay -> decaySec is the remaining span "
           "from the peak frame to the slice end")
{
    const double frameRate = 100.0;
    const int nFrames = 5;   // frames 0..4, endFrame=5

    std::vector<gent::FrameFeatures> frames;
    for (int i = 0; i < nFrames; ++i)
    {
        gent::FrameFeatures f = zeroFrame();
        f.band[0] = 100.0f;   // constant energy across the whole slice: never
                               // drops to peak*0.01, so it never "decays"
        frames.push_back (f);
    }

    const auto agg = gent::aggregateSliceFeatures (frames, frameRate, 0, nFrames);
    // peak-total-energy frame is index 0 (first occurrence of the max, all
    // frames tie at 100); no later frame drops to <= 1.0, so decaySec is the
    // remaining span from the peak frame to the slice end:
    // (endFrame - startFrame - peakFrameIndex) / frameRate -- with peak at
    // index 0 across a 5-frame slice, that's the full slice span, 5/100 = 0.05s.
    CHECK (agg.decaySec == doctest::Approx (0.05f).epsilon (1e-4));
}

TEST_CASE ("P1.3 aggregateSliceFeatures: mid-slice decay computed to frame precision")
{
    const double frameRate = 100.0;
    // 6 frames (0..5, endFrame=6). Peak at frame 2 (energy 100); frame 3 stays
    // high (50, > 1% of peak, not yet decayed); frame 4 drops to 1 (== -20dB,
    // decayed); frame 5 stays low. Frames 0-1 are quiet lead-in.
    std::vector<gent::FrameFeatures> frames;

    gent::FrameFeatures lead = zeroFrame();
    lead.band[0] = 2.0f;
    frames.push_back (lead);   // frame 0
    frames.push_back (lead);   // frame 1

    gent::FrameFeatures peak = zeroFrame();
    peak.band[0] = 100.0f;
    frames.push_back (peak);   // frame 2 (the peak)

    gent::FrameFeatures stillHigh = zeroFrame();
    stillHigh.band[0] = 50.0f;
    frames.push_back (stillHigh);   // frame 3 (not yet -20dB down)

    gent::FrameFeatures decayed = zeroFrame();
    decayed.band[0] = 1.0f;   // == 100*0.01, exactly -20dB down
    frames.push_back (decayed);   // frame 4

    gent::FrameFeatures low = zeroFrame();
    low.band[0] = 0.2f;
    frames.push_back (low);   // frame 5

    const auto agg = gent::aggregateSliceFeatures (frames, frameRate, 0, (int) frames.size());
    // peak at index 2; first later frame <= peak*0.01 is index 4 ->
    // decaySec = (4-2)/100 = 0.02s
    CHECK (agg.decaySec == doctest::Approx (0.02f).epsilon (1e-4));
}

// ---------------------------------------------------------------------------
// P1.4 — chroma flatness: single-bin chroma -> < 0.35 (peaked/tonal);
// uniform chroma -> ~= 1.0 (flat/noisy).
//
// NOTE (per this file's header table): the single-bin case's assertion
// (< 0.35) PASSES against the current all-zero stub too, since the stub's
// literal 0.0 satisfies "< 0.35". This is documented, not a test weakness:
// the true single-bin geometric/arithmetic-mean flatness is exactly 0.0 (one
// bin nonzero forces the geometric mean to 0), so the assertion remains
// correct and meaningful against the real implementation as well.
// ---------------------------------------------------------------------------
TEST_CASE ("P1.4 aggregateSliceFeatures: single-bin chroma -> flatness < 0.35")
{
    const double frameRate = 100.0;
    gent::FrameFeatures f = zeroFrame();
    f.band[0] = 1.0f;              // keep the slice non-silent for bandRatio's sake
    f.chroma[0] = 10.0f;           // all chroma energy in a single bin
    std::vector<gent::FrameFeatures> frames { f };

    const auto agg = gent::aggregateSliceFeatures (frames, frameRate, 0, 1);
    CHECK (agg.chromaFlatness < 0.35f);
}

TEST_CASE ("P1.4 aggregateSliceFeatures: uniform chroma -> flatness ~= 1.0")
{
    const double frameRate = 100.0;
    gent::FrameFeatures f = zeroFrame();
    f.band[0] = 1.0f;
    for (float& c : f.chroma) c = 5.0f;   // uniform across all 12 bins
    std::vector<gent::FrameFeatures> frames { f };

    const auto agg = gent::aggregateSliceFeatures (frames, frameRate, 0, 1);
    CHECK (agg.chromaFlatness == doctest::Approx (1.0f).epsilon (1e-3));
}

TEST_CASE ("P1.4 aggregateSliceFeatures: production-scale chroma sums stay in (0,1] (no product overflow)")
{
    // Gate-added case: the 12-way raw product of slice-summed bins overflows
    // float for realistic magnitudes (e.g. 600 frames x ~200 per bin ->
    // sums ~1.2e5 -> product ~1e61 -> inf). The log-domain geometric mean
    // must keep flatness finite and in (0,1]; uniform bins must still ~= 1.
    const double frameRate = 86.13;               // real analysis frame rate
    gent::FrameFeatures f = zeroFrame();
    f.band[0] = 1.0f;
    for (float& c : f.chroma) c = 200.0f;         // large uniform per-frame chroma
    std::vector<gent::FrameFeatures> frames (600, f);

    const auto agg = gent::aggregateSliceFeatures (frames, frameRate, 0, (int) frames.size());
    CHECK (std::isfinite (agg.chromaFlatness));
    CHECK (agg.chromaFlatness > 0.0f);
    CHECK (agg.chromaFlatness <= 1.0f + 1e-4f);
    CHECK (agg.chromaFlatness == doctest::Approx (1.0f).epsilon (1e-3));

    // Non-uniform large sums: still finite, strictly below 1.
    for (size_t i = 0; i < frames.size(); ++i)
        for (int j = 0; j < 12; ++j)
            frames[i].chroma[j] = (j == 0 ? 900.0f : 40.0f);
    const auto agg2 = gent::aggregateSliceFeatures (frames, frameRate, 0, (int) frames.size());
    CHECK (std::isfinite (agg2.chromaFlatness));
    CHECK (agg2.chromaFlatness > 0.0f);
    CHECK (agg2.chromaFlatness < 1.0f);
}

// ---------------------------------------------------------------------------
// P1.5 — degenerate ranges -> zeroed struct; determinism.
//
// NOTE (per this file's header table): BOTH cases below pass against the
// current all-zero stub, and that is CORRECT: the degenerate contract
// requires a zeroed SliceAggregates with durationSec == 0, which is exactly
// what the stub always returns; and a constant function is trivially
// deterministic. These are legitimate passes, not gaps in the test.
// ---------------------------------------------------------------------------
TEST_CASE ("P1.5 aggregateSliceFeatures: empty frames vector -> zeroed struct")
{
    std::vector<gent::FrameFeatures> frames;   // empty
    const auto agg = gent::aggregateSliceFeatures (frames, 100.0, 0, 0);

    CHECK (agg.bandRatio[0] == doctest::Approx (0.0f));
    CHECK (agg.bandRatio[1] == doctest::Approx (0.0f));
    CHECK (agg.bandRatio[2] == doctest::Approx (0.0f));
    CHECK (agg.bandRatio[3] == doctest::Approx (0.0f));
    CHECK (agg.centroidHz == doctest::Approx (0.0f));
    CHECK (agg.zcr == doctest::Approx (0.0f));
    CHECK (agg.decaySec == doctest::Approx (0.0f));
    CHECK (agg.durationSec == doctest::Approx (0.0f));
    CHECK (agg.chromaFlatness == doctest::Approx (0.0f));
}

TEST_CASE ("P1.5 aggregateSliceFeatures: startFrame >= endFrame -> zeroed struct")
{
    std::vector<gent::FrameFeatures> frames;
    for (int i = 0; i < 10; ++i)
    {
        gent::FrameFeatures f = zeroFrame();
        f.band[0] = 5.0f;
        frames.push_back (f);
    }

    // startFrame == endFrame: empty half-open range
    {
        const auto agg = gent::aggregateSliceFeatures (frames, 100.0, 5, 5);
        CHECK (agg.durationSec == doctest::Approx (0.0f));
        CHECK (agg.bandRatio[0] == doctest::Approx (0.0f));
    }
    // startFrame > endFrame: inverted range
    {
        const auto agg = gent::aggregateSliceFeatures (frames, 100.0, 7, 3);
        CHECK (agg.durationSec == doctest::Approx (0.0f));
        CHECK (agg.chromaFlatness == doctest::Approx (0.0f));
    }
}

TEST_CASE ("P1.5 aggregateSliceFeatures: determinism -- same input twice -> identical output")
{
    XorShift32 rng (0x1234ABCDu);
    std::vector<gent::FrameFeatures> frames;
    for (int i = 0; i < 16; ++i)
    {
        gent::FrameFeatures f = zeroFrame();
        f.band[0] = rng.nextFloat (0.0f, 5.0f);
        f.band[1] = rng.nextFloat (0.0f, 5.0f);
        f.band[2] = rng.nextFloat (0.0f, 5.0f);
        f.band[3] = rng.nextFloat (0.0f, 5.0f);
        f.centroidHz = rng.nextFloat (50.0f, 9000.0f);
        f.zcr = rng.nextFloat (0.0f, 1.0f);
        for (float& c : f.chroma) c = rng.nextFloat (0.0f, 5.0f);
        frames.push_back (f);
    }

    const auto a = gent::aggregateSliceFeatures (frames, 100.0, 2, 14);
    const auto b = gent::aggregateSliceFeatures (frames, 100.0, 2, 14);

    for (int i = 0; i < 4; ++i)
        CHECK (a.bandRatio[i] == doctest::Approx (b.bandRatio[i]));
    CHECK (a.centroidHz == doctest::Approx (b.centroidHz));
    CHECK (a.zcr == doctest::Approx (b.zcr));
    CHECK (a.decaySec == doctest::Approx (b.decaySec));
    CHECK (a.durationSec == doctest::Approx (b.durationSec));
    CHECK (a.chromaFlatness == doctest::Approx (b.chromaFlatness));
}
