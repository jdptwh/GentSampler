// NoveltyTests.cpp — SECTIONS Part 2: gent::noveltyCurve / smoothCurve /
// pickNoveltyPeaks / noveltyBoundaries (spectral-change section detection).
// See SECTIONS_SPEC.md PART 2 + AMENDMENT P2-A for the full normative
// semantics this file pins. Mirrors tests/SectionTests.cpp's style.
//
// NAMED-CONSTANTS DISCIPLINE: every k/threshold used below is read off
// gent::kNoveltyThresh (never a copied numeric literal), so retuning that
// table can't silently break these tests — same discipline as
// tests/ClassifierTests.cpp vs gent::kThreshStems/kThreshNoStems.
#include "doctest.h"
#include "EngineMath.h"

#include <cmath>

namespace
{
// Build one synthetic FrameFeatures with a given band[4] and chroma[12]
// profile (all frames otherwise zeroed; centroidHz/zcr are not consumed by
// the novelty chain).
gent::FrameFeatures makeFrame (const float band[4], const float chroma[12])
{
    gent::FrameFeatures f {};
    for (int i = 0; i < 4; ++i) f.band[i] = band[i];
    for (int i = 0; i < 12; ++i) f.chroma[i] = chroma[i];
    f.centroidHz = 0.0f;
    f.zcr = 0.0f;
    return f;
}

// Fixture tempo: 120 BPM @ 44100 Hz -> samplesPerBeat = 22050, bar = 88200.
constexpr double kSampleRate = 44100.0;
constexpr double kSamplesPerBar = 88200.0;
// Frame rate matching the real analyzer's hop-512 cadence (sr / hop).
constexpr double kFrameRate = kSampleRate / 512.0;   // ~86.13 fps
// framesPerBar at this fixture: kFrameRate * (kSamplesPerBar / kSampleRate)
//   = kFrameRate * (88200/44100) = kFrameRate * 2 = ~172.27 frames/bar.

std::vector<gent::FrameFeatures> makeStepSequence (int nA, int nB,
                                                    const float bandA[4], const float chromaA[12],
                                                    const float bandB[4], const float chromaB[12])
{
    std::vector<gent::FrameFeatures> frames;
    frames.reserve ((size_t) (nA + nB));
    for (int i = 0; i < nA; ++i) frames.push_back (makeFrame (bandA, chromaA));
    for (int i = 0; i < nB; ++i) frames.push_back (makeFrame (bandB, chromaB));
    return frames;
}

// Appends a linearly-RAMPED transition (over `rampLen` frames) from profile
// A to profile B onto `frames`. A ramp -- rather than an instantaneous
// single-frame step -- is used for peak-detection tests because smoothCurve
// is a box (moving-average) filter: an isolated single-frame delta smeared
// over a window `w` produces a flat trapezoid PLATEAU at its top (every
// in-window position sees the same one nonzero sample), which has no
// strictly-greater-than-both-neighbors point at all, so pickNoveltyPeaks
// (by design, per SECTIONS_SPEC.md PART 2's "local maxima" rule) can never
// fire on it -- this models a real section transition (which blends over
// roughly a bar in practice), not a pathological single-sample impulse.
// rampLen must be >= the smoothing window w for a genuine unique maximum to
// emerge (see this file's own scratch verification during authoring).
void appendRamp (std::vector<gent::FrameFeatures>& frames, int rampLen,
                  const float bandA[4], const float chromaA[12],
                  const float bandB[4], const float chromaB[12])
{
    for (int i = 0; i < rampLen; ++i)
    {
        const float t = (float) (i + 1) / (float) (rampLen + 1);
        float band[4], chroma[12];
        for (int k = 0; k < 4;  ++k) band[k]   = bandA[k]   * (1.0f - t) + bandB[k]   * t;
        for (int k = 0; k < 12; ++k) chroma[k] = chromaA[k] * (1.0f - t) + chromaB[k] * t;
        frames.push_back (makeFrame (band, chroma));
    }
}
}

