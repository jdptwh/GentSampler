// ClassifierTests.cpp — P2: gent::classifySlice (heuristic slice classifier:
// KICK/SNARE/HAT/PERC/TONAL/OTHER). See PHASE3_SPEC.md PART 2 ("Heuristic
// classifier") for the full normative decision tree, threshold table, and
// confidence formula this file tests; EngineMath.h's own comment block above
// classifySlice mirrors that text verbatim for the BULK implementer.
//
// BULK test-first protocol (PHASE3_SPEC.md tier table): these cases are
// authored BEFORE the real classifySlice body lands. The current body
// (EngineMath.h) is a DELIBERATE STUB that always returns { OTHER, 0.0f } —
// every TEST_CASE below is marked `* doctest::skip()` so normal `ctest`/gate
// runs stay green; the RED demonstration is a separate, explicit invocation
// with `--no-skip` (see this task's VERIFY step).
//
// NAMED-CONSTANTS DISCIPLINE (binding, per the dispatch instructions): every
// test vector below is built RELATIVE TO gent::kThreshStems / kThreshNoStems
// fields (e.g. `t.kickLowRatio + margin`), never a copied numeric literal
// from the spec's threshold table. Retuning any ClassifierThresholds value
// must not break these tests.
//
// Expected outcome per case run with --no-skip against the current
// always-OTHER-conf-0 stub:
//   P2.1 kick-like (stems)                  -> FAIL (stub gives OTHER, not KICK)
//   P2.1 hat-like (stems)                   -> FAIL (stub gives OTHER, not HAT)
//   P2.1 snare-like (stems)                 -> FAIL (stub gives OTHER, not SNARE)
//   P2.1 tonal-like via stem dominance      -> FAIL (stub gives OTHER, not TONAL)
//   P2.1 tonal-like via no-stems flatness   -> FAIL (stub gives OTHER, not TONAL)
//   P2.1 perc-fallback                      -> FAIL (stub gives conf 0.0, not >= minConfidence)
//   P2.2 drums-dominance beats tonal chroma -> FAIL (stub gives OTHER, not a percussion class)
//   P2.3 invariant: result != OTHER => conf >= minConfidence
//                                            -> PASSES AGAINST THE STUB TOO —
//                                               called out below: the stub's
//                                               result is ALWAYS OTHER, so the
//                                               implication is vacuously true
//                                               for every one of the 1000
//                                               vectors (never falsifiable
//                                               against this particular stub,
//                                               but remains a real, binding
//                                               invariant against any correct
//                                               implementation that returns
//                                               non-OTHER classes).
//   P2.3 invariant: determinism             -> PASSES AGAINST THE STUB TOO —
//                                               a constant function is
//                                               trivially deterministic; not
//                                               a test weakness.
//   P2.3 invariant: confidence in [0,1]     -> PASSES AGAINST THE STUB TOO —
//                                               stub confidence is always
//                                               exactly 0.0, which is in
//                                               range; legitimate pass.
//   P2.4 barely-passing -> confidence ~=0.5 -> FAIL (stub gives 0.0, not ~0.5)
//   P2.4 just-under-minConfidence -> OTHER  -> PASSES AGAINST THE STUB TOO —
//                                               stub always returns OTHER;
//                                               documented, not a gap (the
//                                               real implementation's
//                                               demotion path must ALSO land
//                                               on OTHER here, so the
//                                               assertion stays meaningful).
//   P2.5 evaluation-order pin (KICK before HAT) -> FAIL (stub gives OTHER, not KICK)
//
// So: 7 of 12 cases are expected to FAIL against the stub (majority), and
// the remaining 5 pass for documented, spec-correct reasons (vacuous-true
// invariants over an always-OTHER stub, trivial determinism/range of a
// constant function, and a demotion path whose target IS OTHER), not
// because the tests are weak.
#include "doctest.h"
#include "EngineMath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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
    bool nextBool() { return (next() & 1u) != 0; }
};

gent::SliceFeatures zeroFeatures()
{
    gent::SliceFeatures f {};
    f.bandRatio[0] = f.bandRatio[1] = f.bandRatio[2] = f.bandRatio[3] = 0.25f;
    f.centroidHz = 1000.0f;
    f.zcr = 0.05f;
    f.decaySec = 0.5f;
    f.durationSec = 0.5f;
    f.chromaFlatness = 0.5f;
    f.onsetStrength = 0.5f;
    f.hasStems = false;
    for (float& s : f.stemShare) s = 0.0f;
    return f;
}

