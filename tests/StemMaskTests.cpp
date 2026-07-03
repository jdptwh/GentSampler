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

#include <cstdint>

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