// ---------------------------------------------------------------------------
// noveltyCurve: a timbre step (distinct band + chroma profile) produces a
// spike in combined/bandDist/chromaDist right at the step.
// ---------------------------------------------------------------------------
TEST_CASE ("NOVELTY curve: timbre step produces a spike at the boundary")
{
    const float bandA[4]   = { 1.0f, 0.2f, 0.1f, 0.05f };
    const float chromaA[12]= { 1,0,0,0,0,0,0,0,0,0,0,0 };
    const float bandB[4]   = { 0.05f, 0.1f, 0.3f, 1.0f };
    const float chromaB[12]= { 0,0,0,0,0,0,1,0,0,0,0,0 };

    const auto frames = makeStepSequence (200, 200, bandA, chromaA, bandB, chromaB);
    const auto curve = gent::noveltyCurve (frames);

    REQUIRE (curve.size() == frames.size());

    // Max element (by combined) lands within +-1 frame of index 200 (the step).
    size_t maxIdx = 0;
    float maxVal = -1.0f;
    for (size_t i = 0; i < curve.size(); ++i)
        if (curve[i].combined > maxVal) { maxVal = curve[i].combined; maxIdx = i; }

    CHECK (std::llabs ((long long) maxIdx - 200) <= 1);

    // bandDist and chromaDist both spike at (or within 1 of) the same index.
    size_t maxBandIdx = 0, maxChromaIdx = 0;
    float maxBand = -1.0f, maxChroma = -1.0f;
    for (size_t i = 0; i < curve.size(); ++i)
    {
        if (curve[i].bandDist > maxBand)   { maxBand = curve[i].bandDist; maxBandIdx = i; }
        if (curve[i].chromaDist > maxChroma) { maxChroma = curve[i].chromaDist; maxChromaIdx = i; }
    }
    CHECK (std::llabs ((long long) maxBandIdx - 200) <= 1);
    CHECK (std::llabs ((long long) maxChromaIdx - 200) <= 1);

    // Index 0 is always the 0-distance placeholder.
    CHECK (curve[0].combined == doctest::Approx (0.0f));
    CHECK (curve[0].bandDist == doctest::Approx (0.0f));
    CHECK (curve[0].chromaDist == doctest::Approx (0.0f));
}

TEST_CASE ("NOVELTY curve: empty/one-frame input -> empty/all-zero result")
{
    {
        const std::vector<gent::FrameFeatures> frames;
        const auto curve = gent::noveltyCurve (frames);
        CHECK (curve.empty());
    }
    {
        const float band[4] = { 1, 0, 0, 0 };
        const float chroma[12] = { 1,0,0,0,0,0,0,0,0,0,0,0 };
        std::vector<gent::FrameFeatures> frames { makeFrame (band, chroma) };
        const auto curve = gent::noveltyCurve (frames);
        REQUIRE (curve.size() == 1);
        CHECK (curve[0].combined == doctest::Approx (0.0f));
    }
}

TEST_CASE ("NOVELTY curve: silent frame vs silent frame -> distance 0, not NaN")
{
    const float zeroBand[4] = { 0, 0, 0, 0 };
    const float zeroChroma[12] = { 0,0,0,0,0,0,0,0,0,0,0,0 };
    std::vector<gent::FrameFeatures> frames
    {
        makeFrame (zeroBand, zeroChroma),
        makeFrame (zeroBand, zeroChroma),
        makeFrame (zeroBand, zeroChroma)
    };
    const auto curve = gent::noveltyCurve (frames);
    for (const auto& p : curve)
    {
        CHECK (p.combined == doctest::Approx (0.0f));
        CHECK (p.bandDist == doctest::Approx (0.0f));
        CHECK (p.chromaDist == doctest::Approx (0.0f));
        CHECK (! std::isnan (p.combined));
    }
}

// ---------------------------------------------------------------------------
// smoothCurve: window clamping.
// ---------------------------------------------------------------------------
TEST_CASE ("NOVELTY smoothCurve: w clamps to >=1 and even w forced odd")
{
    std::vector<gent::NoveltyPoint> curve
    {
        { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }
    };
    // w = 0 and negative both clamp to >= 1 (w|1 => 1) -> smoothed == raw combined.
    const auto s0 = gent::smoothCurve (curve, 0);
    const auto sNeg = gent::smoothCurve (curve, -4);
    REQUIRE (s0.size() == curve.size());
    for (size_t i = 0; i < curve.size(); ++i)
    {
        CHECK (s0[i] == doctest::Approx (curve[i].combined));
        CHECK (sNeg[i] == doctest::Approx (curve[i].combined));
    }

    // Even w (e.g. 4) forced to the next odd (5) via w|1.
    const auto s4 = gent::smoothCurve (curve, 4);
    const auto s5 = gent::smoothCurve (curve, 5);
    REQUIRE (s4.size() == s5.size());
    for (size_t i = 0; i < curve.size(); ++i)
        CHECK (s4[i] == doctest::Approx (s5[i]));
}