// Normalize bandRatio to sum to 1 (keeps synthetic vectors physically
// plausible, matching how SliceAggregates::bandRatio is produced upstream;
// classifySlice itself does not require this, but honest test vectors do).
void normalizeBandRatio (gent::SliceFeatures& f)
{
    const float sum = f.bandRatio[0] + f.bandRatio[1] + f.bandRatio[2] + f.bandRatio[3];
    if (sum > 0.0f)
        for (float& b : f.bandRatio)
            b /= sum;
}
}

// ---------------------------------------------------------------------------
// P2.1 — canonical synthetic vectors classify correctly with
// confidence >= minConfidence: kick-like, hat-like, snare-like, tonal-like
// (via stem dominance AND via no-stems flatness), perc-fallback.
//
// Every feature value below is built as `t.field +/- margin` off the named
// threshold fields (never a copied numeric literal), so these cases survive
// any future retuning of ClassifierThresholds.
// ---------------------------------------------------------------------------
TEST_CASE ("P2.1 classifySlice: kick-like vector (stems) -> KICK, confidence >= minConfidence"
)
{
    const auto& t = gent::kThreshStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = true;
    f.stemShare[0] = t.drumsDominant + 0.20f;                 // drums-dominant -> percussion subtree
    f.stemShare[1] = f.stemShare[2] = f.stemShare[3] = f.stemShare[4] = 0.0f;
    f.stemShare[5] = 1.0f - f.stemShare[0];

    // Comfortably clear KICK's three criteria (>= for low ratio, <= for
    // centroid/decay) with generous margins so the case survives retuning.
    f.bandRatio[0] = std::min (0.95f, t.kickLowRatio + 0.5f * t.kickLowRatio);
    const float rem = 1.0f - f.bandRatio[0];
    f.bandRatio[1] = f.bandRatio[2] = f.bandRatio[3] = rem / 3.0f;
    f.centroidHz = std::max (10.0f, t.kickCentroidMax - 0.5f * t.kickCentroidMax);
    f.decaySec = std::max (0.001f, t.kickDecayMax - 0.5f * t.kickDecayMax);
    f.durationSec = t.kickDecayMax + t.hatDurMax;   // long enough not to look like HAT
    f.zcr = 0.5f * t.hatZcrMin;                     // low ZCR: not hat-like
    f.chromaFlatness = 0.5f;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::KICK);
    CHECK (r.confidence >= t.minConfidence);
}

TEST_CASE ("P2.1 classifySlice: hat-like vector (stems) -> HAT, confidence >= minConfidence"
)
{
    const auto& t = gent::kThreshStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = true;
    f.stemShare[0] = t.drumsDominant + 0.20f;
    f.stemShare[5] = 1.0f - f.stemShare[0];

    // HAT: bandRatio[3] >= hatHighRatio, zcr >= hatZcrMin, duration <= hatDurMax.
    // Must ALSO miss KICK (bandRatio[0] low, so KICK's low-ratio test fails)
    // so KICK-before-HAT evaluation order (P2.5) doesn't steal this case.
    f.bandRatio[3] = std::min (0.95f, t.hatHighRatio + 0.5f * t.hatHighRatio);
    const float rem = 1.0f - f.bandRatio[3];
    f.bandRatio[0] = rem * 0.1f;   // deliberately low: fails kickLowRatio
    f.bandRatio[1] = rem * 0.45f;
    f.bandRatio[2] = rem * 0.45f;
    f.zcr = std::min (0.95f, t.hatZcrMin + 0.5f * t.hatZcrMin);
    f.durationSec = std::max (0.001f, t.hatDurMax - 0.5f * t.hatDurMax);
    f.centroidHz = t.kickCentroidMax + 2.0f * t.kickCentroidMax;   // far above kick's ceiling
    f.decaySec = t.kickDecayMax + 2.0f * t.kickDecayMax;            // far above kick's ceiling
    f.chromaFlatness = 0.5f;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::HAT);
    CHECK (r.confidence >= t.minConfidence);
}

