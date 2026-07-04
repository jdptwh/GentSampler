// SectionTests.cpp — SECTIONS Part 1: gent::barSectionSlices (deterministic
// N-bar slicing). See SECTIONS_SPEC.md PART 1 for the normative semantics
// this file pins: 0-anchored cues at i*L, unassigned trailing pads = -1
// (NEVER clamped/piled at len-1, unlike sliceBeats — PluginProcessor.cpp:
// 583-596 — which clamps overflow pads to len-1; barSectionSlices must not).
#include "doctest.h"
#include "EngineMath.h"

// ---------------------------------------------------------------------------
// Shared fixture: spb = 22050 samples/bar (e.g. 120 BPM, 4/4, 44100 sr ->
// samplesPerBeat = 22050, bar = spb*4 = 88200; here we use samplesPerBar
// directly as the function's contract takes it pre-multiplied by 4 already
// done by the caller). len covers ~10 "bars" of 22050 samples = 220500.
// ---------------------------------------------------------------------------
static constexpr double kSpb = 22050.0;
static constexpr int kLen10Bars = 220500;   // exactly 10 * kSpb

TEST_CASE ("SECTIONS exact boundaries: N=1 bar per section")
{
    const auto r = gent::barSectionSlices (kLen10Bars, kSpb, 1);
    CHECK (r.sectionCount == 10);
    for (int i = 0; i < 10; ++i)
        CHECK (r.cue[(size_t) i] == (int) (i * kSpb));
    for (int i = 10; i < 16; ++i)
        CHECK (r.cue[(size_t) i] == -1);
}

TEST_CASE ("SECTIONS exact boundaries: N=2 bars per section")
{
    const auto r = gent::barSectionSlices (kLen10Bars, kSpb, 2);
    CHECK (r.sectionCount == 5);
    for (int i = 0; i < 5; ++i)
        CHECK (r.cue[(size_t) i] == (int) (i * 2.0 * kSpb));
    for (int i = 5; i < 16; ++i)
        CHECK (r.cue[(size_t) i] == -1);
}

TEST_CASE ("SECTIONS exact boundaries: N=4 bars per section")
{
    const auto r = gent::barSectionSlices (kLen10Bars, kSpb, 4);
    // ceil(220500 / 88200) = ceil(2.5) = 3
    CHECK (r.sectionCount == 3);
    for (int i = 0; i < 3; ++i)
        CHECK (r.cue[(size_t) i] == (int) (i * 4.0 * kSpb));
    for (int i = 3; i < 16; ++i)
        CHECK (r.cue[(size_t) i] == -1);
}

TEST_CASE ("SECTIONS exact boundaries: N=8 bars per section")
{
    const auto r = gent::barSectionSlices (kLen10Bars, kSpb, 8);
    // ceil(220500 / 176400) = ceil(1.25) = 2
    CHECK (r.sectionCount == 2);
    for (int i = 0; i < 2; ++i)
        CHECK (r.cue[(size_t) i] == (int) (i * 8.0 * kSpb));
    for (int i = 2; i < 16; ++i)
        CHECK (r.cue[(size_t) i] == -1);
}

TEST_CASE ("SECTIONS short source: trailing pads unassigned, NOT len-1 pile-up")
{
    // Only room for 3 whole 1-bar sections; sliceBeats (cpp:583-596) would
    // clamp every pad i>=3 to len-1 (piling 13 pads on the same sample).
    // barSectionSlices must instead leave pads 3..15 truly unassigned (-1).
    const int len = (int) (3.4 * kSpb);
    const auto r = gent::barSectionSlices (len, kSpb, 1);
    CHECK (r.sectionCount == 4);   // ceil(3.4) = 4
    CHECK (r.cue[0] == (int) (0 * kSpb));
    CHECK (r.cue[1] == (int) (1 * kSpb));
    CHECK (r.cue[2] == (int) (2 * kSpb));
    CHECK (r.cue[3] == (int) (3 * kSpb));
    for (int i = 4; i < 16; ++i)
        CHECK (r.cue[(size_t) i] == -1);   // unassigned, never len-1
}

TEST_CASE ("SECTIONS long source: 16 sections filled from start, sectionCount > 16")
{
    // 30 bars of 1-bar sections -> 30 sections, only first 16 pads assigned.
    const int len = (int) (30.0 * kSpb);
    const auto r = gent::barSectionSlices (len, kSpb, 1);
    CHECK (r.sectionCount == 30);
    CHECK (r.sectionCount > 16);
    for (int i = 0; i < 16; ++i)
        CHECK (r.cue[(size_t) i] == (int) (i * kSpb));
}

TEST_CASE ("SECTIONS degenerate inputs: all -1, sectionCount 0")
{
    {
        const auto r = gent::barSectionSlices (0, kSpb, 4);
        CHECK (r.sectionCount == 0);
        for (int i = 0; i < 16; ++i)
            CHECK (r.cue[(size_t) i] == -1);
    }
    {
        const auto r = gent::barSectionSlices (-100, kSpb, 4);
        CHECK (r.sectionCount == 0);
        for (int i = 0; i < 16; ++i)
            CHECK (r.cue[(size_t) i] == -1);
    }
    {
        const auto r = gent::barSectionSlices (kLen10Bars, 0.0, 4);
        CHECK (r.sectionCount == 0);
        for (int i = 0; i < 16; ++i)
            CHECK (r.cue[(size_t) i] == -1);
    }
    {
        const auto r = gent::barSectionSlices (kLen10Bars, -1.0, 4);
        CHECK (r.sectionCount == 0);
        for (int i = 0; i < 16; ++i)
            CHECK (r.cue[(size_t) i] == -1);
    }
    {
        const auto r = gent::barSectionSlices (kLen10Bars, kSpb, 0);
        CHECK (r.sectionCount == 0);
        for (int i = 0; i < 16; ++i)
            CHECK (r.cue[(size_t) i] == -1);
    }
    {
        const auto r = gent::barSectionSlices (kLen10Bars, kSpb, -4);
        CHECK (r.sectionCount == 0);
        for (int i = 0; i < 16; ++i)
            CHECK (r.cue[(size_t) i] == -1);
    }
}

// ---------------------------------------------------------------------------
// Property test: every assigned cue <= len-1, and cues are strictly
// increasing across assigned pads, over a range of (len, spb, bars) inputs.
// ---------------------------------------------------------------------------
TEST_CASE ("SECTIONS property: assigned cues <= len-1 and strictly increasing")
{
    const double spbValues[] = { 1000.0, 22050.0, 48000.0 };
    const int barsValues[] = { 1, 2, 3, 4, 8 };
    const int lenValues[] = { 1, 500, 12345, 220500, 5000000 };

    for (double spb : spbValues)
        for (int bars : barsValues)
            for (int len : lenValues)
            {
                const auto r = gent::barSectionSlices (len, spb, bars);
                int prev = -1;
                for (int i = 0; i < 16; ++i)
                {
                    const int c = r.cue[(size_t) i];
                    if (c == -1)
                        continue;
                    CHECK (c <= len - 1);
                    CHECK (c > prev);
                    prev = c;
                }
            }
}