// ---------------------------------------------------------------------------
// Flat input: no local maxima above threshold -> pickNoveltyPeaks empty,
// noveltyBoundaries returns exactly {0}.
// ---------------------------------------------------------------------------
TEST_CASE ("NOVELTY flat input: no peaks, boundaries == {0}")
{
    const float band[4] = { 0.25f, 0.25f, 0.25f, 0.25f };
    const float chroma[12] = { 1,1,1,1,1,1,1,1,1,1,1,1 };
    std::vector<gent::FrameFeatures> frames;
    for (int i = 0; i < 500; ++i) frames.push_back (makeFrame (band, chroma));

    const auto curve = gent::noveltyCurve (frames);
    const auto smoothed = gent::smoothCurve (curve, 21);
    const auto peaks = gent::pickNoveltyPeaks (smoothed, gent::kNoveltyThresh.kMedium, 100);
    CHECK (peaks.empty());

    const auto boundaries = gent::noveltyBoundaries (frames, kFrameRate, kSamplesPerBar, kSampleRate, 1);
    REQUIRE (boundaries.size() == 1);
    CHECK (boundaries[0] == 0);
}

// ---------------------------------------------------------------------------
// Two steps closer than 1 bar -> exactly one boundary survives (the
// STRONGER one), plus the mandatory 0.
// ---------------------------------------------------------------------------
TEST_CASE ("NOVELTY two close steps (<1 bar apart): only the stronger boundary survives")
{
    const float bandA[4]    = { 1.0f, 0.1f, 0.05f, 0.02f };
    const float chromaA[12] = { 1,0,0,0,0,0,0,0,0,0,0,0 };
    // Weak transition: small change from A.
    const float bandWeak[4]    = { 0.9f, 0.15f, 0.05f, 0.02f };
    const float chromaWeak[12] = { 0.9f,0,0,0,0.1f,0,0,0,0,0,0,0 };
    // Strong transition: large change from weak-profile.
    const float bandStrong[4]    = { 0.02f, 0.05f, 0.15f, 1.0f };
    const float chromaStrong[12] = { 0,0,0,0,0,0,1,0,0,0,0,0 };

    const double framesPerBarD = kFrameRate * (kSamplesPerBar / kSampleRate);
    const int w = std::max (3, (int) std::round (gent::kNoveltyThresh.smoothBars * framesPerBarD));
    // Each ramp must be >= w frames wide to produce a genuine (non-plateau)
    // local maximum after box smoothing (see appendRamp's comment). Both
    // ramps together, plus a small separation, are still well under 1 bar
    // apart so the min-section-length rule must drop the weaker one.
    const int rampLen = w + 2;
    const int gapFrames = 2;   // negligible separation between the two ramps

    std::vector<gent::FrameFeatures> frames;
    for (int i = 0; i < 300; ++i) frames.push_back (makeFrame (bandA, chromaA));
    appendRamp (frames, rampLen, bandA, chromaA, bandWeak, chromaWeak);
    for (int i = 0; i < gapFrames; ++i) frames.push_back (makeFrame (bandWeak, chromaWeak));
    appendRamp (frames, rampLen, bandWeak, chromaWeak, bandStrong, chromaStrong);
    for (int i = 0; i < 300; ++i) frames.push_back (makeFrame (bandStrong, chromaStrong));

    const auto boundaries = gent::noveltyBoundaries (frames, kFrameRate, kSamplesPerBar, kSampleRate,
                                                       /*sensitivity many*/ 2);

    REQUIRE (boundaries.size() == 2);   // {0, one surviving boundary}
    CHECK (boundaries[0] == 0);

    // The surviving boundary should correspond to the STRONG transition
    // (centered around frame 300 + rampLen + gapFrames + rampLen/2), not the
    // weak one (centered around frame 300 + rampLen/2) -- assert the
    // survivor lands past the midpoint between the two transition centers.
    const double weakCenterFrame   = 300.0 + rampLen / 2.0;
    const double strongCenterFrame = 300.0 + rampLen + gapFrames + rampLen / 2.0;
    const double midFrame = (weakCenterFrame + strongCenterFrame) / 2.0;
    const double survivorFrame = (double) boundaries[1] / kSampleRate * kFrameRate;
    CHECK (survivorFrame > midFrame);
}