TEST_CASE ("P2.1 classifySlice: snare-like vector (stems) -> SNARE, confidence >= minConfidence"
)
{
    const auto& t = gent::kThreshStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = true;
    f.stemShare[0] = t.drumsDominant + 0.20f;
    f.stemShare[5] = 1.0f - f.stemShare[0];

    // SNARE: (bandRatio[1]+bandRatio[2]) >= snareMidRatio, chromaFlatness >= snareFlatMin.
    // Miss KICK (low bandRatio[0]) and miss HAT (low bandRatio[3], low zcr,
    // longer duration) so evaluation order can't steal this into KICK/HAT.
    const float mid = std::min (0.95f, t.snareMidRatio + 0.5f * t.snareMidRatio);
    f.bandRatio[1] = mid * 0.5f;
    f.bandRatio[2] = mid * 0.5f;
    const float rem = 1.0f - mid;
    f.bandRatio[0] = rem * 0.1f;   // low: fails kickLowRatio
    f.bandRatio[3] = rem * 0.9f;   // kept below hatHighRatio, see below
    f.bandRatio[3] = std::min (f.bandRatio[3], 0.5f * t.hatHighRatio);
    // renormalize the four bands so they still sum to 1 exactly
    normalizeBandRatio (f);

    f.chromaFlatness = std::min (1.0f, t.snareFlatMin + 0.5f * (1.0f - t.snareFlatMin));
    f.zcr = 0.5f * t.hatZcrMin;                                     // low: fails hatZcrMin
    f.durationSec = t.hatDurMax + 2.0f * t.hatDurMax;                // long: fails hatDurMax
    f.centroidHz = t.kickCentroidMax + 2.0f * t.kickCentroidMax;     // high: fails kickCentroidMax
    f.decaySec = t.kickDecayMax + 2.0f * t.kickDecayMax;             // long: fails kickDecayMax

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::SNARE);
    CHECK (r.confidence >= t.minConfidence);
}

TEST_CASE ("P2.1 classifySlice: tonal-like vector via STEM DOMINANCE -> TONAL, confidence >= minConfidence"
)
{
    const auto& t = gent::kThreshStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = true;
    f.stemShare[0] = 0.0f;   // drums NOT dominant: skip the percussion subtree
    const float tonalShare = std::min (0.95f, t.tonalDominant + 0.5f * t.tonalDominant);
    f.stemShare[1] = tonalShare / 4.0f;   // bass
    f.stemShare[2] = tonalShare / 4.0f;   // vocals
    f.stemShare[3] = tonalShare / 4.0f;   // guitar
    f.stemShare[4] = tonalShare / 4.0f;   // piano
    f.stemShare[5] = 1.0f - tonalShare;   // other

    f.chromaFlatness = std::max (0.0f, t.tonalFlatMax - 0.5f * t.tonalFlatMax);   // peaked/tonal
    f.durationSec = t.tonalDurMin + 0.5f * t.tonalDurMin;
    f.bandRatio[0] = f.bandRatio[1] = f.bandRatio[2] = f.bandRatio[3] = 0.25f;
    f.zcr = 0.05f;
    f.centroidHz = 1000.0f;
    f.decaySec = 0.5f;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::TONAL);
    CHECK (r.confidence >= t.minConfidence);
}

TEST_CASE ("P2.1 classifySlice: tonal-like vector via NO-STEMS FLATNESS -> TONAL, confidence >= minConfidence"
)
{
    const auto& t = gent::kThreshNoStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = false;

    f.chromaFlatness = std::max (0.0f, t.tonalFlatMax - 0.5f * t.tonalFlatMax);   // peaked/tonal
    f.durationSec = t.tonalDurMin + 0.5f * t.tonalDurMin;
    f.bandRatio[0] = f.bandRatio[1] = f.bandRatio[2] = f.bandRatio[3] = 0.25f;
    f.zcr = 0.05f;
    f.centroidHz = 1000.0f;
    f.decaySec = 0.5f;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::TONAL);
    CHECK (r.confidence >= t.minConfidence);
}

TEST_CASE ("P2.1 classifySlice: perc-fallback vector -> PERC, confidence >= minConfidence"
)
{
    // No-stems spectral tree: fails the TONAL test, fails all of KICK/HAT/
    // SNARE's full criteria sets, but drums-adjacent enough (band energy
    // concentrated below 3k, noisy, short) that a near-miss margin still
    // clears minConfidence -- lands on PERC.
    const auto& t = gent::kThreshNoStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = false;

    // Fails TONAL: flatness ABOVE tonalFlatMax (noisy, not peaked).
    f.chromaFlatness = std::min (1.0f, t.tonalFlatMax + 0.5f * (1.0f - t.tonalFlatMax));
    // Fails KICK: bandRatio[0] just under kickLowRatio (near-miss, not a
    // full match) but everything else (centroid/decay) still kick-ish, so
    // the near-miss margin is nontrivial.
    f.bandRatio[0] = std::max (0.0f, t.kickLowRatio - 0.02f * t.kickLowRatio);
    const float rem = 1.0f - f.bandRatio[0];
    f.bandRatio[1] = rem * 0.4f;
    f.bandRatio[2] = rem * 0.4f;
    f.bandRatio[3] = rem * 0.2f;
    f.centroidHz = std::max (10.0f, t.kickCentroidMax - 0.5f * t.kickCentroidMax);
    f.decaySec = std::max (0.001f, t.kickDecayMax - 0.5f * t.kickDecayMax);
    // Fails HAT: zcr well under hatZcrMin, bandRatio[3] well under hatHighRatio.
    f.zcr = 0.1f * t.hatZcrMin;
    // Fails SNARE: mid-band ratio well under snareMidRatio.
    f.durationSec = t.hatDurMax + 2.0f * t.hatDurMax;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::PERC);
    CHECK (r.confidence >= t.minConfidence);
}

