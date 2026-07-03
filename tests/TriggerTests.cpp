// TriggerTests.cpp — T4: trigger/playback state transitions
// (gent::latchPressTurnsOff / gent::voiceGateFlag / gent::chokeSilences /
// gent::releaseApplies). See TEST_TARGET_TASK.md T4.
//
// Mode encoding: 0 gate, 1 one-shot, 2 latch. Voice state: 0 attack,
// 1 sustain, 2 release.
#include "doctest.h"
#include "EngineMath.h"

// ---------------------------------------------------------------------------
// T4.1 — latchPressTurnsOff
// ---------------------------------------------------------------------------
TEST_CASE ("T4.1 latchPressTurnsOff: tap-off / tap-on / keyboard-mode never toggles / non-latch never toggles")
{
    CHECK (gent::latchPressTurnsOff (2, false, true) == true);    // tap-off
    CHECK (gent::latchPressTurnsOff (2, false, false) == false);  // tap-on (nothing sounding yet)
    CHECK (gent::latchPressTurnsOff (2, true, true) == false);    // keyboard mode never latch-toggles

    for (int mode : { 0, 1 })
        for (bool kb : { false, true })
            for (bool sounding : { false, true })
                CHECK (gent::latchPressTurnsOff (mode, kb, sounding) == false);
}

// ---------------------------------------------------------------------------
// T4.2 — voiceGateFlag
// ---------------------------------------------------------------------------
TEST_CASE ("T4.2 voiceGateFlag: gate-mode true, one-shot/latch false, keyboard-mode always true")
{
    CHECK (gent::voiceGateFlag (0, false) == true);
    CHECK (gent::voiceGateFlag (1, false) == false);
    CHECK (gent::voiceGateFlag (2, false) == false);
    for (int mode : { 0, 1, 2, 99 })
        CHECK (gent::voiceGateFlag (mode, true) == true);
}

// ---------------------------------------------------------------------------
// T4.3 — releaseApplies: ONE-SHOT survives key-up
// ---------------------------------------------------------------------------
TEST_CASE ("T4.3 releaseApplies: onlyGate=true leaves a non-gate (one-shot/latch) voice playing")
{
    // mirrors handleNoteOff's onlyGate=true call (PluginProcessor.cpp) for a
    // one-shot/latch voice (vGate == false): must NOT release
    const bool active = true;
    const int pad = 5;
    CHECK (gent::releaseApplies (active, pad, -1, /*vGate*/ false, 0, pad, -1, false, /*onlyGate*/ true) == false);
}

// ---------------------------------------------------------------------------
// T4.4 — releaseApplies: quick-fade overrides an in-progress release (retrigger)
// ---------------------------------------------------------------------------
TEST_CASE ("T4.4 releaseApplies: quick fade overrides in-progress release; non-quick does not")
{
    const bool active = true;
    const int pad = 3, note = -1;

    // same-pad retrigger path: releaseVoices(pad, -1, /*quick*/true, /*onlyGate*/false)
    CHECK (gent::releaseApplies (active, pad, note, /*vGate*/ true, /*vState*/ 2, pad, -1, /*quick*/ true, false) == true);

    // non-quick on an already-releasing voice: filtered out
    CHECK (gent::releaseApplies (active, pad, note, /*vGate*/ true, /*vState*/ 2, pad, -1, /*quick*/ false, false) == false);
}

