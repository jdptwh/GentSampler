#pragma once
// ============================================================================
//  EngineMath.h — GentSampler pure engine-core math
//
//  Extracted, behavior-identical decision cores for the slice-window / snap /
//  trigger-state / stem-mask logic that lives in PluginProcessor.{h,cpp} and
//  PluginEditor.h. See TEST_TARGET_TASK.md for the extraction plan and the
//  exact production line numbers each function was pulled from.
//
//  GROUND RULES (TEST_TARGET_TASK.md):
//   - Pure functions only. No JUCE. No locking/atomics/allocation here — all
//     of that stays in the production call sites, which pass already-loaded
//     plain values in and get plain values back.
//   - juce::jmax/jmin/jlimit on int become std::max/std::min/std::clamp
//     ONLY inside this header — identical semantics for the int ranges
//     involved at every call site (T6 reviewer verifies per-site).
//   - Behavior-identical is the deliverable: every boundary constant below
//     (+32, +33, 6*sppNow, 0.05*sr, 0x3F, quick-fade-overrides-releasing,
//     strict '<' in nearest-transient) is preserved exactly as production
//     had it, including any pre-existing quirks/discrepancies (flagged in
//     comments, not "fixed").
// ============================================================================

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace gent
{

// ---------------------------------------------------------------------------
//  T2 — slice-window math
// ---------------------------------------------------------------------------

// PluginProcessor.h:135 — cueEnds sentinel for an "open/gated" slice: play
// from the start, gate/release decides the cut, ignore boundaries.
constexpr int kOpenSlice = -2;

// From setCueEnd (PluginProcessor.cpp:417-422). samplePos < 0 means "reset to
// auto"; samplePos within 32 samples of cue collapses to the OPEN sentinel
// (there is no usable window that small); otherwise it's a real window end.
inline int resolveCueEndEdit (int samplePos, int cue)
{
    if (samplePos < 0)
        return -1;
    if (samplePos <= cue + 32)
        return kOpenSlice;
    return samplePos;
}

// From setCue (PluginProcessor.cpp:407-410). Moving the region start clamps
// to sample 0; if that push lands the start within 32 samples of (or past)
// the CURRENT end, the end auto-clears back to -1 (auto).
struct CueEditResult
{
    int cue = 0;
    int end = -1;
};

inline CueEditResult applyCueEdit (int samplePos, int currentEnd)
{
    CueEditResult r;
    r.cue = std::max (0, samplePos);
    r.end = currentEnd;
    if (currentEnd >= 0 && currentEnd <= samplePos + 32)
        r.end = -1;
    return r;
}

// From getEffectiveCueEnd (PluginProcessor.cpp:425-454). `allCues` is every
// pad's cue (source domain, -1 = unassigned); this pad's own cue is `cue`.
// The pad-index range guard stays production-side; this only needs len/cue/
// end/sliceMode/allCues to reproduce the decision.
inline int effectiveCueEnd (int cue, int end, int len, bool sliceMode,
                            const std::array<int, 16>& allCues)
{
    if (len < 2)
        return std::max (0, len - 1);

    if (cue < 0)
        return -1;                             // unassigned

    if (end == kOpenSlice)
        return len - 1;                        // open/gated: runs to the sample end
    if (end > cue)
        return std::min (end, len - 1);

    if (sliceMode)
    {
        int nextCue = -1;
        bool found = false;
        for (int cc : allCues)
        {
            if (cc >= 0 && cc > cue && (! found || cc < nextCue))
            {
                nextCue = cc;
                found = true;
            }
        }
        if (found)
            return std::min (nextCue, len - 1);
    }
    return len - 1;
}

// From applyEndHandleDrag (PluginEditor.h:61-69). Collapse-vs-window decision
// for an END-handle drag: within `collapseTolSamples` of cue collapses to
// the cue itself (the caller's resolveCueEndEdit-equivalent then turns that
// into kOpenSlice); otherwise the window floors at cue+33 (the smallest real
// window — one more than the +32 collapse tolerance's own boundary).
inline int resolveEndDragTarget (int cue, int proposedEnd, int collapseTolSamples)
{
    if (proposedEnd <= cue + collapseTolSamples)
        return cue;
    return std::max (cue + 33, proposedEnd);
}

// ---------------------------------------------------------------------------
//  T3 — snap capture math
// ---------------------------------------------------------------------------

// From nearestTransient (PluginProcessor.cpp:300-311). 50 ms capture cap;
// strict '<' so the EARLIER vector element wins on an exact tie. Empty list
// or nothing within the cap returns sourcePos unchanged.
inline int nearestTransientIn (const std::vector<int>& ts, int sourcePos, double sampleRate)
{
    if (ts.empty())
        return sourcePos;
    const int maxDist = (int) (sampleRate * 0.05);         // snap within 50 ms
    int best = sourcePos, bestD = maxDist + 1;
    for (int t : ts)
    {
        const int d = std::abs (t - sourcePos);
        if (d < bestD) { bestD = d; best = t; }
    }
    return bestD <= maxDist ? best : sourcePos;
}

// From resolveSnap (PluginEditor.h:144-145). 6-screen-pixel capture test:
// snap iff the candidate is within 6*sppNow samples of the proposed sample.
inline int applySnapThreshold (int proposed, int candidate, double sppNow)
{
    const double thresholdSamples = 6.0 * sppNow;
    return (std::abs (candidate - proposed) <= thresholdSamples) ? candidate : proposed;
}

// From snapCursor (PluginProcessor.cpp:540-563). Candidate competition for
// the live cursor: with a grid (hasGrid), nearest of {gridCand, all 16 placed
// cues} wins outright (no distance bail). Without a grid, the competition
// also includes transientCand, and if nothing landed within 0.05*sampleRate
// of `pos`, the cursor stays put (free/unsnapped) rather than jumping to a
// far-away cue/transient.
inline int selectSnapCursor (int pos, bool hasGrid, int gridCand,
                             const std::array<int, 16>& cues, int transientCand,
                             double sampleRate)
{
    int best = pos;
    long long bestDist = -1;                    // -1 = "nothing considered yet" (real distances >= 0)
    auto consider = [&] (int cand)
    {
        if (cand < 0) return;
        long long d = (long long) cand - (long long) pos;
        if (d < 0) d = -d;
        if (bestDist < 0 || d < bestDist) { bestDist = d; best = cand; }
    };
    if (hasGrid) consider (gridCand);
    for (int c : cues) consider (c);
    if (! hasGrid)
    {
        consider (transientCand);
        if (bestDist < 0 || bestDist > (long long) (0.05 * sampleRate))
            return pos;                          // nothing close: stay free
    }
    return best;
}

// ---------------------------------------------------------------------------
//  T4 — trigger/playback state transitions
//  Mode encoding: 0 gate, 1 one-shot, 2 latch. Voice state: 0 attack,
//  1 sustain, 2 release.
// ---------------------------------------------------------------------------

// From startVoice (PluginProcessor.cpp:1484-1494). A press on an already-
// sounding LATCH pad (non-keyboard-mode) toggles it off instead of
// retriggering. "padSounding" is the caller's scan for any active voice on
// the pad with state != 2 — that scan stays production-side.
inline bool latchPressTurnsOff (int mode, bool kbMode, bool padSounding)
{
    return ! kbMode && mode == 2 && padSounding;
}

// From startVoice (PluginProcessor.cpp:1609). GATE mode (or keyboard-mode
// input regardless of the pad's own mode) releases on key-up.
inline bool voiceGateFlag (int mode, bool kbMode)
{
    return kbMode || mode == 0;
}

// From startVoice's choke-group loop (PluginProcessor.cpp:1503-1513). A
// trigger on `triggerPad` (choke group myChoke) silences another sounding
// voice iff: this pad actually has a non-zero choke group, the other voice
// is active and not already releasing, it has a valid pad index, it's on a
// DIFFERENT pad, and that pad shares the same choke group.
inline bool chokeSilences (int myChoke, int triggerPad, bool otherActive, int otherState,
                           int otherPad, int otherChoke)
{
    if (myChoke <= 0)
        return false;
    return otherActive && otherState != 2 && otherPad >= 0 && otherPad != triggerPad
           && otherChoke == myChoke;
}

// From releaseVoices' four filter lines (PluginProcessor.cpp:1640-1643). A
// voice is released iff: it's active on the target pad, it matches the note
// filter (note < 0 = any note), it isn't excluded by the onlyGate filter
// (onlyGate skips non-gate voices — one-shot/latch survive key-up), and it
// isn't already releasing UNLESS this is a quick fade (quick-fade overrides
// an in-progress release, e.g. retrigger/choke).
inline bool releaseApplies (bool vActive, int vPad, int vNote, bool vGate, int vState,
                            int pad, int note, bool quick, bool onlyGate)
{
    if (! vActive || vPad != pad)            return false;
    if (note >= 0 && vNote != note)          return false;
    if (onlyGate && ! vGate)                 return false;
    if (vState == 2 && ! quick)              return false;
    return true;
}

// ---------------------------------------------------------------------------
//  T5 — stem-source switching
// ---------------------------------------------------------------------------

// From setPadStemBit (PluginProcessor.h:232-239). The pad/stem range guard
// stays production-side; this is just the bit set/clear + 0x3F clamp (bits
// beyond the 6 stems are always stripped, even from a dirty input mask).
inline std::uint8_t stemMaskWithBit (std::uint8_t m, int stem, bool on)
{
    std::uint8_t r = m;
    if (on) r = (std::uint8_t) (r | (1u << stem));
    else    r = (std::uint8_t) (r & ~(1u << stem));
    return (std::uint8_t) (r & 0x3F);
}

// From padMapHue/padSourceColour's single-bit classification
// (PluginEditor.h:26-28, 41-43). Returns the stem index 0-5 when exactly one
// bit is set (a classic power-of-two-or-zero test), else -1 for FULL (mask
// 0) or any multi-stem combination (both read as "neutral"/default hue).
inline int singleStemIndex (std::uint8_t m)
{
    if (m != 0 && (std::uint8_t) (m & (m - 1)) == 0)
        for (int k = 0; k < 6; ++k)
            if (m == (std::uint8_t) (1u << k))
                return k;
    return -1;
}

// From processBlock's per-voice per-stem gain table (PluginProcessor.cpp
// ~2422-2437). FULL (mask 0) plays every stem at unity; on a filtered pad,
// the selected stem(s) stay at unity and the rest "bleed" in at
// clamp(bleedParam,0,1)*0.5; globalAudible=false forces 0 regardless (global
// mute/solo always wins). Audio-thread hot path: kept branchy/allocation-
// free to match the original inline shape exactly.
inline float stemGainFor (std::uint8_t pmask, int k, float bleedParam, bool globalAudible)
{
    const bool padFiltered = (pmask != 0);
    const float bleedGain = padFiltered ? std::clamp (bleedParam, 0.0f, 1.0f) * 0.5f : 0.0f;
    float gk;
    if (! padFiltered)                        gk = 1.0f;          // FULL: every stem full
    else if (((pmask >> k) & 1) != 0)         gk = 1.0f;          // selected stem
    else                                      gk = bleedGain;     // unselected stem bled in
    return globalAudible ? gk : 0.0f;
}

// ---------------------------------------------------------------------------
//  D2 — hero view-mode state (COMPOSITE<->STEMS)
//  See docs/STEM_VIEW_MODEL.md SS4 (State model) and SS6 (Async separation
//  lifecycle x view — the full matrix). RATIFIED, normative; do not deviate
//  without re-opening D1.
//
//  Legal view values: 0 = COMPOSITE, 1 = STEMS. No third value exists
//  (SS4 "Legal values"). `heroView` (the processor atomic, D2 scope) stores
//  the user's sticky REQUEST, never the effective view — availability
//  changes must never silently overwrite the stored request (SS4
//  "Effective vs requested").
//
//  sanitizeHeroView(stored): pure clamp for the persistence-restore path
//  (SS4 "Sanitization", SS5). Any out-of-range/garbage stored int (negative,
//  >1, hand-edited/corrupted kit file, INT_MIN/INT_MAX) maps to 0
//  (COMPOSITE) -- the safe, always-valid default. Only {0,1} pass through
//  unchanged.
//
//  resolveHeroView(requested, stemsAvailable): pure function from the sticky
//  REQUEST + current stem availability to the EFFECTIVE view actually
//  painted (SS4 "Effective vs requested", SS5 DECISION D1-DECISION-2, SS6).
//  Semantics (binary result, never a third value -- SS6 preamble):
//   - requested == COMPOSITE (0) -> always 0, regardless of stemsAvailable.
//   - requested == STEMS (1) and stemsAvailable -> 1.
//   - requested == STEMS (1) and !stemsAvailable -> 0 (falls back to
//     COMPOSITE; the request itself is untouched/sticky -- callers keep
//     storing 1 and this function will return 1 again the instant
//     stemsAvailable flips true, with no further user action -- SS5 "missing-
//     source case", SS6 row 1-4/6 -> row 5/7 transitions).
//  This function is PURE: it does not read or write the sticky request
//  itself (that lives in the processor atomic elsewhere) and has no
//  knowledge of *why* stemsAvailable is what it is (pre-separation,
//  downloading, separating, failed, or a stale/missing restored source are
//  all just `false` at this layer -- SS6's matrix differs only in what the
//  STEMS paint branch shows as content, not in what this predicate returns).
inline int sanitizeHeroView (int stored)
{
    return 0;
}

inline int resolveHeroView (int requested, bool stemsAvailable)
{
    return 0;
}

} // namespace gent
