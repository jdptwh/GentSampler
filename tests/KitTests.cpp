// KitTests.cpp — KIT Part A: gent::kitHits (hit isolation + time-order
// layout). See KIT_SPEC.md PART A "Pure core" for the full normative
// semantics this file pins. Mirrors tests/NoveltyTests.cpp's style.
//
// NAMED-CONSTANTS DISCIPLINE: every threshold used below is read off
// gent::kKitThresh (never a copied numeric literal), so retuning that table
// can't silently break these tests — same discipline as
// tests/NoveltyTests.cpp vs gent::kNoveltyThresh.
#include "doctest.h"
#include "EngineMath.h"

#include <cmath>

namespace
{
using Onsets = std::vector<std::pair<int, float>>;

// Fixture sample rate: 44100 Hz -> kKitThresh.minSpacingSec (30 ms) = 1323
// samples.
constexpr double kSampleRate = 44100.0;
}

// ---------------------------------------------------------------------------
// Completeness pin: a dense pair 50 ms apart (> the 30 ms min-spacing, well
// under the 80 ms gap that makes Analyzer's `slices` merge dense-break hits)
// survives as TWO hits at every sensitivity that passes both onsets'
// strength.
// ---------------------------------------------------------------------------
TEST_CASE ("KIT completeness: dense 50ms pair survives as TWO hits (contrast the 80ms `slices` gap)")
{
    const int spacing50ms = (int) (kSampleRate * 0.05);
    REQUIRE (spacing50ms > (int) (kSampleRate * gent::kKitThresh.minSpacingSec));   // 50ms > 30ms min-spacing
    REQUIRE (spacing50ms < (int) (kSampleRate * 0.08));                            // 50ms < the 80ms `slices` gap

    const float strongStrength = gent::kKitThresh.sFew + 0.1f;   // clears every sensitivity tier
    Onsets onsets {
        { 1000, strongStrength },
        { 1000 + spacing50ms, strongStrength }
    };

    for (int sens = 0; sens <= 2; ++sens)
    {
        const auto hits = gent::kitHits (onsets, kSampleRate, sens);
        REQUIRE (hits.size() == 2);
        CHECK (hits[0] == 1000);
        CHECK (hits[1] == 1000 + spacing50ms);
    }
}

// ---------------------------------------------------------------------------
// 15ms double-trigger collapses to the EARLIER position (never strength-
// based — the later onset is dropped even if it happens to be stronger).
// ---------------------------------------------------------------------------
TEST_CASE ("KIT double-trigger: 15ms pair collapses to the EARLIER onset, never strength-based")
{
    const int spacing15ms = (int) (kSampleRate * 0.015);
    REQUIRE (spacing15ms < (int) (kSampleRate * gent::kKitThresh.minSpacingSec));   // 15ms < 30ms min-spacing

    const float earlyStrength = gent::kKitThresh.sMany + 0.05f;
    const float laterStrongerStrength = earlyStrength + 0.3f;   // later onset is STRONGER
    Onsets onsets {
        { 2000, earlyStrength },
        { 2000 + spacing15ms, laterStrongerStrength }
    };

    const auto hits = gent::kitHits (onsets, kSampleRate, /*many*/ 2);
    REQUIRE (hits.size() == 1);
    CHECK (hits[0] == 2000);   // the EARLIER position wins despite the later onset's higher strength
}

// ---------------------------------------------------------------------------
// Sensitivity nesting: few subset medium subset many, for a mixed-strength
// onset list (strengths straddling all three thresholds), spaced far enough
// apart that min-spacing never interferes.
// ---------------------------------------------------------------------------
TEST_CASE ("KIT sensitivity nesting: few subset medium subset many")
{
    const int wideGap = (int) (kSampleRate * 0.5);   // 500ms — far past min-spacing

    Onsets onsets;
    // Strength below every tier -> never kept.
    onsets.push_back ({ 0 * wideGap, gent::kKitThresh.sMany - 0.05f });
    // Strength clears MANY only.
    onsets.push_back ({ 1 * wideGap, (gent::kKitThresh.sMany + gent::kKitThresh.sMedium) / 2.0f });
    // Strength clears MANY + MEDIUM, not FEW.
    onsets.push_back ({ 2 * wideGap, (gent::kKitThresh.sMedium + gent::kKitThresh.sFew) / 2.0f });
    // Strength clears all three.
    onsets.push_back ({ 3 * wideGap, gent::kKitThresh.sFew + 0.1f });

    const auto few    = gent::kitHits (onsets, kSampleRate, 0);
    const auto medium = gent::kitHits (onsets, kSampleRate, 1);
    const auto many   = gent::kitHits (onsets, kSampleRate, 2);

    for (int h : few)
        CHECK (std::find (medium.begin(), medium.end(), h) != medium.end());
    for (int h : medium)
        CHECK (std::find (many.begin(), many.end(), h) != many.end());

    CHECK (few.size() == 1);
    CHECK (medium.size() == 2);
    CHECK (many.size() == 3);
}

// ---------------------------------------------------------------------------
// Time-order preserved: onsets already ascending in -> hits stay ascending
// out, even after the min-spacing drop pass.
// ---------------------------------------------------------------------------
TEST_CASE ("KIT time-order: output stays ascending")
{
    const int wideGap = (int) (kSampleRate * 0.2);
    const float s = gent::kKitThresh.sFew + 0.1f;
    Onsets onsets { { 0, s }, { wideGap, s }, { 2 * wideGap, s }, { 3 * wideGap, s } };

    const auto hits = gent::kitHits (onsets, kSampleRate, 0);
    REQUIRE (hits.size() == 4);
    for (size_t i = 1; i < hits.size(); ++i)
        CHECK (hits[i] > hits[i - 1]);
}

// ---------------------------------------------------------------------------
// Mixed: some onsets below threshold drop, survivors keep their positions
// and order.
// ---------------------------------------------------------------------------
TEST_CASE ("KIT mixed: below-threshold onsets drop, survivors keep position + order")
{
    const int wideGap = (int) (kSampleRate * 0.2);
    const float above = gent::kKitThresh.sMedium + 0.1f;
    const float below = gent::kKitThresh.sMedium - 0.1f;

    Onsets onsets {
        { 0 * wideGap, above },
        { 1 * wideGap, below },   // drops at medium
        { 2 * wideGap, above },
        { 3 * wideGap, below },   // drops at medium
        { 4 * wideGap, above }
    };

    const auto hits = gent::kitHits (onsets, kSampleRate, /*medium*/ 1);
    REQUIRE (hits.size() == 3);
    CHECK (hits[0] == 0 * wideGap);
    CHECK (hits[1] == 2 * wideGap);
    CHECK (hits[2] == 4 * wideGap);
}

// ---------------------------------------------------------------------------
// Degenerates: empty onsets, sampleRate <= 0.
// ---------------------------------------------------------------------------
TEST_CASE ("KIT degenerates: empty onsets, zero/negative sampleRate -> empty")
{
    CHECK (gent::kitHits ({}, kSampleRate, 1).empty());

    Onsets onsets { { 0, gent::kKitThresh.sFew + 0.1f }, { 1000, gent::kKitThresh.sFew + 0.1f } };
    CHECK (gent::kitHits (onsets, 0.0, 1).empty());
    CHECK (gent::kitHits (onsets, -44100.0, 1).empty());
}