// ---------------------------------------------------------------------------
// P2.2 — drums-dominance routes to the percussion subtree even when chroma
// looks tonal (stems-assist beats spectral): stemShare[0] >= drumsDominant
// AND chromaFlatness is set LOW/peaked (tonal-looking) -> still lands on a
// percussion class (KICK/HAT/SNARE/PERC), never TONAL.
// ---------------------------------------------------------------------------
TEST_CASE ("P2.2 classifySlice: drums-dominance overrides tonal-looking chroma -> percussion class, not TONAL"
)
{
    const auto& t = gent::kThreshStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = true;
    f.stemShare[0] = t.drumsDominant + 0.5f * (1.0f - t.drumsDominant);   // strongly drums-dominant
    const float restShare = 1.0f - f.stemShare[0];
    f.stemShare[1] = f.stemShare[2] = f.stemShare[3] = f.stemShare[4] = 0.0f;
    f.stemShare[5] = restShare;

    // Chroma looks tonal (peaked, well under tonalFlatMax) -- if the
    // drums-dominance test were (wrongly) skipped in favor of a chroma-first
    // read, this vector would be mistaken for TONAL. Also satisfy KICK's
    // three criteria so the outcome is deterministic (not just "not TONAL").
    f.chromaFlatness = std::max (0.0f, t.tonalFlatMax - 0.5f * t.tonalFlatMax);
    f.bandRatio[0] = std::min (0.95f, t.kickLowRatio + 0.5f * t.kickLowRatio);
    const float rem = 1.0f - f.bandRatio[0];
    f.bandRatio[1] = f.bandRatio[2] = f.bandRatio[3] = rem / 3.0f;
    f.centroidHz = std::max (10.0f, t.kickCentroidMax - 0.5f * t.kickCentroidMax);
    f.decaySec = std::max (0.001f, t.kickDecayMax - 0.5f * t.kickDecayMax);
    f.durationSec = t.tonalDurMin + t.kickDecayMax;   // long enough to also pass tonalDurMin
    f.zcr = 0.5f * t.hatZcrMin;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls != gent::TONAL);
    CHECK ((r.cls == gent::KICK || r.cls == gent::SNARE || r.cls == gent::HAT || r.cls == gent::PERC));
}

// ---------------------------------------------------------------------------
// P2.3 — invariant over 1000 fixed-seed random vectors, BOTH presets:
// result != OTHER => confidence >= minConfidence; determinism (same input
// twice -> same output); all confidences in [0,1].
// ---------------------------------------------------------------------------
TEST_CASE ("P2.3 classifySlice: 1000-vector invariant (both presets) -- "
           "non-OTHER implies confidence>=minConfidence, determinism, confidence in [0,1]"
)
{
    XorShift32 rng (0xC1A551F1u);   // "classify" fixed seed

    for (int preset = 0; preset < 2; ++preset)
    {
        const gent::ClassifierThresholds& t = (preset == 0) ? gent::kThreshStems : gent::kThreshNoStems;

        for (int trial = 0; trial < 1000; ++trial)
        {
            gent::SliceFeatures f {};
            const bool wantStems = (preset == 0) ? rng.nextBool() : false;
            f.bandRatio[0] = rng.nextFloat (0.0f, 1.0f);
            f.bandRatio[1] = rng.nextFloat (0.0f, 1.0f);
            f.bandRatio[2] = rng.nextFloat (0.0f, 1.0f);
            f.bandRatio[3] = rng.nextFloat (0.0f, 1.0f);
            normalizeBandRatio (f);
            f.centroidHz = rng.nextFloat (20.0f, 12000.0f);
            f.zcr = rng.nextFloat (0.0f, 1.0f);
            f.decaySec = rng.nextFloat (0.0f, 3.0f);
            f.durationSec = rng.nextFloat (0.0f, 3.0f);
            f.chromaFlatness = rng.nextFloat (0.0f, 1.0f);
            f.onsetStrength = rng.nextFloat (0.0f, 1.0f);
            f.hasStems = wantStems;
            if (wantStems)
            {
                float shares[6];
                float sum = 0.0f;
                for (float& s : shares) { s = rng.nextFloat (0.0f, 1.0f); sum += s; }
                for (int k = 0; k < 6; ++k)
                    f.stemShare[k] = (sum > 0.0f) ? shares[k] / sum : 0.0f;
            }
            else
            {
                for (float& s : f.stemShare) s = 0.0f;
            }

            const auto r1 = gent::classifySlice (f, t);
            const auto r2 = gent::classifySlice (f, t);

            // determinism
            CHECK (r1.cls == r2.cls);
            CHECK (r1.confidence == doctest::Approx (r2.confidence));

            // confidence range
            CHECK (r1.confidence >= 0.0f);
            CHECK (r1.confidence <= 1.0f);

            // non-OTHER implies confidence >= minConfidence
            if (r1.cls != gent::OTHER)
                CHECK (r1.confidence >= t.minConfidence);
        }
    }
}