// ---------------------------------------------------------------------------
// T4.5 — chokeSilences: exhaustive over choke groups {0,1,2} x pads {0,1}
// ---------------------------------------------------------------------------
TEST_CASE ("T4.5 chokeSilences: zero-group / same-pad / already-releasing / group-match exhaustive")
{
    CHECK (gent::chokeSilences (0, 0, true, 0, 1, 0) == false);    // myChoke == 0 -> never chokes
    CHECK (gent::chokeSilences (1, 0, true, 0, 0, 1) == false);    // triggering pad itself -> false
    CHECK (gent::chokeSilences (1, 0, true, 2, 1, 1) == false);    // other voice already releasing -> false

    for (int group : { 0, 1, 2 })
    {
        for (int triggerPad : { 0, 1 })
        {
            for (int otherPad : { 0, 1 })
            {
                for (int otherGroup : { 0, 1, 2 })
                {
                    for (bool otherActive : { false, true })
                    {
                        for (int otherState : { 0, 1, 2 })
                        {
                            const bool expected = (group > 0) && otherActive && (otherState != 2)
                                                 && (otherPad >= 0) && (otherPad != triggerPad)
                                                 && (otherGroup == group);
                            CHECK (gent::chokeSilences (group, triggerPad, otherActive, otherState,
                                                        otherPad, otherGroup) == expected);
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// T4.6 — composition scenario tests: scripted press/release sequences per mode
// ---------------------------------------------------------------------------
namespace
{
// a minimal plain-struct voice model — no JUCE, no audio, just the fields the
// predicates need, scripted through a press/release sequence.
struct ScriptVoice
{
    bool active = false;
    int  pad = -1;
    int  note = -1;
    bool gate = false;
    int  state = 0;   // 0 attack, 1 sustain, 2 release
};

// press: mirrors startVoice's decision shape for a single-voice pad (no voice
// pool contention — enough to exercise the predicates faithfully)
void scriptedPress (ScriptVoice& v, int mode, bool kbMode, int pad, int note)
{
    const bool padSounding = v.active && v.pad == pad && v.state != 2;
    if (gent::latchPressTurnsOff (mode, kbMode, padSounding))
    {
        // release (quick=false, onlyGate=false): mirrors releaseVoices(pad,-1,false,false)
        if (gent::releaseApplies (v.active, v.pad, v.note, v.gate, v.state, pad, -1, false, false))
            v.state = 2;
        return;
    }
    // non-kb triggers quick-release any currently sounding voice on the pad first
    // (retrigger path), mirroring releaseVoices(pad,-1,true,false)
    if (! kbMode && gent::releaseApplies (v.active, v.pad, v.note, v.gate, v.state, pad, -1, true, false))
        v.state = 2;

    // then (re)trigger this voice
    v.active = true;
    v.pad = pad;
    v.note = note;
    v.gate = gent::voiceGateFlag (mode, kbMode);
    v.state = 0;
}

// release: mirrors handleNoteOff's release call shape
void scriptedRelease (ScriptVoice& v, int pad, int note, bool onlyGate)
{
    if (gent::releaseApplies (v.active, v.pad, v.note, v.gate, v.state, pad, note, false, onlyGate))
        v.state = 2;
}
}

TEST_CASE ("T4.6 GATE mode: sounds while held, releases on key-up")
{
    ScriptVoice v;
    scriptedPress (v, /*mode*/ 0, false, /*pad*/ 2, /*note*/ 40);
    CHECK (v.active == true);
    CHECK (v.state == 0);
    CHECK (v.gate == true);

    scriptedRelease (v, 2, 40, /*onlyGate*/ true);   // key-up: gate voices DO release
    CHECK (v.state == 2);
}

TEST_CASE ("T4.6 ONE-SHOT mode: plays to end, retrigger quick-fades the old voice")
{
    ScriptVoice v;
    scriptedPress (v, /*mode*/ 1, false, /*pad*/ 4, /*note*/ 41);
    CHECK (v.gate == false);

    // key-up must NOT release a one-shot voice (onlyGate=true skips non-gate voices)
    scriptedRelease (v, 4, 41, /*onlyGate*/ true);
    CHECK (v.state == 0);   // still playing

    // a retrigger (press again on the same pad) quick-fades the old voice via the
    // production shape's pre-trigger releaseVoices(pad,-1,true,false) call
    scriptedPress (v, /*mode*/ 1, false, /*pad*/ 4, /*note*/ 41);
    // after the scripted retrigger, the (single, reused) voice model is back to
    // a fresh attack — the quick-release-then-retrigger shape is what T4.4
    // verifies at the predicate level; here we confirm the sequence leaves the
    // voice sounding (new instance), not stuck in release.
    CHECK (v.state == 0);
    CHECK (v.active == true);
}

TEST_CASE ("T4.6 LATCH mode: press turns on, second press (while sounding) turns off")
{
    ScriptVoice v;
    scriptedPress (v, /*mode*/ 2, false, /*pad*/ 6, /*note*/ 42);
    CHECK (v.active == true);
    CHECK (v.state == 0);
    CHECK (v.gate == false);   // latch is not a gate voice

    // second press while sounding (state != 2): latch-toggles OFF
    scriptedPress (v, /*mode*/ 2, false, /*pad*/ 6, /*note*/ 42);
    CHECK (v.state == 2);   // released, not retriggered
}