// ---------------------------------------------------------------------------
// Sensitivity ordering: boundaries(few) subset of boundaries(medium) subset
// of boundaries(many), for a sequence with steps of differing magnitude.
// ---------------------------------------------------------------------------
TEST_CASE ("NOVELTY sensitivity ordering: few subset medium subset many")
{
    const float bandBase[4]    = { 0.4f, 0.3f, 0.2f, 0.1f };
    const float chromaBase[12] = { 1,0,0,0,0,0,0,0,0,0,0,0 };

    // Three transitions of increasing magnitude, spaced far apart (>> 1 bar)
    // so min-gap dropping never interferes -- only the k threshold decides.
    // Magnitudes tuned (against this fixture's frameRate/samplesPerBar) so
    // the small transition clears none of the three k thresholds, the
    // medium transition clears kMany + kMedium but not kFew, and the big
    // transition clears all three -- producing a genuine 2/3/3 split.
    const float bandSmall[4]    = { 0.39f, 0.31f, 0.205f, 0.105f };   // tiny change
    const float chromaSmall[12] = { 0.99f,0,0,0,0.01f,0,0,0,0,0,0,0 };

    const float bandMed[4]    = { 0.12f, 0.43f, 0.32f, 0.13f };       // moderate change
    const float chromaMed[12] = { 0.55f,0,0,0,0,0,0.45f,0,0,0,0,0 };

    const float bandBig[4]    = { 0.02f, 0.05f, 0.15f, 0.78f };       // large change
    const float chromaBig[12] = { 0,0,0,0,0,0,1,0,0,0,0,0 };

    const double framesPerBarD = kFrameRate * (kSamplesPerBar / kSampleRate);
    const int w = std::max (3, (int) std::round (gent::kNoveltyThresh.smoothBars * framesPerBarD));
    const int rampLen = w + 2;   // wide enough for a genuine (non-plateau) peak

    std::vector<gent::FrameFeatures> frames;
    const int seg = 400;
    for (int i = 0; i < seg; ++i) frames.push_back (makeFrame (bandBase, chromaBase));
    appendRamp (frames, rampLen, bandBase, chromaBase, bandSmall, chromaSmall);
    for (int i = 0; i < seg; ++i) frames.push_back (makeFrame (bandSmall, chromaSmall));
    appendRamp (frames, rampLen, bandSmall, chromaSmall, bandMed, chromaMed);
    for (int i = 0; i < seg; ++i) frames.push_back (makeFrame (bandMed, chromaMed));
    appendRamp (frames, rampLen, bandMed, chromaMed, bandBig, chromaBig);
    for (int i = 0; i < seg; ++i) frames.push_back (makeFrame (bandBig, chromaBig));

    const auto few    = gent::noveltyBoundaries (frames, kFrameRate, kSamplesPerBar, kSampleRate, 0);
    const auto medium = gent::noveltyBoundaries (frames, kFrameRate, kSamplesPerBar, kSampleRate, 1);
    const auto many   = gent::noveltyBoundaries (frames, kFrameRate, kSamplesPerBar, kSampleRate, 2);

    // few subset medium: every boundary in `few` appears in `medium`.
    for (int b : few)
        CHECK (std::find (medium.begin(), medium.end(), b) != medium.end());
    // medium subset many.
    for (int b : medium)
        CHECK (std::find (many.begin(), many.end(), b) != many.end());

    // The tiers should actually differ in count (few <= medium <= many) so
    // this test is non-vacuous.
    CHECK (few.size() <= medium.size());
    CHECK (medium.size() <= many.size());
    CHECK (many.size() > few.size());
}

// ---------------------------------------------------------------------------
// Beat snap: a step placed off-beat -> returned boundary is an exact
// multiple of spb (samplesPerBar / 4).
// ---------------------------------------------------------------------------
TEST_CASE ("NOVELTY beat snap: boundary snaps to an exact beat multiple")
{
    const float bandA[4]    = { 1.0f, 0.1f, 0.05f, 0.02f };
    const float chromaA[12] = { 1,0,0,0,0,0,0,0,0,0,0,0 };
    const float bandB[4]    = { 0.02f, 0.05f, 0.1f, 1.0f };
    const float chromaB[12] = { 0,0,0,0,0,0,1,0,0,0,0,0 };

    const double framesPerBarD = kFrameRate * (kSamplesPerBar / kSampleRate);
    const int w = std::max (3, (int) std::round (gent::kNoveltyThresh.smoothBars * framesPerBarD));
    const int rampLen = w + 2;   // wide enough for a genuine (non-plateau) peak

    // Off-beat transition position: not aligned to any nice frame boundary.
    std::vector<gent::FrameFeatures> frames;
    for (int i = 0; i < 223; ++i) frames.push_back (makeFrame (bandA, chromaA));
    appendRamp (frames, rampLen, bandA, chromaA, bandB, chromaB);
    for (int i = 0; i < 401; ++i) frames.push_back (makeFrame (bandB, chromaB));
    const auto boundaries = gent::noveltyBoundaries (frames, kFrameRate, kSamplesPerBar, kSampleRate, 1);

    REQUIRE (boundaries.size() >= 2);
    const double spb = kSamplesPerBar / 4.0;
    for (size_t i = 1; i < boundaries.size(); ++i)
    {
        const double ratio = (double) boundaries[i] / spb;
        CHECK (std::abs (ratio - std::round (ratio)) < 1e-6);
    }
}