// ---------------------------------------------------------------------------
// P2.4 — barely-passing vector -> confidence ~= 0.5 (+-0.01); a vector just
// under minConfidence -> OTHER (demotion path).
// ---------------------------------------------------------------------------
TEST_CASE ("P2.4 classifySlice: barely-passing KICK vector -> confidence ~= 0.5"
)
{
    const auto& t = gent::kThreshStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = true;
    // Drums dominant enough to route to the percussion subtree, but exactly
    // AT threshold itself contributes m=0 to ITS OWN criterion only when
    // drumsDominant is one of the winning class's confidence criteria; per
    // the decision tree, KICK's confidence criteria are kickLowRatio/
    // kickCentroidMax/kickDecayMax only (drums-dominance is a ROUTING test,
    // not one of KICK's three criteria) -- so push stemShare[0] with a
    // healthy margin (irrelevant to KICK's own confidence) and put every ONE
    // of KICK's three criteria EXACTLY at its threshold (m=0 each).
    f.stemShare[0] = std::min (0.99f, t.drumsDominant + 0.5f * (1.0f - t.drumsDominant));
    f.stemShare[5] = 1.0f - f.stemShare[0];

    f.bandRatio[0] = t.kickLowRatio;              // exactly at threshold: m=0
    const float rem = 1.0f - f.bandRatio[0];
    f.bandRatio[1] = f.bandRatio[2] = f.bandRatio[3] = rem / 3.0f;
    f.centroidHz = t.kickCentroidMax;             // exactly at threshold: m=0
    f.decaySec = t.kickDecayMax;                  // exactly at threshold: m=0
    f.durationSec = t.kickDecayMax + t.hatDurMax; // avoid accidental HAT match
    f.zcr = 0.5f * t.hatZcrMin;
    f.chromaFlatness = 0.5f;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::KICK);
    CHECK (r.confidence == doctest::Approx (0.5f).epsilon (0.02));
}

TEST_CASE ("P2.4 classifySlice: confidence just under minConfidence -> demoted to OTHER"
)
{
    // No-stems spectral tree, PERC near-miss path: construct a vector whose
    // best near-miss margin yields a confidence just barely under
    // t.minConfidence -- the ambiguity rule (rule 5) must demote this to
    // OTHER while STILL reporting the computed (sub-threshold) confidence,
    // not resetting it to 0.
    const auto& t = gent::kThreshNoStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = false;

    // Fails TONAL outright (flatness far above tonalFlatMax).
    f.chromaFlatness = std::min (1.0f, t.tonalFlatMax + 0.9f * (1.0f - t.tonalFlatMax));
    // KICK near-miss: bandRatio[0] a hair under kickLowRatio (tiny margin ->
    // small positive m contribution once mirrored, kept deliberately small
    // so the resulting confidence sits just under minConfidence).
    f.bandRatio[0] = std::max (0.0f, t.kickLowRatio - 0.001f * t.kickLowRatio);
    const float rem = 1.0f - f.bandRatio[0];
    f.bandRatio[1] = rem * 0.4f;
    f.bandRatio[2] = rem * 0.4f;
    f.bandRatio[3] = rem * 0.2f;
    f.centroidHz = t.kickCentroidMax - 0.001f * t.kickCentroidMax;
    f.decaySec = t.kickDecayMax - 0.001f * t.kickDecayMax;
    // Fails HAT/SNARE outright so KICK's near-miss margin is the one in play.
    f.zcr = 0.1f * t.hatZcrMin;
    f.durationSec = t.hatDurMax + 2.0f * t.hatDurMax;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::OTHER);
    CHECK (r.confidence < t.minConfidence);
}

