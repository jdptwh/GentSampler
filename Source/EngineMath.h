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
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
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

// Restore-path clamp for padStemMask (PREPACKAGE_AUDIT.md #7, WAVE2_SPEC.md).
// Every other writer of padStemMask goes through stemMaskWithBit above, whose
// contract strips bits beyond the 6 stems "even from a dirty input mask" --
// but the setStateInformation restore loop read the persisted "src<i>" value
// straight into padStemMask with no such clamp. This is the same defense
// applied one block below via sanitizeHeroView for heroView: any raw stored
// int (including a hand-edited/corrupted kit file) is masked down to the 6
// legal stem bits before it ever reaches gent::stemGainFor.
inline std::uint8_t sanitizeStemMask (int raw)
{
    return (std::uint8_t) (raw & 0x3F);
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

// ---------------------------------------------------------------------------
//  P1 — per-frame analysis features + slice-level aggregation
//  See PHASE3_SPEC.md PART 1 ("Analyzer changes" + "Pure aggregation").
//
//  FrameFeatures is filled inside Analyzer::analyze's existing flux FFT loop
//  (Source/Analysis.h, hop 512 at 1024-pt FFT; no second FFT pass) and stored
//  per-frame under `infoLock` alongside the existing onset list. Plain, JUCE-
//  free struct: 72 bytes/frame (4+1+1+12 floats), n/512 frames per source.
//
//  band[4]      — sum-of-squared-magnitude energy in 4 bins: <120 Hz,
//                 120-600 Hz, 600-3k Hz, >3k Hz (bin->Hz via b*sr/fftSize).
//  centroidHz   — magnitude-weighted spectral centroid for the frame, 0 for
//                 a silent frame.
//  zcr          — zero-crossing rate over the same 1024-sample time-domain
//                 window (crossings / window length).
//  chroma[12]   — 12-bin chroma folded from the same 1024-pt magnitudes,
//                 restricted to 55-1760 Hz (same fold as the analyzer's
//                 existing global key-detection chroma). Coarse below ~200 Hz
//                 (43 Hz bin spacing) — acceptable here since this chroma only
//                 feeds tonal-vs-noisy flatness, never key naming.
// ---------------------------------------------------------------------------
struct FrameFeatures
{
    float band[4];
    float centroidHz;
    float zcr;
    float chroma[12];
};

// PHASE3_SPEC.md PART 1, "Pure aggregation": first `kAttackWindowSec` seconds
// of a slice (or the whole slice if shorter) define the attack window that
// bandRatio/centroidHz/zcr are aggregated over.
constexpr float kAttackWindowSec = 0.12f;

// aggregateSliceFeatures's per-slice summary. All fields aggregated per the
// normative rules below (PHASE3_SPEC.md PART 1, verbatim):
//
//  - Attack window = first kAttackWindowSec seconds of the slice (or the
//    whole slice if the slice is shorter). bandRatio (normalized to sum 1;
//    an all-zero attack window -> equal 0.25 each), centroidHz (energy-
//    weighted mean over the attack window's frames), and zcr (mean over the
//    attack window's frames) are aggregated over the ATTACK WINDOW ONLY —
//    frames beyond the attack window never contribute to these three fields.
//    FRAME-COUNT PIN (gate ruling, non-integer boundaries): a frame belongs
//    to the attack window iff its START time is inside it, so
//    attackCount = clamp((int) std::ceil(kAttackWindowSec * frameRate),
//                        1, sliceFrameCount)
//    — CEIL, not floor (at 44100/512 ~ 86.13 fps, 0.12s = 10.335 frames ->
//    11: frame 10 starts at 0.1161s < 0.12s and is included).
//  - decaySec = time from the slice's peak-total-energy frame (max of
//    band[0]+band[1]+band[2]+band[3] across the whole slice) to the first
//    LATER frame whose total energy <= peak * 0.01 (i.e. -20 dB down from
//    peak); if no such frame exists before the slice ends, decaySec = the
//    remaining span from the peak frame to the slice end (i.e. "decays to
//    the slice end" is reported as decaying across the full remaining span,
//    not as "no decay").
//  - durationSec = the slice's full time span (endFrame - startFrame) /
//    frameRate, regardless of the attack window or decay computation.
//  - chromaFlatness = geometric-mean / arithmetic-mean of the SLICE-SUMMED
//    12-bin chroma (sum each of the 12 bins across every frame in the slice,
//    then take flatness of that 12-vector) — a value in (0,1]: 1.0 = flat/
//    noisy, values -> 0 = peaked/tonal. An empty or all-zero summed chroma
//    vector maps to 1.0 (treated as flat/noisy, not tonal, by convention).
//  - Degenerate ranges (empty frames vector; startFrame >= endFrame;
//    startFrame/endFrame out of [0, frames.size()) bounds after clamping
//    leaves nothing to aggregate) return a fully zeroed SliceAggregates with
//    durationSec = 0 — deterministic, no exceptions, no UB.
//
// See PHASE3_SPEC.md PART 1 for the full normative text this comment mirrors.
struct SliceAggregates
{
    float bandRatio[4];
    float centroidHz;
    float zcr;
    float decaySec;
    float durationSec;
    float chromaFlatness;
};

// STUB — deliberately wrong (always returns a zeroed struct). The BULK
// implementer replaces this body per the rules above; tests in
// tests/FeatureAggTests.cpp are authored against this stub first and must
// mostly FAIL against it (see that file's header comment for the expected
// pass/fail table). `frameRate` is frames-per-second (== sampleRate / hop);
// `startFrame`/`endFrame` are a half-open [startFrame, endFrame) frame-index
// range into `frames`.
inline SliceAggregates aggregateSliceFeatures (const std::vector<FrameFeatures>& frames,
                                               double frameRate, int startFrame, int endFrame)
{
    SliceAggregates result {};

    // Clamp startFrame/endFrame into valid range [0, frames.size()]
    const int sz = (int) frames.size();
    startFrame = std::clamp (startFrame, 0, sz);
    endFrame = std::clamp (endFrame, 0, sz);

    // Degenerate case: empty frames, startFrame >= endFrame, or clamped to nothing
    if (frames.empty() || startFrame >= endFrame)
        return result;  // already zeroed, durationSec = 0

    const int sliceFrameCount = endFrame - startFrame;

    // durationSec = (endFrame - startFrame) / frameRate
    result.durationSec = (float) (sliceFrameCount / frameRate);

    // Attack window: attackCount = clamp(ceil(kAttackWindowSec * frameRate), 1, sliceFrameCount)
    const int attackCount = std::clamp (
        (int) std::ceil (kAttackWindowSec * frameRate),
        1,
        sliceFrameCount
    );

    // --- bandRatio, centroidHz, zcr aggregation (attack window only) ---
    float bandSum[4] = {0.f, 0.f, 0.f, 0.f};
    float centroidEnergyWeightedSum = 0.f;
    float centroidEnergySum = 0.f;
    float zcrSum = 0.f;

    for (int i = 0; i < attackCount; ++i)
    {
        const FrameFeatures& frame = frames[startFrame + i];
        const float totalEnergy = frame.band[0] + frame.band[1] + frame.band[2] + frame.band[3];

        bandSum[0] += frame.band[0];
        bandSum[1] += frame.band[1];
        bandSum[2] += frame.band[2];
        bandSum[3] += frame.band[3];

        centroidEnergyWeightedSum += frame.centroidHz * totalEnergy;
        centroidEnergySum += totalEnergy;
        zcrSum += frame.zcr;
    }

    // Normalize bandRatio to sum to 1.0
    const float attackTotalEnergy = bandSum[0] + bandSum[1] + bandSum[2] + bandSum[3];
    if (attackTotalEnergy > 0.f)
    {
        result.bandRatio[0] = bandSum[0] / attackTotalEnergy;
        result.bandRatio[1] = bandSum[1] / attackTotalEnergy;
        result.bandRatio[2] = bandSum[2] / attackTotalEnergy;
        result.bandRatio[3] = bandSum[3] / attackTotalEnergy;
    }
    else
    {
        // All-zero attack window -> equal 0.25 each
        result.bandRatio[0] = result.bandRatio[1] = result.bandRatio[2] = result.bandRatio[3] = 0.25f;
    }

    // Energy-weighted centroid mean
    if (centroidEnergySum > 0.f)
        result.centroidHz = (float) (centroidEnergyWeightedSum / centroidEnergySum);
    else
        result.centroidHz = 0.f;

    // Mean ZCR
    result.zcr = zcrSum / (float) attackCount;

    // --- decaySec aggregation (whole slice) ---
    // Find peak-total-energy frame index
    int peakFrameIdx = 0;
    float peakTotalEnergy = 0.f;
    for (int i = 0; i < sliceFrameCount; ++i)
    {
        const FrameFeatures& frame = frames[startFrame + i];
        const float totalEnergy = frame.band[0] + frame.band[1] + frame.band[2] + frame.band[3];
        if (totalEnergy > peakTotalEnergy)
        {
            peakTotalEnergy = totalEnergy;
            peakFrameIdx = i;
        }
    }

    // Find first LATER frame where energy <= peak * 0.01
    const float decayThreshold = peakTotalEnergy * 0.01f;
    int decayFrameIdx = -1;
    for (int i = peakFrameIdx + 1; i < sliceFrameCount; ++i)
    {
        const FrameFeatures& frame = frames[startFrame + i];
        const float totalEnergy = frame.band[0] + frame.band[1] + frame.band[2] + frame.band[3];
        if (totalEnergy <= decayThreshold)
        {
            decayFrameIdx = i;
            break;
        }
    }

    if (decayFrameIdx >= 0)
        result.decaySec = (float) ((decayFrameIdx - peakFrameIdx) / frameRate);
    else
        result.decaySec = (float) ((sliceFrameCount - peakFrameIdx) / frameRate);

    // --- chromaFlatness aggregation (whole slice) ---
    float sliceSummedChroma[12] = {0.f};
    for (int i = 0; i < sliceFrameCount; ++i)
    {
        const FrameFeatures& frame = frames[startFrame + i];
        for (int j = 0; j < 12; ++j)
            sliceSummedChroma[j] += frame.chroma[j];
    }

    // Compute geometric mean / arithmetic mean
    float arithmeticMean = 0.f;
    bool allZero = true;
    for (int j = 0; j < 12; ++j)
    {
        arithmeticMean += sliceSummedChroma[j];
        if (sliceSummedChroma[j] > 0.f)
            allZero = false;
    }
    arithmeticMean /= 12.f;

    if (allZero || arithmeticMean <= 0.f)
    {
        // Empty or all-zero summed chroma -> 1.0
        result.chromaFlatness = 1.f;
    }
    else
    {
        // Geometric mean in the LOG domain (gate fix, post-BULK): the raw
        // 12-way product overflows float for production-scale chroma sums
        // (hundreds of frames x real magnitudes -> product > 1e38 -> inf),
        // breaking the (0,1] contract. exp(mean(log)) is overflow-immune.
        bool hasZero = false;
        double logSum = 0.0;
        for (int j = 0; j < 12; ++j)
        {
            if (sliceSummedChroma[j] <= 0.f)
            {
                hasZero = true;
                break;
            }
            logSum += std::log ((double) sliceSummedChroma[j]);
        }

        if (hasZero)
        {
            // Any zero bin means geometric mean = 0 -> maximally tonal/peaked
            result.chromaFlatness = 0.0f;
        }
        else
        {
            const double geometricMean = std::exp (logSum / 12.0);
            result.chromaFlatness = (float) (geometricMean / (double) arithmeticMean);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
//  P2 — heuristic slice classifier (KICK/SNARE/HAT/PERC/TONAL/OTHER)
//  See PHASE3_SPEC.md PART 2 ("Heuristic classifier") for the full normative
//  text this comment mirrors. classifySlice is pure and deterministic: same
//  input + same thresholds -> same output, always.
//
//  SliceFeatures = SliceAggregates + context (onsetStrength from the cached
//  transient-onset list; hasStems/stemShare from a per-slice per-stem energy
//  sum over StemSet buffers, done by the worker-thread caller — never here).
//
//  DECISION TREE (normative structure; numbers below are tuned at the Joe
//  gate via ClassifierThresholds ONLY — do not hard-code literals anywhere
//  that mirrors this table):
//
//   1. STEMS BRANCH (f.hasStems):
//        a. stemShare[0] (drums) >= t.drumsDominant
//             -> enter the PERCUSSION SUBTREE (below), using preset `t`
//                (kThreshStems when hasStems, per the wiring contract).
//        b. else if stemShare[1]+[2]+[3]+[4] (bass+vox+gtr+pno) >=
//             t.tonalDominant -> TONAL (dominance test + t.tonalFlatMax
//             criterion feeds the confidence margin, see below).
//        c. else -> fall through to the NO-STEMS SPECTRAL TREE, but
//             evaluated with kThreshNoStems (an ambiguous stem mix means
//             the stems signal isn't trustworthy here, so classification
//             widens to the no-stems thresholds even though f.hasStems is
//             true) — this is the one case where the caller-selected `t`
//             is NOT what decides the spectral-tree pass; classifySlice
//             itself must switch to kThreshNoStems for this fallthrough.
//
//   2. PERCUSSION SUBTREE (used both from 1a and from 3's spectral tree),
//      evaluated in this exact ORDER, first full match wins:
//        KICK:  bandRatio[0] >= t.kickLowRatio  AND centroidHz <= t.kickCentroidMax  AND decaySec <= t.kickDecayMax
//        HAT:   bandRatio[3] >= t.hatHighRatio  AND zcr >= t.hatZcrMin              AND durationSec <= t.hatDurMax
//        SNARE: (bandRatio[1]+bandRatio[2]) >= t.snareMidRatio AND chromaFlatness >= t.snareFlatMin
//        no match -> PERC, confidence = the drums-dominance margin alone
//                    (m of stemShare[0] vs t.drumsDominant; if this path was
//                    reached without a drums-dominance margin — i.e. via the
//                    no-stems spectral tree's own PERC fallback — use the
//                    highest near-miss margin among KICK/HAT/SNARE instead,
//                    per rule 3).
//
//   3. NO-STEMS SPECTRAL TREE (used when !f.hasStems, OR when 1c fell
//      through): TONAL test FIRST (chromaFlatness <= t.tonalFlatMax AND
//      durationSec >= t.tonalDurMin) -> TONAL; else the PERCUSSION SUBTREE
//      (KICK -> HAT -> SNARE); else PERC with confidence taken from
//      whichever of KICK/HAT/SNARE's near-miss margin is HIGHEST — if that
//      highest margin's resulting confidence is < t.minConfidence, the
//      result demotes to OTHER (rule 5).
//
//   4. CONFIDENCE (exact, test-pinned): for each criterion in the WINNING
//      class's test, compute a normalized margin
//          m = clamp((x - threshold) / max(threshold, 1e-6), 0, 1)   for >=-criteria
//          m = clamp((threshold - x) / max(threshold, 1e-6), 0, 1)   for <=-criteria (mirrored)
//      then confidence = clamp(0.5 + 0.5 * mean(m), 0, 1) — a criterion that
//      barely passes (x == threshold) contributes m=0, so a class whose every
//      criterion barely passes has confidence exactly 0.5; margins beyond the
//      threshold push confidence up toward 1.0, clamped.
//
//   5. AMBIGUITY RULE (binding): any winning class (KICK/SNARE/HAT/PERC/
//      TONAL) with confidence < t.minConfidence is DEMOTED to OTHER, keeping
//      the computed confidence value (OTHER's confidence is NOT reset to 0
//      or 1 — it carries the same number that triggered the demotion).
//      OTHER is never a wrong guess; a confident wrong guess is the failure
//      mode this rule exists to prevent.
// ---------------------------------------------------------------------------

enum SliceClass { KICK = 0, SNARE = 1, HAT = 2, PERC = 3, TONAL = 4, OTHER = 5 };

// = SliceAggregates + context. onsetStrength is 0..1 from the cached
// transientOnsets list (caller-supplied, not computed here). hasStems/
// stemShare come from a per-slice per-stem energy sum over StemSet buffers
// (order: drums, bass, vocals, guitar, piano, other — matching StemSet);
// stemShare sums to 1 when hasStems, all-zero when !hasStems.
struct SliceFeatures
{
    float bandRatio[4];
    float centroidHz;
    float zcr;
    float decaySec;
    float durationSec;
    float chromaFlatness;
    float onsetStrength;
    bool  hasStems;
    float stemShare[6];
};

struct ClassifyResult
{
    SliceClass cls;
    float confidence;
};

// ALL tunables live in this ONE struct (PHASE3_SPEC.md PART 2 threshold
// table) — the Joe gate tunes these values only; structure changes (new
// fields, new branches in classifySlice) require a spec amendment.
struct ClassifierThresholds
{
    float drumsDominant;     // stems only: drums stemShare >= -> percussion subtree
    float tonalDominant;     // stems only: bass+vox+gtr+pno share >= -> TONAL
    float kickLowRatio;      // bandRatio[0] >=
    float kickCentroidMax;   // centroidHz <=
    float kickDecayMax;      // decaySec <=
    float hatHighRatio;      // bandRatio[3] >=
    float hatZcrMin;         // zcr >=
    float hatDurMax;         // durationSec <=
    float snareMidRatio;     // bandRatio[1]+bandRatio[2] >=
    float snareFlatMin;      // chromaFlatness >= (noisy)
    float tonalFlatMax;      // chromaFlatness <= (peaked)
    float tonalDurMin;       // durationSec >=
    float minConfidence;     // below -> demote to OTHER
};

// Table column 1 (PHASE3_SPEC.md PART 2) — used when SliceFeatures::hasStems
// is true (the wiring contract: doClassifyJob selects this preset iff
// hasStems()). drumsDominant/tonalDominant are meaningful here.
constexpr ClassifierThresholds kThreshStems {
    /* drumsDominant   */ 0.55f,
    /* tonalDominant   */ 0.55f,
    /* kickLowRatio    */ 0.45f,
    /* kickCentroidMax */ 400.0f,
    /* kickDecayMax    */ 0.35f,
    /* hatHighRatio    */ 0.40f,
    /* hatZcrMin       */ 0.12f,
    /* hatDurMax       */ 0.35f,
    /* snareMidRatio   */ 0.50f,
    /* snareFlatMin    */ 0.55f,
    /* tonalFlatMax    */ 0.60f,
    /* tonalDurMin     */ 0.20f,
    /* minConfidence   */ 0.50f
};

// Table column 2 (PHASE3_SPEC.md PART 2) — used for the no-stems spectral
// tree, both when SliceFeatures::hasStems is false and when the stems branch
// falls through on an ambiguous stem mix (1c above). drumsDominant/
// tonalDominant have NO meaning in this column (the spec table lists "—" for
// both): set to 2.0f, a value no stemShare (which sums to <= 1) can ever
// reach, so the stems-only branches (1a/1b) can never trigger when this
// preset is in play — they simply can't fire since classifySlice only
// consults drumsDominant/tonalDominant when f.hasStems is true, and this
// preset is never paired with f.hasStems == true by the wiring contract
// (doClassifyJob picks kThreshStems for hasStems() sources); the sentinel is
// defensive belt-and-suspenders, not load-bearing.
constexpr ClassifierThresholds kThreshNoStems {
    /* drumsDominant   */ 2.0f,     // sentinel: never meaningful/reachable, see above
    /* tonalDominant   */ 2.0f,     // sentinel: never meaningful/reachable, see above
    /* kickLowRatio    */ 0.50f,
    /* kickCentroidMax */ 350.0f,
    /* kickDecayMax    */ 0.30f,
    /* hatHighRatio    */ 0.45f,
    /* hatZcrMin       */ 0.15f,
    /* hatDurMax       */ 0.30f,
    /* snareMidRatio   */ 0.55f,
    /* snareFlatMin    */ 0.60f,
    /* tonalFlatMax    */ 0.50f,
    /* tonalDurMin     */ 0.30f,
    /* minConfidence   */ 0.60f
};

// STUB — deliberately wrong (always OTHER, confidence 0). The BULK
// implementer replaces this body with the decision tree documented in the
// comment block above; tests in tests/ClassifierTests.cpp are authored
// against this stub first and must mostly FAIL against it (see that file's
// header comment for the expected pass/fail table). Legitimate passes
// against this stub are the ambiguity/OTHER-shaped cases and are called out
// there explicitly — not a sign of a weak test.
inline ClassifyResult classifySlice (const SliceFeatures& f, const ClassifierThresholds& t)
{
    // Helper lambda: compute normalized margin for a single criterion
    // Returns {passes: bool, margin: float}. Margin is only valid/meaningful if passes=true.
    auto computeMargin = [](float x, float threshold, bool isGreaterEqual) -> std::pair<bool, float>
    {
        bool passes;
        float m;
        const float divisor = std::max (threshold, 1e-6f);
        if (isGreaterEqual)
        {
            passes = (x >= threshold);
            m = (x - threshold) / divisor;
        }
        else
        {
            passes = (x <= threshold);
            m = (threshold - x) / divisor;
        }
        // Clamp a PASSING criterion's margin to [0,1]. A FAILING criterion
        // keeps its (negative) raw margin on purpose: it only ever feeds the
        // PERC/OTHER near-miss ranking, where a negative pushes near-miss
        // confidence DOWN — the conservative "ambiguous -> OTHER, never a
        // confident wrong guess" behavior the spec intends and P2a's demotion
        // test (ClassifierTests.cpp:455) pins. Winning classes have all
        // criteria passing, so this matches the spec's clamp-every-margin
        // formula exactly for them. (Near-miss margin clamping is the one
        // point PHASE3_SPEC PART 2 leaves underspecified — flagged for R1/Joe;
        // the conservative reading stands unless the ear-gate says otherwise.)
        if (passes)
            m = std::clamp (m, 0.0f, 1.0f);
        return {passes, m};
    };

    // Helper lambda: compute confidence from a list of criterion margins
    auto computeConfidence = [](const std::vector<float>& margins) -> float
    {
        if (margins.empty()) return 0.5f;
        float sum = 0.0f;
        for (float m : margins) sum += m;
        float mean = sum / (float) margins.size();
        return std::clamp (0.5f + 0.5f * mean, 0.0f, 1.0f);
    };

    // Track the winning class and its confidence
    SliceClass winningClass = OTHER;
    float winningConfidence = 0.0f;

    // Track best near-miss margin for PERC fallback in spectral tree
    float bestNearMissMargin = 0.0f;
    bool hasNearMiss = false;

    if (f.hasStems)
    {
        // STEMS BRANCH
        if (f.stemShare[0] >= t.drumsDominant)
        {
            // Enter percussion subtree with kThreshStems (which is `t` here)
            // Try KICK
            {
                auto [p0, m0] = computeMargin (f.bandRatio[0], t.kickLowRatio, true);
                auto [p1, m1] = computeMargin (f.centroidHz, t.kickCentroidMax, false);
                auto [p2, m2] = computeMargin (f.decaySec, t.kickDecayMax, false);
                if (p0 && p1 && p2)
                {
                    std::vector<float> margins = {m0, m1, m2};
                    winningClass = KICK;
                    winningConfidence = computeConfidence (margins);
                    goto finish_classification;
                }
                // Track as near-miss: mean of all margins
                float nearMissM = (m0 + m1 + m2) / 3.0f;
                if (nearMissM > bestNearMissMargin)
                {
                    bestNearMissMargin = nearMissM;
                    hasNearMiss = true;
                }
            }

            // Try HAT
            {
                auto [p0, m0] = computeMargin (f.bandRatio[3], t.hatHighRatio, true);
                auto [p1, m1] = computeMargin (f.zcr, t.hatZcrMin, true);
                auto [p2, m2] = computeMargin (f.durationSec, t.hatDurMax, false);
                if (p0 && p1 && p2)
                {
                    std::vector<float> margins = {m0, m1, m2};
                    winningClass = HAT;
                    winningConfidence = computeConfidence (margins);
                    goto finish_classification;
                }
                // Track as near-miss: mean of all margins
                float nearMissM = (m0 + m1 + m2) / 3.0f;
                if (nearMissM > bestNearMissMargin)
                {
                    bestNearMissMargin = nearMissM;
                    hasNearMiss = true;
                }
            }

            // Try SNARE
            {
                float midSum = f.bandRatio[1] + f.bandRatio[2];
                auto [p0, m0] = computeMargin (midSum, t.snareMidRatio, true);
                auto [p1, m1] = computeMargin (f.chromaFlatness, t.snareFlatMin, true);
                if (p0 && p1)
                {
                    std::vector<float> margins = {m0, m1};
                    winningClass = SNARE;
                    winningConfidence = computeConfidence (margins);
                    goto finish_classification;
                }
                // Track as near-miss: mean of all margins
                float nearMissM = (m0 + m1) / 2.0f;
                if (nearMissM > bestNearMissMargin)
                {
                    bestNearMissMargin = nearMissM;
                    hasNearMiss = true;
                }
            }

            // Percussion subtree fallback: PERC
            // Confidence = drums-dominance margin
            {
                auto [p, drumsDomMargin] = computeMargin (f.stemShare[0], t.drumsDominant, true);
                winningClass = PERC;
                winningConfidence = computeConfidence ({drumsDomMargin});
                goto finish_classification;
            }
        }
        else if ((f.stemShare[1] + f.stemShare[2] + f.stemShare[3] + f.stemShare[4]) >= t.tonalDominant)
        {
            // TONAL via dominance (both criteria feed confidence margin)
            auto [p0, tonalShareMargin] = computeMargin (
                f.stemShare[1] + f.stemShare[2] + f.stemShare[3] + f.stemShare[4],
                t.tonalDominant, true);
            auto [p1, flatnessMargin] = computeMargin (f.chromaFlatness, t.tonalFlatMax, false);
            // We enter this branch on dominance alone, but include flatness in confidence
            std::vector<float> margins = {tonalShareMargin, flatnessMargin};
            winningClass = TONAL;
            winningConfidence = computeConfidence (margins);
            goto finish_classification;
        }
        else
        {
            // Fall through to NO-STEMS SPECTRAL TREE with kThreshNoStems
            goto spectral_tree_with_kThreshNoStems;
        }
    }
    else
    {
        // NO STEMS SPECTRAL TREE (hasStems false)
spectral_tree_with_kThreshNoStems:
        const ClassifierThresholds& activeT = f.hasStems ? kThreshNoStems : t;

        // TONAL test FIRST (both criteria must pass)
        {
            auto [p0, m0] = computeMargin (f.chromaFlatness, activeT.tonalFlatMax, false);
            auto [p1, m1] = computeMargin (f.durationSec, activeT.tonalDurMin, true);
            if (p0 && p1)
            {
                std::vector<float> margins = {m0, m1};
                winningClass = TONAL;
                winningConfidence = computeConfidence (margins);
                goto finish_classification;
            }
        }

        // Try KICK
        {
            auto [p0, m0] = computeMargin (f.bandRatio[0], activeT.kickLowRatio, true);
            auto [p1, m1] = computeMargin (f.centroidHz, activeT.kickCentroidMax, false);
            auto [p2, m2] = computeMargin (f.decaySec, activeT.kickDecayMax, false);
            if (p0 && p1 && p2)
            {
                std::vector<float> margins = {m0, m1, m2};
                winningClass = KICK;
                winningConfidence = computeConfidence (margins);
                goto finish_classification;
            }
            // Track as near-miss: mean of all margins (treating failed criteria as m=0)
            float nearMissM = (m0 + m1 + m2) / 3.0f;
            if (nearMissM > bestNearMissMargin)
            {
                bestNearMissMargin = nearMissM;
                hasNearMiss = true;
            }
        }

        // Try HAT
        {
            auto [p0, m0] = computeMargin (f.bandRatio[3], activeT.hatHighRatio, true);
            auto [p1, m1] = computeMargin (f.zcr, activeT.hatZcrMin, true);
            auto [p2, m2] = computeMargin (f.durationSec, activeT.hatDurMax, false);
            if (p0 && p1 && p2)
            {
                std::vector<float> margins = {m0, m1, m2};
                winningClass = HAT;
                winningConfidence = computeConfidence (margins);
                goto finish_classification;
            }
            // Track as near-miss: mean of all margins
            float nearMissM = (m0 + m1 + m2) / 3.0f;
            if (nearMissM > bestNearMissMargin)
            {
                bestNearMissMargin = nearMissM;
                hasNearMiss = true;
            }
        }

        // Try SNARE
        {
            float midSum = f.bandRatio[1] + f.bandRatio[2];
            auto [p0, m0] = computeMargin (midSum, activeT.snareMidRatio, true);
            auto [p1, m1] = computeMargin (f.chromaFlatness, activeT.snareFlatMin, true);
            if (p0 && p1)
            {
                std::vector<float> margins = {m0, m1};
                winningClass = SNARE;
                winningConfidence = computeConfidence (margins);
                goto finish_classification;
            }
            // Track as near-miss: mean of all margins
            float nearMissM = (m0 + m1) / 2.0f;
            if (nearMissM > bestNearMissMargin)
            {
                bestNearMissMargin = nearMissM;
                hasNearMiss = true;
            }
        }

        // PERC fallback with highest near-miss margin
        if (hasNearMiss)
        {
            winningClass = PERC;
            winningConfidence = computeConfidence ({bestNearMissMargin});
        }
        else
        {
            winningClass = PERC;
            winningConfidence = 0.5f;
        }
    }

finish_classification:
    // Apply ambiguity rule: if confidence < minConfidence, demote to OTHER
    // but keep the computed confidence
    if (winningClass != OTHER && winningConfidence < t.minConfidence)
    {
        return {OTHER, winningConfidence};
    }

    return {winningClass, winningConfidence};
}

// ---------------------------------------------------------------------------
//  Self-drop guard — reject drops of our own temp exports (filesDropped)
// ---------------------------------------------------------------------------
// Normalize an absolute path for a case-insensitive, separator-agnostic
// compare: backslashes -> forward slashes, ASCII lowercase, and strip a single
// trailing slash. The CALLER must pass already-canonicalized absolute paths
// (production uses juce::File::getFullPathName()); this header stays JUCE-free
// and does no filesystem access, so it's unit-testable in the logic-only rig.
inline std::string normPathForCompare (std::string s)
{
    for (auto& c : s)
    {
        if (c == '\\')
            c = '/';
        else
            c = (char) std::tolower ((unsigned char) c);
    }
    if (! s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

// True iff `path` is `dir` itself or something inside `dir`. Both absolute.
// Used by filesDropped to reject re-loading our own tempDir() exports
// (GentSampler_Pad*.wav / Performance.mid), which restore as a silent EMPTY
// source and read as a paint bug (CLAUDE.md landmine). Guards the sibling-
// prefix false positive: dir "/x/GentSampler" must NOT match
// "/x/GentSampler2/y" — the match only counts at a real path boundary.
inline bool pathIsWithin (const std::string& path, const std::string& dir)
{
    const std::string p = normPathForCompare (path);
    const std::string d = normPathForCompare (dir);
    if (d.empty() || p.size() < d.size())
        return false;
    if (p.compare (0, d.size(), d) != 0)
        return false;
    return p.size() == d.size() || p[d.size()] == '/';
}

// ---------------------------------------------------------------------------
//  SECTIONS — Part 1: deterministic N-bar slicing
//  See SECTIONS_SPEC.md PART 1 ("Pure core") for the full normative text this
//  comment mirrors. barSectionSlices is pure and deterministic.
//
//  Section length L = bars * samplesPerBar. sectionCount = ceil(len / L)
//  (>= 1 for len > 0). Pad i < min(sectionCount, 16) gets cue[i] = i*L,
//  0-anchored (grid-aligned by construction). Pad i >= sectionCount is
//  UNASSIGNED: cue[i] = -1 — contrast sliceBeats (PluginProcessor.cpp:583-596)
//  which clamps overflow pads to len-1 (a pile-up); barSectionSlices must
//  NEVER do that — an unassigned pad stays -1, full stop.
//  Degenerate (len<=0 || samplesPerBar<=0 || bars<=0): all -1, sectionCount 0.
// ---------------------------------------------------------------------------
struct BarSections
{
    std::array<int, 16> cue;
    int sectionCount;
};

inline BarSections barSectionSlices (int len, double samplesPerBar, int bars)
{
    BarSections result {};
    result.cue.fill (-1);
    result.sectionCount = 0;

    if (len <= 0 || samplesPerBar <= 0.0 || bars <= 0)
        return result;

    const double L = (double) bars * samplesPerBar;
    const int sectionCount = (int) std::ceil ((double) len / L);
    result.sectionCount = std::max (1, sectionCount);

    const int assigned = std::min (result.sectionCount, 16);
    for (int i = 0; i < assigned; ++i)
    {
        // i < sectionCount guarantees i*L < len; keep a defensive min against
        // len-1 only in case llround pushes the rounded value past the end.
        const long long pos = std::llround ((double) i * L);
        result.cue[(size_t) i] = std::min ((int) pos, len - 1);
    }

    return result;
}

// ---------------------------------------------------------------------------
//  SECTIONS — Part 2: NOVELTY (spectral-change) boundary detection.
//  See SECTIONS_SPEC.md PART 2 + AMENDMENT P2-A for the full normative text
//  these comments mirror. Pure, JUCE-free, deterministic. Consumes only the
//  cached P1 FrameFeatures — no new FFT, no analyzer changes.
// ---------------------------------------------------------------------------

// One frame's novelty (spectral-change) readout vs the PREVIOUS frame:
//  - combined   — cosine distance over the concat of L2-normalized band[4]
//                 and L2-normalized chroma[12] (each part normalized
//                 SEPARATELY, then concatenated, so band and chroma weight
//                 equally regardless of their raw magnitude).
//  - bandDist   — cosine distance over band[4] alone.
//  - chromaDist — cosine distance over chroma[12] alone.
// Cosine distance = 1 - cosine similarity. If either frame's vector (for the
// given part -- band, chroma, or the band+chroma concat for `combined`) is
// all-zero/silent, that distance is defined as 0 (no NaN, no divide-by-zero).
struct NoveltyPoint
{
    float combined;
    float bandDist;
    float chromaDist;
};

namespace detail
{
    // Cosine distance between two equal-length raw vectors. Either vector
    // all-zero -> 0 (silent frame convention, SECTIONS_SPEC.md PART 2).
    inline float cosineDistanceRaw (const float* a, const float* b, int n)
    {
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (int i = 0; i < n; ++i)
        {
            dot += (double) a[i] * (double) b[i];
            na  += (double) a[i] * (double) a[i];
            nb  += (double) b[i] * (double) b[i];
        }
        if (na <= 0.0 || nb <= 0.0)
            return 0.0f;
        const double sim = dot / (std::sqrt (na) * std::sqrt (nb));
        return (float) (1.0 - std::clamp (sim, -1.0, 1.0));
    }

    // L2-normalize `src[n]` into `dst[n]`; leaves dst all-zero if src is
    // all-zero/silent (never divides by zero).
    inline void l2Normalize (const float* src, float* dst, int n)
    {
        double sumSq = 0.0;
        for (int i = 0; i < n; ++i)
            sumSq += (double) src[i] * (double) src[i];
        if (sumSq <= 0.0)
        {
            for (int i = 0; i < n; ++i) dst[i] = 0.0f;
            return;
        }
        const double invNorm = 1.0 / std::sqrt (sumSq);
        for (int i = 0; i < n; ++i)
            dst[i] = (float) ((double) src[i] * invNorm);
    }
}

inline std::vector<NoveltyPoint> noveltyCurve (const std::vector<FrameFeatures>& frames)
{
    std::vector<NoveltyPoint> out (frames.size(), NoveltyPoint { 0.0f, 0.0f, 0.0f });
    if (frames.size() < 2)
        return out;

    for (size_t f = 1; f < frames.size(); ++f)
    {
        const auto& prev = frames[f - 1];
        const auto& cur  = frames[f];

        // combined: concat of separately-L2-normalized band[4] + chroma[12]
        float prevN[16], curN[16];
        detail::l2Normalize (prev.band, prevN, 4);
        detail::l2Normalize (prev.chroma, prevN + 4, 12);
        detail::l2Normalize (cur.band, curN, 4);
        detail::l2Normalize (cur.chroma, curN + 4, 12);

        NoveltyPoint p;
        p.combined   = detail::cosineDistanceRaw (prevN, curN, 16);
        p.bandDist   = detail::cosineDistanceRaw (prev.band, cur.band, 4);
        p.chromaDist = detail::cosineDistanceRaw (prev.chroma, cur.chroma, 12);
        out[f] = p;
    }

    return out;
}

// Centered moving average of NoveltyPoint::combined over window `w` (clamped
// >= 1; even w forced to the next odd via w|1). Edges average only the
// in-range portion (no zero-padding bias).
inline std::vector<float> smoothCurve (const std::vector<NoveltyPoint>& curve, int w)
{
    std::vector<float> out (curve.size(), 0.0f);
    if (curve.empty())
        return out;

    w = std::max (1, w) | 1;   // clamp >=1, force odd
    const int half = w / 2;
    const int n = (int) curve.size();

    for (int i = 0; i < n; ++i)
    {
        const int lo = std::max (0, i - half);
        const int hi = std::min (n - 1, i + half);
        double sum = 0.0;
        for (int j = lo; j <= hi; ++j)
            sum += (double) curve[(size_t) j].combined;
        out[(size_t) i] = (float) (sum / (double) (hi - lo + 1));
    }

    return out;
}

// ALL novelty tunables live in this ONE struct (SECTIONS_SPEC.md PART 2 +
// AMENDMENT P2-A) — the Joe ear-gate tunes these values only, same
// discipline as ClassifierThresholds.
struct NoveltyThresholds
{
    float kFew = 1.5f;
    float kMedium = 1.0f;
    float kMany = 0.6f;
    float smoothBars = 0.75f;
    float minSectionBars = 1.0f;
};

constexpr NoveltyThresholds kNoveltyThresh {};

// Local maxima of `smoothed` (strictly greater than both neighbors) whose
// value exceeds mean + k*stddev of the WHOLE smoothed curve. Peaks closer
// than minGapFrames have the weaker one dropped (greedy strongest-first).
// Returns ascending frame indices. Degenerate (size<3, zero variance) ->
// empty.
inline std::vector<int> pickNoveltyPeaks (const std::vector<float>& smoothed, float k, int minGapFrames)
{
    std::vector<int> result;
    const int n = (int) smoothed.size();
    if (n < 3)
        return result;

    double mean = 0.0;
    for (float v : smoothed) mean += (double) v;
    mean /= (double) n;

    double var = 0.0;
    for (float v : smoothed) var += ((double) v - mean) * ((double) v - mean);
    var /= (double) n;
    const double stddev = std::sqrt (var);

    if (stddev <= 0.0)
        return result;   // flat curve -> no meaningful peaks

    const double threshold = mean + (double) k * stddev;

    struct Peak { int idx; float value; };
    std::vector<Peak> candidates;
    for (int i = 1; i < n - 1; ++i)
    {
        if (smoothed[(size_t) i] > smoothed[(size_t) (i - 1)]
            && smoothed[(size_t) i] > smoothed[(size_t) (i + 1)]
            && (double) smoothed[(size_t) i] > threshold)
        {
            candidates.push_back ({ i, smoothed[(size_t) i] });
        }
    }
    if (candidates.empty())
        return result;

    // Greedy strongest-first: sort by value descending, keep a peak only if
    // it is farther than minGapFrames from every already-kept peak.
    std::vector<Peak> byStrength = candidates;
    std::sort (byStrength.begin(), byStrength.end(),
               [] (const Peak& a, const Peak& b) { return a.value > b.value; });

    std::vector<int> kept;
    const int gap = std::max (1, minGapFrames);
    for (const auto& c : byStrength)
    {
        bool tooClose = false;
        for (int k2 : kept)
            if (std::abs (c.idx - k2) < gap) { tooClose = true; break; }
        if (! tooClose)
            kept.push_back (c.idx);
    }

    std::sort (kept.begin(), kept.end());
    return kept;
}

// Top-level chain: FrameFeatures -> boundary sample positions, snapped to the
// nearest beat, boundary 0 always present, deduped, capped by the caller at
// 16 (this function returns the full set; callers cap for pad assignment and
// report the overflow — SECTIONS_SPEC.md PART 2 point 4).
// sensitivity: 0 few, 1 medium, 2 many.
inline std::vector<int> noveltyBoundaries (const std::vector<FrameFeatures>& frames,
                                            double frameRate, double samplesPerBar,
                                            double sampleRate, int sensitivity)
{
    if (frames.empty())
        return {};
    if (frameRate <= 0.0 || samplesPerBar <= 0.0 || sampleRate <= 0.0)
        return { 0 };

    const double framesPerBar = frameRate * (samplesPerBar / sampleRate);
    const int w = std::max (3, (int) std::round (kNoveltyThresh.smoothBars * framesPerBar));
    const int minGap = std::max (1, (int) std::round (kNoveltyThresh.minSectionBars * framesPerBar));

    const float k = sensitivity <= 0 ? kNoveltyThresh.kFew
                  : sensitivity == 1 ? kNoveltyThresh.kMedium
                                     : kNoveltyThresh.kMany;

    const auto curve    = noveltyCurve (frames);
    const auto smoothed = smoothCurve (curve, w);
    const auto peaks    = pickNoveltyPeaks (smoothed, k, minGap);

    const double spb = samplesPerBar / 4.0;   // beat = bar / 4 (assume 4/4)

    std::vector<int> boundaries;
    boundaries.push_back (0);
    for (int peakFrame : peaks)
    {
        const double pos = (double) peakFrame / frameRate * sampleRate;
        long long snapped = spb > 0.0 ? std::llround (std::round (pos / spb) * spb)
                                      : std::llround (pos);
        snapped = std::max ((long long) 0, snapped);
        boundaries.push_back ((int) snapped);
    }

    std::sort (boundaries.begin(), boundaries.end());
    boundaries.erase (std::unique (boundaries.begin(), boundaries.end()), boundaries.end());

    return boundaries;
}

// ---------------------------------------------------------------------------
//  KIT — Part A: hit isolation + time-order layout.
//  See KIT_SPEC.md PART A ("Pure core") for the full normative text this
//  comment mirrors. kitHits is pure and deterministic.
//
//  Contrast with Analyzer::analyze's `slices` (Analysis.h:198-241): `slices`
//  selects the top-16 transients by STRENGTH with an 80 ms min-gap — it
//  drops/merges hits and caps at 16, which is exactly what a "get every hit"
//  kit must NOT do. kitHits instead takes ALL onsets (`onsets`, every peak,
//  no cap), keeps every one at/above the sensitivity's strength floor, then
//  applies only a 30 ms anti-double-trigger min-spacing pass that drops the
//  LATER of two too-close onsets (never strength-based — that's what loses
//  a real hit). Time-ordered output; no 16-cap here (the caller lays the
//  first 16 and reports overflow, `sectionCount`/barSectionSlices precedent).
// ---------------------------------------------------------------------------
struct KitThresholds
{
    float sFew = 0.45f, sMedium = 0.25f, sMany = 0.12f;
    float minSpacingSec = 0.03f;
};

constexpr KitThresholds kKitThresh {};

// onsets: ascending (samplePos, strength 0..1) pairs, by construction (the
// analyzer's own onset list). sensitivity: 0 few, 1 medium, 2 many. Empty
// onsets or sampleRate<=0 -> empty.
inline std::vector<int> kitHits (const std::vector<std::pair<int, float>>& onsets,
                                  double sampleRate, int sensitivity)
{
    if (onsets.empty() || sampleRate <= 0.0)
        return {};

    const float sThresh = sensitivity <= 0 ? kKitThresh.sFew
                        : sensitivity == 1 ? kKitThresh.sMedium
                                           : kKitThresh.sMany;

    std::vector<int> kept;
    for (const auto& o : onsets)
        if (o.second >= sThresh)
            kept.push_back (o.first);

    const int minSpacing = (int) (sampleRate * (double) kKitThresh.minSpacingSec);

    std::vector<int> result;
    for (int pos : kept)
    {
        if (! result.empty() && (pos - result.back()) < minSpacing)
            continue;   // too close to the previously-kept (earlier) hit: drop the LATER one
        result.push_back (pos);
    }

    return result;
}

} // namespace gent