// ---------------------------------------------------------------------------
// Degenerates.
// ---------------------------------------------------------------------------
TEST_CASE ("NOVELTY degenerates: empty frames, one frame, zero rates, all-silent")
{
    {
        const std::vector<gent::FrameFeatures> frames;
        const auto b = gent::noveltyBoundaries (frames, kFrameRate, kSamplesPerBar, kSampleRate, 1);
        CHECK (b.empty());
    }
    {
        const float band[4] = { 1, 0, 0, 0 };
        const float chroma[12] = { 1,0,0,0,0,0,0,0,0,0,0,0 };
        std::vector<gent::FrameFeatures> frames { makeFrame (band, chroma) };
        const auto b = gent::noveltyBoundaries (frames, kFrameRate, kSamplesPerBar, kSampleRate, 1);
        REQUIRE (b.size() == 1);
        CHECK (b[0] == 0);
    }
    {
        const float band[4] = { 1, 0, 0, 0 };
        const float chroma[12] = { 1,0,0,0,0,0,0,0,0,0,0,0 };
        std::vector<gent::FrameFeatures> frames;
        for (int i = 0; i < 50; ++i) frames.push_back (makeFrame (band, chroma));

        // frameRate <= 0
        {
            const auto b = gent::noveltyBoundaries (frames, 0.0, kSamplesPerBar, kSampleRate, 1);
            REQUIRE (b.size() == 1);
            CHECK (b[0] == 0);
        }
        // samplesPerBar <= 0
        {
            const auto b = gent::noveltyBoundaries (frames, kFrameRate, 0.0, kSampleRate, 1);
            REQUIRE (b.size() == 1);
            CHECK (b[0] == 0);
        }
        // sampleRate <= 0
        {
            const auto b = gent::noveltyBoundaries (frames, kFrameRate, kSamplesPerBar, 0.0, 1);
            REQUIRE (b.size() == 1);
            CHECK (b[0] == 0);
        }
    }
    {
        // all-silent frames -> {0} only (flat/zero curve, no peaks).
        const float zeroBand[4] = { 0, 0, 0, 0 };
        const float zeroChroma[12] = { 0,0,0,0,0,0,0,0,0,0,0,0 };
        std::vector<gent::FrameFeatures> frames;
        for (int i = 0; i < 400; ++i) frames.push_back (makeFrame (zeroBand, zeroChroma));
        const auto b = gent::noveltyBoundaries (frames, kFrameRate, kSamplesPerBar, kSampleRate, 1);
        REQUIRE (b.size() == 1);
        CHECK (b[0] == 0);
    }
}

// ---------------------------------------------------------------------------
// pickNoveltyPeaks: degenerate (size<3, zero variance) -> empty.
// ---------------------------------------------------------------------------
TEST_CASE ("NOVELTY pickNoveltyPeaks: degenerate size<3 or zero-variance -> empty")
{
    CHECK (gent::pickNoveltyPeaks ({}, gent::kNoveltyThresh.kMedium, 10).empty());
    CHECK (gent::pickNoveltyPeaks ({ 0.5f }, gent::kNoveltyThresh.kMedium, 10).empty());
    CHECK (gent::pickNoveltyPeaks ({ 0.5f, 0.6f }, gent::kNoveltyThresh.kMedium, 10).empty());
    // Zero variance (all identical values), size >= 3.
    CHECK (gent::pickNoveltyPeaks ({ 0.3f, 0.3f, 0.3f, 0.3f, 0.3f }, gent::kNoveltyThresh.kMedium, 10).empty());
}