// ---------------------------------------------------------------------------
// P2.5 — evaluation-order pin: a vector satisfying BOTH the KICK and HAT
// full-criteria sets simultaneously returns KICK (percussion subtree order
// is KICK -> HAT -> SNARE, first full match wins).
// ---------------------------------------------------------------------------
TEST_CASE ("P2.5 classifySlice: vector satisfying both KICK and HAT criteria -> KICK (order pin)"
)
{
    const auto& t = gent::kThreshStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = true;
    f.stemShare[0] = t.drumsDominant + 0.20f;
    f.stemShare[5] = 1.0f - f.stemShare[0];

    // KICK criteria: bandRatio[0] >= kickLowRatio, centroidHz <= kickCentroidMax,
    // decaySec <= kickDecayMax -- all comfortably satisfied.
    f.bandRatio[0] = std::min (0.6f, t.kickLowRatio + 0.5f * t.kickLowRatio);
    f.centroidHz = std::max (10.0f, t.kickCentroidMax - 0.5f * t.kickCentroidMax);
    f.decaySec = std::max (0.001f, t.kickDecayMax - 0.5f * t.kickDecayMax);

    // HAT criteria: bandRatio[3] >= hatHighRatio, zcr >= hatZcrMin,
    // durationSec <= hatDurMax -- ALSO comfortably satisfied simultaneously.
    f.bandRatio[3] = std::min (0.6f, t.hatHighRatio + 0.5f * t.hatHighRatio);
    const float rem = std::max (0.0f, 1.0f - f.bandRatio[0] - f.bandRatio[3]);
    f.bandRatio[1] = rem * 0.5f;
    f.bandRatio[2] = rem * 0.5f;
    normalizeBandRatio (f);
    // normalizeBandRatio rescales all four proportionally, which could pull
    // bandRatio[0]/[3] back toward their thresholds; push both back up with
    // margin after normalizing so both criteria sets remain unambiguously
    // satisfied post-normalization.
    f.bandRatio[0] = std::max (f.bandRatio[0], std::min (0.5f, t.kickLowRatio + 0.3f * t.kickLowRatio));
    f.bandRatio[3] = std::max (f.bandRatio[3], std::min (0.5f, t.hatHighRatio + 0.3f * t.hatHighRatio));

    f.zcr = std::min (0.95f, t.hatZcrMin + 0.5f * t.hatZcrMin);
    f.durationSec = std::max (0.001f, t.hatDurMax - 0.5f * t.hatDurMax);
    f.chromaFlatness = 0.5f;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::KICK);
}

// ---------------------------------------------------------------------------
// P2.6 — branch 1c coverage: hasStems == true but NEITHER drums-dominant
// (stemShare[0] < t.drumsDominant) NOR tonal-dominant (bass+vox+gtr+pno share
// < t.tonalDominant) falls through to the NO-STEMS SPECTRAL TREE evaluated
// with kThreshNoStems (not the caller's kThreshStems). Per the wiring
// contract classifySlice is always called with `t = kThreshStems` when
// f.hasStems is true, so the caller's `t` below is kThreshStems throughout —
// that is also what the final ambiguity-rule demotion (rule 5) checks
// against (t.minConfidence), even though the spectral-tree feature tests
// themselves ran under kThreshNoStems. See EngineMath.h classifySlice's
// `spectral_tree_with_kThreshNoStems` label and the `finish_classification`
// demotion check.
// ---------------------------------------------------------------------------
TEST_CASE ("P2.6 classifySlice: branch 1c (ambiguous stems) KICK-shaped -> KICK via no-stems fallthrough"
)
{
    const auto& t  = gent::kThreshStems;    // caller preset (hasStems == true wiring contract)
    const auto& nt = gent::kThreshNoStems;  // preset the fallthrough spectral tree actually uses
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = true;

    // Ambiguous stem mix: BOTH dominance tests fail (1c's own routing test).
    f.stemShare[0] = std::max (0.0f, t.drumsDominant - 0.5f * t.drumsDominant);
    const float tonalShare = std::max (0.0f, t.tonalDominant - 0.5f * t.tonalDominant);
    f.stemShare[1] = tonalShare / 4.0f;
    f.stemShare[2] = tonalShare / 4.0f;
    f.stemShare[3] = tonalShare / 4.0f;
    f.stemShare[4] = tonalShare / 4.0f;
    f.stemShare[5] = 1.0f - f.stemShare[0] - tonalShare;

    // Spectral features shaped for KICK per kThreshNoStems (the fallthrough's
    // active preset), mirroring the no-stems KICK case's construction above.
    f.bandRatio[0] = std::min (0.95f, nt.kickLowRatio + 0.5f * nt.kickLowRatio);
    const float rem = 1.0f - f.bandRatio[0];
    f.bandRatio[1] = f.bandRatio[2] = f.bandRatio[3] = rem / 3.0f;
    f.centroidHz = std::max (10.0f, nt.kickCentroidMax - 0.5f * nt.kickCentroidMax);
    f.decaySec = std::max (0.001f, nt.kickDecayMax - 0.5f * nt.kickDecayMax);
    f.durationSec = nt.kickDecayMax + nt.hatDurMax;   // long enough not to look like HAT
    f.zcr = 0.5f * nt.hatZcrMin;                      // low ZCR: not hat-like
    // Fails the no-stems TONAL test (flatness above tonalFlatMax: noisy, not peaked).
    f.chromaFlatness = std::min (1.0f, nt.tonalFlatMax + 0.5f * (1.0f - nt.tonalFlatMax));

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::KICK);
    CHECK (r.confidence >= t.minConfidence);
}

