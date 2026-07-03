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
    return (stored == 0 || stored == 1) ? stored : 0;
}

inline int resolveHeroView (int requested, bool stemsAvailable)
{
    return (requested == 1 && stemsAvailable) ? 1 : 0;
}

// ---------------------------------------------------------------------------
//  D5 — STEMS-placeholder CTA affordance ("SEPARATE STEMS to fill the map")
//  See docs/STEM_VIEW_MODEL.md SS6 matrix rows 2-4/6. `doStemJob()` runs on a
//  single serial worker thread (PluginProcessor.cpp's `wait(250)` loop); a
//  second `requestStemSeparation()` call while a job is already running (rows
//  3/4: `downloadingModels`/`separating` true) does not crash or interleave --
//  it silently sets `wantStems` again and gets picked up on the loop's NEXT
//  iteration, queuing a full duplicate re-run right after the current job
//  finishes. That queued-duplicate-job outcome is what this predicate exists
//  to prevent at the UI layer, matching the existing guard the plain
//  `sepStemsBtn` button already has (`sepStemsBtn.setEnabled(!busy)`,
//  PluginEditor.cpp ~1593) -- the wave's own placeholder CTA had no equivalent
//  guard before D5 (a real gap: it could double-fire where the button
//  couldn't). Pure OR of the two lifecycle busy-flags; `hasStems` is not part
//  of the decision (a job can be legitimately re-run against an already-
//  separated source, e.g. changing quality, whenever it is NOT already busy).
inline bool ctaEnabledFor (bool separating, bool downloadingModels)
{
    return ! (separating || downloadingModels);
}

// ---------------------------------------------------------------------------
//  D3 — stem lane geometry
//  See docs/STEM_VIEW_MODEL.md SS8/SS9. Extracted from the duplicated inline
//  formula in mouseDown (PluginEditor.h ~861-863, pre-D3 line 732) and its
//  mouseMove twin (pre-D3 line 943): `jlimit(0,5,(int)((e.y-bandTop)/(bandH/6.0)))`.
//  D3 keeps this extraction feeding the DORMANT band hit-test code only (its
//  band geometry collapses to zero in the retired composite band and the real
//  STEMS-view rewire is D4's job); behavior-identical to the pre-D3 inline
//  formula for every (bandTop, bandH) pair actually reachable in production --
//  both call sites guard `bandH > 0` before ever calling this (unchanged by
//  D3), so bandH<=0 is out of this function's contract (division-by-zero
//  territory the original inline formula never had to handle either, since it
//  lived inside the same `if (stemBandH > 0 && ...)` guard).
// ---------------------------------------------------------------------------

// From the pre-D3 band hit-test (PluginEditor.h mouseDown/mouseMove). `y` is
// the event's y in the SAME coordinate space as `bandTop`; `bandH` is the
// total band height (six lanes), REQUIRED > 0 (caller's contract, matching
// both production call sites' existing `stemBandH > 0` guard). Returns the
// 0-5 lane index, clamped exactly as the original jlimit(0,5,...) did for any
// y (including out-of-band y above/below the six-lane run).
inline int laneIndexAt (int y, int bandTop, int bandH)
{
    const double laneH = (double) bandH / 6.0;
    return std::clamp ((int) (((double) y - (double) bandTop) / laneH), 0, 5);
}

// ---------------------------------------------------------------------------
//  D4 — lane zone (mute / solo / wave) x-classification
//  See docs/STEM_VIEW_MODEL.md SS8 and REDESIGN_TASK_D.md's D4 ctest scope:
//  "extract gent::laneZoneAt (int x, int w, int labW, int soloW) -> {mute,
//  solo, wave} classification extracted from the [dormant] constants".
//
//  Extracted VERBATIM (same constants, same if-order) from the dormant band
//  hit-test in PluginEditor.h::mouseDown (pre-D4 lines 704-710):
//
//      if (stemBandH > 0 && e.y >= stemBandTop && e.y < stemBandTop + stemBandH)
//      {
//          const int lane = gent::laneIndexAt (e.y, stemBandTop, stemBandH);
//          if (e.x >= w - stemSoloW - 6)
//              p.setStemSoloed (lane, ! p.isStemSoloed (lane));
//          else if (e.x <= 4 + stemLabW + 6)
//              p.setStemMuted (lane, ! p.isStemMuted (lane));
//          ...
//      }
//
//  Boundary constants (verified against the cited lines above, unchanged by
//  this extraction):
//    - SOLO zone:  x >= w - soloW - 6   (dormant line 707: `e.x >= w - stemSoloW - 6`)
//    - MUTE zone:  x <= 4 + labW + 6    (dormant line 709: `e.x <= 4 + stemLabW + 6`)
//    - everything else: wave (the dormant code's implicit "neither if/else-if
//      matched" fallthrough, which still `return`s at the OLD whole-band level
//      -- D4's job is to make that a real "does nothing, falls through" case
//      instead of an unconditional return).
//
//  PRECEDENCE (degenerate small w, where the two zones overlap): the dormant
//  code is an `if / else if` with SOLO tested FIRST -- so when both conditions
//  are true simultaneously (possible whenever w is small enough that
//  `w - soloW - 6 <= 4 + labW + 6`), SOLO WINS. This function preserves that
//  exact precedence: the SOLO test is evaluated before the MUTE test, matching
//  the dormant `if (solo) ... else if (mute) ...` order line-for-line.
// ---------------------------------------------------------------------------

enum class LaneZone { mute, solo, wave };

inline LaneZone laneZoneAt (int x, int w, int labW, int soloW)
{
    if (x >= w - soloW - 6)
        return LaneZone::solo;
    if (x <= 4 + labW + 6)
        return LaneZone::mute;
    return LaneZone::wave;
}

} // namespace gent
