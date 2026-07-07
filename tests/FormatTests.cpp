// ============================================================================
//  FormatTests.cpp — PHASE E2: gent::fmt formatValue system.
//  One case per row of the PHASE_E_POLISH_PASS.md E2.1 format table, pinned
//  to the doc's own examples, plus the unit-switch boundaries.
// ============================================================================

#include "vendor/doctest.h"
#include "../Source/EngineMath.h"

TEST_CASE ("E2 fmt: time — ms below 1000, then s with 1 decimal")
{
    CHECK (gent::fmt::timeMs (1.0)    == "1 ms");
    CHECK (gent::fmt::timeMs (80.0)   == "80 ms");
    CHECK (gent::fmt::timeMs (500.0)  == "500 ms");
    CHECK (gent::fmt::timeMs (999.0)  == "999 ms");
    CHECK (gent::fmt::timeMs (1000.0) == "1.0 s");
    CHECK (gent::fmt::timeMs (1200.0) == "1.2 s");
    CHECK (gent::fmt::timeMs (2000.0) == "2.0 s");
    CHECK (gent::fmt::timeMs (0.0)    == "0 ms");
}

TEST_CASE ("E2 fmt: frequency — Hz below 1000, then kHz with 1 decimal")
{
    CHECK (gent::fmt::hz (250.0)     == "250 Hz");
    CHECK (gent::fmt::hz (20.0)      == "20 Hz");
    CHECK (gent::fmt::hz (999.0)     == "999 Hz");
    CHECK (gent::fmt::hz (1000.0)    == "1.0 kHz");
    CHECK (gent::fmt::hz (2500.0)    == "2.5 kHz");
    CHECK (gent::fmt::hz (20000.0)   == "20.0 kHz");
}

TEST_CASE ("E2 fmt: semitones — signed integer + st")
{
    CHECK (gent::fmt::semitones (0.0)   == "0 st");
    CHECK (gent::fmt::semitones (-5.0)  == "-5 st");
    CHECK (gent::fmt::semitones (7.0)   == "+7 st");
    CHECK (gent::fmt::semitones (-0.4)  == "0 st");     // rounds toward 0 st
    CHECK (gent::fmt::semitones (11.6)  == "+12 st");
}

TEST_CASE ("E2 fmt: ratio/speed — 2 decimals + multiplication sign")
{
    CHECK (gent::fmt::ratio (1.0)   == "1.00\xC3\x97");
    CHECK (gent::fmt::ratio (0.82)  == "0.82\xC3\x97");
    CHECK (gent::fmt::ratio (1.685) == "1.69\xC3\x97"); // 2-dec rounding
}

TEST_CASE ("E2 fmt: normalized — 2 decimals")
{
    CHECK (gent::fmt::norm (0.0)   == "0.00");
    CHECK (gent::fmt::norm (0.1)   == "0.10");
    CHECK (gent::fmt::norm (1.0)   == "1.00");
    CHECK (gent::fmt::norm (0.005) == "0.01");
}

TEST_CASE ("E2 fmt: pan — L## / C / R##")
{
    CHECK (gent::fmt::pan (0.0)    == "C");
    CHECK (gent::fmt::pan (-0.25)  == "L25");
    CHECK (gent::fmt::pan (0.5)    == "R50");
    CHECK (gent::fmt::pan (-1.0)   == "L100");
    CHECK (gent::fmt::pan (1.0)    == "R100");
    CHECK (gent::fmt::pan (0.004)  == "C");             // sub-half-percent stays centred
}

TEST_CASE ("E2 fmt: BPM — 1 decimal")
{
    CHECK (gent::fmt::bpm (159.2)  == "159.2");
    CHECK (gent::fmt::bpm (130.0)  == "130.0");
    CHECK (gent::fmt::bpm (76.84)  == "76.8");
}