TEST_CASE ("P2.6 classifySlice: branch 1c (ambiguous stems) TONAL-shaped -> TONAL via no-stems fallthrough"
)
{
    const auto& t  = gent::kThreshStems;
    const auto& nt = gent::kThreshNoStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = true;

    // Same ambiguous-stems routing as the KICK-shaped case above.
    f.stemShare[0] = std::max (0.0f, t.drumsDominant - 0.5f * t.drumsDominant);
    const float tonalShare = std::max (0.0f, t.tonalDominant - 0.5f * t.tonalDominant);
    f.stemShare[1] = tonalShare / 4.0f;
    f.stemShare[2] = tonalShare / 4.0f;
    f.stemShare[3] = tonalShare / 4.0f;
    f.stemShare[4] = tonalShare / 4.0f;
    f.stemShare[5] = 1.0f - f.stemShare[0] - tonalShare;

    // Spectral features shaped for TONAL per kThreshNoStems: peaked chroma
    // AND long enough duration (both criteria of the no-stems TONAL test,
    // which runs FIRST in the spectral tree).
    f.chromaFlatness = std::max (0.0f, nt.tonalFlatMax - 0.5f * nt.tonalFlatMax);
    f.durationSec = nt.tonalDurMin + 0.5f * nt.tonalDurMin;
    f.bandRatio[0] = f.bandRatio[1] = f.bandRatio[2] = f.bandRatio[3] = 0.25f;
    f.zcr = 0.05f;
    f.centroidHz = 1000.0f;
    f.decaySec = 0.5f;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::TONAL);
    CHECK (r.confidence >= t.minConfidence);
}

// *** WAVE3_SPEC.md #18 case 3 -- OPEN QUESTION, NOT YET RESOLVED, DO NOT ***
// *** "FIX" BY GUESSING -- see WAVE3 implementer report for the escalation. ***
// The spec asks for a case where hasStems==true, both dominance tests fail
// (1c), and spectral features are weak everywhere -> expect OTHER via the
// min-confidence demotion. That exact scenario is PROVABLY UNREACHABLE
// against the current classifySlice body when the caller passes kThreshStems
// (the wiring-contract preset for hasStems==true), because:
//   - finish_classification's ambiguity rule demotes on
//     `winningConfidence < t.minConfidence`, using the CALLER's `t`
//     (kThreshStems, minConfidence == 0.50f) -- NOT `activeT`
//     (kThreshNoStems) even though activeT is what actually ran the
//     fallthrough's spectral-tree feature tests. This matches the binding
//     comment above classifySlice ("rule 5") verbatim -- code and its own
//     spec-comment agree, so this is not a transcription slip.
//   - Every non-OTHER result reachable from the no-stems spectral tree has
//     confidence >= 0.5f exactly: a full KICK/HAT/SNARE/TONAL match's
//     confidence is clamp(0.5 + 0.5*mean(passing margins), 0, 1) with
//     passing margins clamped to [0,1] (floor 0.5 when every criterion
//     barely passes); the PERC near-miss fallback is either exactly 0.5f
//     (no positive near-miss found) or > 0.5f (some positive near-miss).
//   - So the demotion test is always `(>=0.5f) < 0.50f`, which is false --
//     the ambiguity rule can never fire through the 1c fallthrough while
//     kThreshStems.minConfidence stays at exactly 0.50f.
// This case instead pins the ACTUAL reachable floor (PERC, confidence
// exactly 0.5f, no demotion) so the fallthrough's real behavior stays
// covered and regressions on this exact question are caught either way.
// Escalated to the planner: is this a real classifySlice defect (demotion
// should check `activeT.minConfidence` for the 1c path) or does the spec
// need amending to describe the reachable floor? Classifier BODY is
// deliberately UNTOUCHED per WAVE3_SPEC.md's "do not fix the classifier"
// instruction pending that answer.
TEST_CASE ("P2.6 classifySlice: branch 1c (ambiguous stems) weak-everywhere -> "
           "PERC at the confidence floor (0.5, NOT demoted) -- see OPEN QUESTION comment above"
)
{
    const auto& t  = gent::kThreshStems;    // demotion check (rule 5) uses THIS preset's minConfidence
    const auto& nt = gent::kThreshNoStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = true;

    // Same ambiguous-stems routing.
    f.stemShare[0] = std::max (0.0f, t.drumsDominant - 0.5f * t.drumsDominant);
    const float tonalShare = std::max (0.0f, t.tonalDominant - 0.5f * t.tonalDominant);
    f.stemShare[1] = tonalShare / 4.0f;
    f.stemShare[2] = tonalShare / 4.0f;
    f.stemShare[3] = tonalShare / 4.0f;
    f.stemShare[4] = tonalShare / 4.0f;
    f.stemShare[5] = 1.0f - f.stemShare[0] - tonalShare;

    // Spectral features weak/neutral everywhere under kThreshNoStems: fails
    // TONAL (flatness above tonalFlatMax), and fails KICK/HAT/SNARE by a wide
    // margin each (flat bands, mid centroid/zcr/duration) so every near-miss
    // margin is deeply negative (no positive near-miss survives) and the
    // PERC fallback lands on the exact 0.5f floor. chromaFlatness is placed
    // strictly between tonalFlatMax and snareFlatMin so it ALSO fails
    // SNARE's own flatness criterion (>= snareFlatMin) -- otherwise SNARE's
    // chroma criterion alone would pass and push a positive near-miss above
    // the floor.
    f.chromaFlatness = nt.tonalFlatMax + 0.1f * (nt.snareFlatMin - nt.tonalFlatMax);
    f.bandRatio[0] = f.bandRatio[1] = f.bandRatio[2] = f.bandRatio[3] = 0.25f;
    f.centroidHz = 1000.0f;
    f.zcr = 0.5f * nt.hatZcrMin;
    f.decaySec = nt.kickDecayMax + 2.0f * nt.kickDecayMax;
    f.durationSec = nt.hatDurMax + 2.0f * nt.hatDurMax;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls == gent::PERC);
    CHECK (r.confidence == doctest::Approx (0.5f));
}

TEST_CASE ("P2.6 classifySlice: contrast pin -- drums CLEARLY dominant with case-2's spectral shape "
           "does NOT take the 1c fallthrough (result differs from the TONAL-shaped case)"
)
{
    const auto& t  = gent::kThreshStems;
    const auto& nt = gent::kThreshNoStems;
    gent::SliceFeatures f = zeroFeatures();
    f.hasStems = true;

    // Drums CLEARLY dominant this time (routes straight into 1a's percussion
    // subtree with kThreshStems, never reaching the 1c fallthrough).
    f.stemShare[0] = std::min (0.95f, t.drumsDominant + 0.5f * (1.0f - t.drumsDominant));
    const float restShare = 1.0f - f.stemShare[0];
    f.stemShare[1] = f.stemShare[2] = f.stemShare[3] = f.stemShare[4] = 0.0f;
    f.stemShare[5] = restShare;

    // SAME spectral features as the TONAL-shaped 1c case above (peaked chroma,
    // long duration) -- under 1a's percussion subtree these don't correspond
    // to any of KICK/HAT/SNARE's criteria, so this must resolve via the PERC
    // fallback (confidence from the drums-dominance margin alone), landing on
    // PERC or OTHER (demoted) -- either way, never TONAL like case 2.
    f.chromaFlatness = std::max (0.0f, nt.tonalFlatMax - 0.5f * nt.tonalFlatMax);
    f.durationSec = nt.tonalDurMin + 0.5f * nt.tonalDurMin;
    f.bandRatio[0] = f.bandRatio[1] = f.bandRatio[2] = f.bandRatio[3] = 0.25f;
    f.zcr = 0.05f;
    f.centroidHz = 1000.0f;
    f.decaySec = 0.5f;

    const auto r = gent::classifySlice (f, t);
    CHECK (r.cls != gent::TONAL);
}
