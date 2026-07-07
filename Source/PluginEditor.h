#pragma once
// ============================================================================
//  PluginEditor.h — GentSampler v1.1 GUI
//  Row 1: title / file / BPM / key
//  Waveform with 16 numbered cue flags (click/drag moves selected pad's cue)
//  Row 2: LOAD, AUTO-SLICE, slice-mode menu, TEMPO SYNC, KEYBOARD, MASTER PITCH
//  Row 3: SAVE KIT, LOAD KIT, EXPORT KIT, REC, drag-out chips (PAD WAV / MIDI)
//  Bottom: 4x4 pad grid (left) + per-pad controls (right)
// ============================================================================

#include "PluginProcessor.h"
#include "Theme.h"

inline juce::Colour padColour (int i)
{
    return juce::Colour::fromHSV ((float) i / 16.0f, 0.65f, 0.95f, 1.0f);
}

// stem hues come from the Theme tokens (DRM/BASS/VOX/GTR/PNO/OTH)
inline juce::Colour stemColour (int k) { return Theme::stem (k); }

// a pad set to a single stem takes that stem's colour; FULL or multi keeps its default hue
inline juce::Colour padSourceColour (int i, const GentSamplerAudioProcessor& p)
{
    const int k = gent::singleStemIndex (p.getPadStemMask (i));
    return k >= 0 ? stemColour (k) : padColour (i);
}

// C2: the hero-map visuals (flags/slice-line glow, playheads) show what each pad
// actually PLAYS — mockup: "FULL = neutral #C7CCD2". Unlike padSourceColour (used
// by the pad grid, which keeps every pad a distinct rainbow hue even when its
// source is FULL/multi-stem), the map must fall back to Theme::fullStem instead
// of the rainbow padColour so an unset/FULL/multi-stem pad reads as neutral on
// the map, matching the mockup's stem-hue legend exactly.
inline juce::Colour padMapHue (int i, const GentSamplerAudioProcessor& p)
{
    const int k = gent::singleStemIndex (p.getPadStemMask (i));
    return k >= 0 ? stemColour (k) : Theme::fullStem;
}

// C3 review fix: the END-handle drag decision (collapse-to-open vs. real window,
// with SNAP) used to be duplicated verbatim between WaveformView::mouseDrag and
// SliceDetailStrip::mouseDrag — one copy here, called by both, so the rule can
// never drift between the two edit surfaces. The collapse check stays in SAMPLE
// space (proposedEnd <= cue + tolerance), with the caller responsible for
// converting its own on-screen ~8px affordance into sample-space tolerance at its
// OWN current zoom (map and strip zoom independently, so 8px means a different
// sample count on each) — this preserves the map's existing zoom-dependent
// behaviour byte-for-byte while giving the strip the identical rule at its zoom.
// F1: snap block removed (SLICE_FEEL_TASK.md F1 bullet 3 / AC-F1.7) — the caller
// (HandleDragEngine::handleDragMove) now resolves snap BEFORE calling this, so
// `proposedEndSample` arriving here is already the final (possibly snapped)
// candidate. This function's only remaining job is the collapse-vs-window
// decision and the cue+33 floor, both byte-identical to before.
inline void applyEndHandleDrag (GentSamplerAudioProcessor& p, int pad,
                                int proposedEndSample, int collapseToleranceSamples)
{
    const int cue = p.getCue (pad);
    p.setCueEnd (pad, gent::resolveEndDragTarget (cue, proposedEndSample, collapseToleranceSamples));
}

// ---------------------------------------------------------------------------
// F1: HandleDragEngine — the single shared implementation of relative-drag
// CUE/END/grain-marker editing for BOTH edit surfaces (WaveformView hero map,
// SliceDetailStrip). Ground rule 1 (SLICE_FEEL_TASK.md): all drag/snap/nudge
// decision logic lives HERE, once; surfaces only supply their own hit-testing,
// spp (samples-per-pixel) and repaint calls — no per-surface fork of the
// decision tree itself.
//
// Shape is deliberately future-proofed for F2 (fine-mode rate), F3 (snap
// threshold resolve step) and F5 (grain-marker reuse) so those tasks are pure
// additions to this struct/these functions, not rewrites:
//   - `rate` is already threaded through handleDragMove (F1 hardcodes 1.0;
//     F2 sets it from e.mods.isShiftDown() at the call site).
//   - the snap step below is isolated in resolveSnap() so F3 can replace its
//     body with the 6px-threshold test without touching the accumulator.
//   - `handle` already carries a `grain` enumerator so F5's marker drag can
//     drive this exact accumulator/lazy-undo path with no third drag impl.
struct HandleDragEngine
{
    enum class Handle { none, cue, end, grain };

    Handle handle = Handle::none;
    int    pad = -1;
    int    anchorSample = 0;      // getCue/getEffectiveCueEnd at mouseDown
    int    lastX = 0;             // last raw event x (screen px)
    double accumSamples = 0.0;    // double accumulation -> immune to view-scroll & rounding drift
    bool   undoPushed = false;    // lazy pushUndo guard: first EFFECTIVE movement only

    bool isActive() const { return handle != Handle::none && pad >= 0; }
};

// mouseDown: arm the gesture. No setCue/setCueEnd call, no pushUndo here — F1
// ground rule: "click-without-move creates no undo entry" (AC-F1.1/AC-F1.2).
// F5: Handle::grain seeds anchorSample from the marker's CURRENT absolute
// sample position (cue + frac*(end-cue)), the same "anchor = current handle
// position" rule CUE/END already follow — this is what makes AC-F5.1's
// no-jump-at-grab guarantee hold for the marker too (see handleDragMove).
inline void handleDragBegin (HandleDragEngine& g, GentSamplerAudioProcessor& p,
                             int pad, HandleDragEngine::Handle handle, int startX,
                             int openEndAnchor = -1)
{
    g.pad = pad;
    g.handle = handle;
    g.lastX = startX;
    g.accumSamples = 0.0;
    g.undoPushed = false;
    if (handle == HandleDragEngine::Handle::cue)
        g.anchorSample = p.getCue (pad);
    else if (handle == HandleDragEngine::Handle::end)
    {
        // PREPACK_UX U2: an OPEN slice's drawn end grip sits at cue+~14px (hero)
        // or waveR-10 (strip), not at len-1 — seed the anchor at the grip the
        // caller actually drew so first movement follows the mouse instead of
        // teleporting to end-of-file. Non-open slices are unaffected (anchor =
        // effectiveCueEnd, same as before, its drawn position already matches).
        g.anchorSample = (p.isOpenSlice (pad) && openEndAnchor >= 0)
                            ? openEndAnchor
                            : p.getEffectiveCueEnd (pad);
    }
    else // grain
    {
        const int cue = p.getCue (pad);
        const int end = p.getEffectiveCueEnd (pad);
        g.anchorSample = cue + (int) (p.getGrainPosFor (pad) * (float) juce::jmax (1, end - cue));
    }
}

// Single shared snap-resolve step (F1 bullet 3: snap moves OUT of the drag path,
// into the engine, for both CUE and END — applyEndHandleDrag's own snap block is
// removed). F3: capture-threshold + Alt-bypass test (SLICE_FEEL_TASK.md F3) —
// snap only engages when the candidate grid/transient point is within 6 screen
// px (6 * sppNow samples) of the proposed sample, and Alt held bypasses snap
// entirely for that event (modifiers read per event, no extra state — releasing
// Alt mid-gesture re-engages on the very next event). nearestTransient's own
// internal 50ms cap (PluginProcessor.cpp) is untouched, so effective transient
// capture is the min of the two. The accumulator/lazy-undo mechanics and both
// call sites below are otherwise untouched by F3.
inline int resolveSnap (GentSamplerAudioProcessor& p, int proposed, double sppNow, bool altDown)
{
    // reviewer-inspection note (T3.2, TEST_TARGET_TASK.md): the two lines below —
    // the snap-disabled and Alt-bypass early-outs — are covered by reviewer
    // inspection, not unit test (they need a live GentSamplerAudioProcessor to
    // construct, which the logic-only test binary deliberately can't do). See
    // PluginEditor.h:140-141 in the spec's line numbering.
    if (! p.snapEnabled.load() || altDown)
        return proposed;
    const int cand = (p.gridStepSamples() > 0.0) ? p.nearestGridLine (proposed)
                                                  : p.nearestTransient (proposed);
    return gent::applySnapThreshold (proposed, cand, sppNow);
}

// mouseDrag per event: incremental accumulation in sample space (double), then
// apply via the existing processor calls with snap=false (the engine, not
// setCue, now owns snap resolution for the drag path — ground rule 2). `rate`
// is the F2 hook (1.0 in F1; Shift => 0.10 from F2 onward). `collapseTolSamp`
// is the surface's own ~8px-at-its-zoom tolerance for applyEndHandleDrag,
// unchanged from the pre-F1 per-surface computation. `altDown` is F3's Alt-
// bypass flag, read per event by the caller from e.mods (same pattern as F2's
// rate) and threaded straight into resolveSnap — no new engine state.
inline void handleDragMove (HandleDragEngine& g, GentSamplerAudioProcessor& p,
                            int x, double samplesPerPixel, int collapseTolSamples,
                            double rate = 1.0, bool altDown = false)
{
    if (! g.isActive()) return;

    g.accumSamples += (double) (x - g.lastX) * samplesPerPixel * rate;
    g.lastX = x;
    const int proposed = g.anchorSample + juce::roundToInt (g.accumSamples);

    if (proposed == g.anchorSample && ! g.undoPushed)
        return;                                    // no effective movement yet: no edit, no undo

    // 0.3: the F5 grain-marker exclusion below is REMOVED. Its rationale was
    // that grain position lived only in APVTS, outside CueSnap, so a grain-only
    // pushUndo would record an entry Ctrl+Z "restores" with zero visible effect.
    // That rationale dissolves now that CueSnap carries all 7 grain params
    // (PluginProcessor.h CueSnap::grain, snapshot()/applySnap() in
    // PluginProcessor.cpp) — a marker drag's first effective movement is now a
    // real, undoable gesture like CUE/END, using the exact same lazy
    // first-effective-movement guard (`undoPushed`) as those two handles.
    if (! g.undoPushed)
    {
        p.pushUndo();
        g.undoPushed = true;
    }

    if (g.handle == HandleDragEngine::Handle::cue)
    {
        p.setCue (g.pad, resolveSnap (p, proposed, samplesPerPixel, altDown), false);
    }
    else if (g.handle == HandleDragEngine::Handle::end)
    {
        // F1 bullet 3 / AC-F1.7: snap resolved HERE (once, shared with CUE above),
        // not inside applyEndHandleDrag (that block was removed) — the resolved
        // value is what the collapse-vs-window decision sees. F3: resolveSnap now
        // applies the 6px-capture-threshold + Alt-bypass test; this call site's
        // shape is unchanged from F1 to F3.
        applyEndHandleDrag (p, g.pad, resolveSnap (p, proposed, samplesPerPixel, altDown), collapseTolSamples);
    }
    else if (g.handle == HandleDragEngine::Handle::grain)
    {
        // F5: grain marker reuses this exact accumulator (no third drag impl,
        // AC-F5.3) but takes NEITHER of the two branches above's extra behaviour:
        // no resolveSnap (the marker never had snap, and the spec says it never
        // should — it's a performance control, not a cut point) and no
        // applyEndHandleDrag collapse logic (there's no OPEN-slice concept for a
        // grain position). anchorSample was seeded in handleDragBegin from the
        // CURRENT marker sample (cue + frac*(end-cue)), so `proposed` here is
        // already in absolute sample space, exactly like CUE/END. Clamp to the
        // live [cue, effectiveEnd] window (it can move independently of the
        // marker's own gesture, e.g. via nudge/other-pad edits, but not mid-drag
        // in practice) and convert back to the fraction setGrainPosFor expects.
        const int cue = p.getCue (g.pad);
        const int end = p.getEffectiveCueEnd (g.pad);
        const int clamped = juce::jlimit (cue, juce::jmax (cue, end), proposed);
        const int span = juce::jmax (1, end - cue);
        const float frac = (float) (clamped - cue) / (float) span;
        p.setGrainPosFor (g.pad, frac);
    }
}

inline void handleDragEnd (HandleDragEngine& g)
{
    g.handle = HandleDragEngine::Handle::none;
    g.pad = -1;
}

// ---------------------------------------------------------------------------
class WaveformView : public juce::Component,
                     private juce::Timer
{
public:
    explicit WaveformView (GentSamplerAudioProcessor& proc) : p (proc)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);   // F4: clicking the map puts keyboard focus in the
                                         // plugin so unhandled keys (arrows) bubble to the
                                         // editor's keyPressed (single handler, ground rule 1)
        startTimerHz (30);
    }

    void setFollow (bool shouldFollow)                     { follow = shouldFollow; }
    void fullView()                                        { if (cachedLen > 0) setView (0.0, (double) cachedLen); }

    // F4: fired right after handleDragBegin arms a CUE/END gesture from a mouse grab on
    // this surface, so the editor can mirror it into its own armed-handle nudge target
    // (SLICE_FEEL_TASK.md F4 — "set also when a handle is grabbed by mouse on EITHER
    // surface").
    std::function<void (HandleDragEngine::Handle)> onHandleGrabbed;

    // C2: the hero's corner-anchored overlay chips (mockup .ov.bl/.br) sit on solid
    // plates below the wave; reserve that band so the scrollbar + end-handle grips
    // don't render underneath them (the actual collision B6 flagged). The caller
    // (PluginEditor::layoutContent) owns the real chip geometry and passes its
    // height here — height-relative, no 196-derived offset baked in.
    void setBottomChromeInset (int px)                     { bottomChromeH = juce::jmax (0, px); repaint(); }

    std::function<void()> onRequestLoad;                   // called when the empty map is clicked

    // D2 R2-fix: the STEMS-placeholder "SEPARATE STEMS to fill the map" CTA must
    // actually click (R2 SEV-2 finding — a painted-only chip is the B6 dead-chip
    // failure class). The editor wires this to the SAME callback sepStemsBtn's
    // onClick uses (direct std::function copy, same precedent as onRequestLoad
    // above / loadBtn.onClick — no logic duplicated, no re-derivation of the
    // processor call).
    std::function<void()> onRequestStemSeparation;

    // ------------------------------------------------------------------ paint
    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::well);
        const int w = getWidth(), h = getHeight();
        const int sb = 11;                      // scrollbar strip height
        // C2: pull the interactive/scrollbar band up above the reserved corner-chip
        // plates (mockup .ov.bl/.ov.br) so nothing important renders under them.
        const int waveBottom = h - sb - bottomChromeH;
        // R2 fix: invalidate the CTA hit-rect unconditionally at the top of every
        // paint. It is only re-armed inside the STEMS-placeholder block below, so
        // no stale rect from a prior frame/state can survive into COMPOSITE view,
        // the stems-ready STEMS view, or the no-source empty state.
        ctaRect = {};
        {   // recessed-well inner top shadow (mockup .hero inset)
            juce::ColourGradient sh (juce::Colours::black.withAlpha (0.5f), 0.0f, 0.0f,
                                     juce::Colours::transparentBlack, 0.0f, 10.0f, false);
            g.setGradientFill (sh);
            g.fillRect (0, 0, w, 10);
        }

        if (peaks.empty() || cachedLen <= 0)
        {
            // first-launch teaching line: LOAD -> SEPARATE -> SLICE as quiet ghost
            // chips with a one-line hint (whole map is already click-to-load)
            const char* steps[3] = { "LOAD", "SEPARATE", "SLICE" };
            const float chipW = 86.0f, chipH = 24.0f, arrowW = 26.0f;
            const float totW = chipW * 3.0f + arrowW * 2.0f;
            float cx = ((float) w - totW) * 0.5f;
            const float cy = (float) h * 0.5f - 22.0f;
            for (int i = 0; i < 3; ++i)
            {
                auto cr = juce::Rectangle<float> (cx, cy, chipW, chipH);
                g.setColour (juce::Colours::white.withAlpha (0.10f));
                g.drawRoundedRectangle (cr, 6.5f, 1.0f);
                g.setColour (Theme::t2);
                g.setFont (Theme::chipFont());
                g.drawText (steps[i], cr, juce::Justification::centred);
                if (i < 2)
                {
                    g.setColour (Theme::t3);
                    g.setFont (Theme::ui (12.0f));
                    g.drawText (">", juce::Rectangle<float> (cx + chipW, cy, arrowW, chipH),
                                juce::Justification::centred);
                }
                cx += chipW + arrowW;
            }
            g.setColour (Theme::t3);
            g.setFont (Theme::mono (9.0f));
            g.drawText (juce::String (juce::CharPointer_UTF8 ("click anywhere or drop a sample  \xc2\xb7  wav / mp3 / aiff / flac / ogg")),
                        juce::Rectangle<float> (0.0f, cy + chipH + 12.0f, (float) w, 14.0f),
                        juce::Justification::centred);
            return;
        }

        // ---------------------------------------------------------------
        // D2: hero COMPOSITE<->STEMS top-level paint branch.
        //
        // Branch selector: the SANITIZED STICKY REQUEST (gent::sanitizeHeroView),
        // not gent::resolveHeroView -- per the D1 addendum ("Joe ruling
        // 2026-07-03, DECISION-2/3 interplay"): requesting STEMS always enters
        // the STEMS branch (a placeholder is always available, so the branch is
        // never suppressed by stem availability; row 1 above -- no source loaded
        // at all -- is the sole exception and is handled by the early return
        // just above, unchanged, regardless of request).
        //
        // Inside the STEMS branch, gent::resolveHeroView(request, hasStems())
        // answers the CONTENT question exactly per docs/STEM_VIEW_MODEL.md SS6's
        // R1 clarification and the addendum: 1 (stems available) -> real lane
        // territory (D3 paints the actual per-stem columns; D2c scaffolds the
        // same six plates + labels here as a placeholder-level stand-in, since
        // real lane data painting is explicitly D3's job, not D2c's); 0 (no
        // stems yet / downloading / separating / failed) -> the DECISION-3
        // placeholder: six empty lane plates + stem-name labels + a centered
        // "SEPARATE STEMS to fill the map" CTA that is a REAL click target (R2
        // fix): its painted rect is captured into ctaRect below and hit-tested
        // in mouseDown/mouseMove, invoking the same callback sepStemsBtn's
        // onClick uses (onRequestStemSeparation, wired in PluginEditor.cpp as a
        // direct std::function copy of sepStemsBtn.onClick -- no logic
        // duplicated).
        //
        // COMPOSITE branch (request == 0): today's paint, unchanged, falls
        // through below -- including the still-live always-on lanes band (D3
        // retires it; D2c must not touch it, per REDESIGN_TASK_D.md D2 scope).
        const int heroReq = gent::sanitizeHeroView (p.heroView.load());
        if (heroReq == 1)
        {
            // D3: real port of the mockup's drawStems() (gentsampler_redesign_v2.html
            // 469-491). The mockup canvas is 2x backing scale (2032x320 vs a 1016x160
            // hero element, docs/STEM_VIEW_MODEL.md SS2 "2x canvas-backing note") --
            // px literals below are halved from the mockup's canvas-space values,
            // then aligned to the existing stemLabW/stemSoloW members (60/18) that
            // D4's lane hit-zones will reuse verbatim (STEM_VIEW_MODEL.md SS8 point 1
            // cites "the exact constants already used at lines 735/733" -- i.e. these
            // same members), rather than an independently re-derived half-of-58. The
            // mockup's own column start (x=64 canvas-space) is superseded by "start
            // right of the label plate" (this task's own prose) so columns never
            // begin mid-plate. sbs capture (D6) arbitrates any residual.
            const bool stemsReady = gent::resolveHeroView (heroReq, p.hasStems()) == 1;
            static const char* slab[6] = { "DRUMS", "BASS", "VOX", "GTR", "PNO", "OTHER" };
            const float laneH  = (float) waveBottom / 6.0f;
            const float plateW = (float) stemLabW;
            const float soloW  = (float) stemSoloW;

            // D3/D1-DECISION-4: lanes claim the FULL hero -- no separate flag row
            // reserved above them (unlike composite's flagY-row layout). Flags/cue-
            // region/playheads render full-height THROUGH the lanes, matching the
            // mockup's drawFlags(g,W,H) full-canvas treatment (SS2). paint() uses
            // local laneH/waveBottom directly for its own geometry below -- it does
            // NOT write stemBandTop/stemBandH/flagBarY/flagBarH here. Those members
            // stay at their composite-branch-set values (stemBandTop/H zeroed by the
            // retired band; flagBarY/H likewise collapse to the bandH==0 constants)
            // so mouseDown/mouseMove's DORMANT band hit-test (PAINT ONLY this task --
            // interaction rewiring is D4's) never activates over the STEMS lanes
            // either -- setting them non-zero here would silently turn that dormant
            // code back on for the wrong geometry, reintroducing the exact
            // whole-band-early-return landmine D4 is supposed to fix, one task early.

            for (int s = 0; s < 6; ++s)
            {
                const auto col = Theme::stem (s);
                const bool muted  = p.isStemMuted (s);
                const bool soloed = p.isStemSoloed (s);
                const float ly     = laneH * (float) s;
                const float rowMid = ly + laneH * 0.5f;

                if (s % 2 != 0)   // odd lanes get a faint alternating bed (mockup drawStems li%2)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.015f));
                    g.fillRect (0.0f, ly, (float) w, laneH);
                }
                g.setColour (juce::Colours::black.withAlpha (0.45f));
                g.fillRect (0.0f, ly + laneH - 1.0f, (float) w, 1.0f);   // lane separator

                if (stemsReady)
                {
                    // real per-stem colour columns from stemPeaks (rebuildStemPeaks,
                    // ~1256-1283), alpha .8 (mockup line 482), starting right of the
                    // label plate -- muted lanes dim to match the pre-D3 band's own
                    // muted ribbon alpha (0.12) so the mute affordance stays legible
                    // at full lane height, not just at the 13px band's scale.
                    const auto& sp = stemPeaks[(size_t) s];
                    if (! sp.empty())
                    {
                        g.setColour (col.withAlpha (muted ? 0.12f : 0.8f));
                        const int x0 = (int) plateW;
                        const int x1 = juce::jmin (w, (int) sp.size());
                        const float rh = laneH * 0.42f;
                        for (int x = x0; x < x1; ++x)
                        {
                            const auto pk = sp[(size_t) x];
                            const float amp = juce::jmax (std::abs (pk.first), std::abs (pk.second));
                            g.drawVerticalLine (x, rowMid - amp * rh, rowMid + amp * rh);
                        }
                    }
                }

                // label plate (opaque, left) -- doubles as the MUTE toggle per
                // DECISION-4 (paint only this task; D4 wires the click). Carries the
                // pre-D3 band's exact mute semantics/appearance (PluginEditor.h
                // 394-410 lineage): filled/tinted when unmuted, outlined/struck-
                // through when muted.
                auto plate = juce::Rectangle<float> (0.0f, ly, plateW, laneH);
                g.setColour (juce::Colour (0xff0A0D10).withAlpha (0.85f));
                g.fillRect (plate);
                if (muted)
                {
                    g.setColour (Theme::knobTrack);
                    g.drawRoundedRectangle (plate.reduced (2.0f), 4.0f, 1.0f);
                    g.setColour (Theme::t3);
                }
                else
                {
                    g.setColour (col.withAlpha (0.12f));
                    g.fillRoundedRectangle (plate.reduced (2.0f), 4.0f);
                    g.setColour (col);
                }
                g.setFont (Theme::mono (9.5f * 1.12f, true));
                g.drawText (slab[s], plate.reduced (6.0f, 0.0f), juce::Justification::centredLeft);
                if (muted)
                    g.drawLine (plate.getX() + 6.0f, rowMid, plate.getRight() - 6.0f, rowMid, 1.0f);

                // solo "S" box, hover-revealed (or shown solid when soloed) at the
                // lane's right edge -- carries the pre-D3 band's exact solo semantics/
                // appearance (PluginEditor.h 412-424 lineage). Paint only this task;
                // hoverLane is untouched (still fed by mouseMove's existing dormant
                // code, itself unchanged by D3 per the timer-stomp/single-edit-path
                // rules).
                if (hoverLane == s || soloed)
                {
                    auto sbox = juce::Rectangle<float> ((float) w - soloW - 2.0f,
                                                        rowMid - soloW * 0.5f, soloW, soloW);
                    if (soloed)
                    {
                        g.setColour (Theme::accent); g.fillRoundedRectangle (sbox, 4.0f);
                        g.setColour (Theme::inkOnAccent);
                    }
                    else
                    {
                        g.setColour (Theme::panel); g.fillRoundedRectangle (sbox, 4.0f);
                        g.setColour (Theme::knobTrack.brighter (0.25f));
                        g.drawRoundedRectangle (sbox, 4.0f, 1.0f);
                        g.setColour (Theme::t3);
                    }
                    g.setFont (juce::Font (9.0f, juce::Font::bold));
                    g.drawText ("S", sbox, juce::Justification::centred);
                }
            }

            if (! stemsReady)
            {
                // D5 / docs/STEM_VIEW_MODEL.md SS6 matrix rows 2-4/6: the placeholder
                // must not contradict stemStatusLbl -- it reads the SAME lifecycle
                // predicates/narration the label already reads (PluginEditor.cpp
                // ~1590-1607), never a second derivation. Row 2 (idle, truly never
                // separated -- getStemStatus() is empty): the DECISION-3 CTA hint.
                // Row 6 (failed -- getStemStatus() holds doStemJob's error text,
                // e.g. "model download failed: ..."/"init failed: ...", set right
                // before `separating`/`downloadingModels` both go back to false):
                // show that error text instead of the generic hint, but the click-
                // target STAYS LIVE (retry is exactly what a failed, now-idle state
                // should allow -- same as sepStemsBtn, which is only ever disabled
                // by `busy`, not by a prior failure). Rows 3/4 (a job is ALREADY
                // running, gent::ctaEnabledFor(separating, downloadingModels) ==
                // false): status narration ("Downloading models NN%" / "Separating
                // NN%"), and the click-target goes INERT -- clicking SEPARATE STEMS
                // mid-job must not double-fire a second separation queued behind
                // the one already running (doStemJob() runs on the single serial
                // worker thread; wantStems.exchange(false) is polled once per outer
                // wait(250) loop, so a second requestStemSeparation() call while
                // `separating`/`downloadingModels` is true is silently QUEUED and
                // re-runs the whole job again right after the current one finishes
                // -- exactly the double-fire this guards against). sepStemsBtn
                // already guards the same way via `sepStemsBtn.setEnabled (! busy)`
                // (PluginEditor.cpp:1593); this matches that existing affordance
                // rather than inventing a new one -- a disabled-look chip (dimmed
                // text, no hover/click) is sufficient, since the processor has
                // nothing else to add here (no re-entry lock to surface, just
                // "don't queue a second job").
                const bool ctaEnabled = gent::ctaEnabledFor (p.isSeparating(), p.isDownloadingModels());
                const bool busy = ! ctaEnabled;
                const juce::String status = p.getStemStatus();
                const juce::String hint = status.isNotEmpty() ? status
                                                                : juce::String ("SEPARATE STEMS to fill the map");
                g.setFont (Theme::ui (10.5f * 1.12f, true).withExtraKerningFactor (0.12f));
                const float hintW = juce::jmin ((float) w - 24.0f,
                                                 g.getCurrentFont().getStringWidthFloat (hint) + 28.0f);
                auto hintR = juce::Rectangle<float> (((float) w - hintW) * 0.5f,
                                                     (float) waveBottom * 0.5f - 12.0f, hintW, 24.0f);
                // R2 fix retained: only a LIVE (non-busy) hint arms ctaRect as a
                // real click target -- while busy, ctaRect stays cleared from the
                // top-of-paint reset (this frame paints no clickable CTA), so
                // mouseDown/mouseMove's ctaRect.isEmpty() guards fall through with
                // no hit-test, matching the inert affordance described above.
                if (! busy)
                {
                    ctaRect = hintR;
                    // ctaHoverLast is kept current by mouseMove's own dirty-watch
                    // (single source of truth for hover state -- avoids a second,
                    // possibly-inconsistent hover detector here in paint()).
                    Theme::paintChip (g, hintR, false, ctaHoverLast, false, 3);
                    g.setColour (Theme::t2);
                }
                else
                {
                    // inert/disabled-look chip: same chip painter, hover forced off,
                    // dimmed text -- no ctaRect armed, so it cannot be clicked.
                    Theme::paintChip (g, hintR, false, false, false, 3);
                    g.setColour (Theme::t3);
                }
                g.drawText (hint, Theme::chipFace (hintR), juce::Justification::centred);
            }
            else
            {
                // D3: flags, active cue region, and playheads render full-height
                // THROUGH the lanes (this task's own scope line) -- same shared
                // method the composite branch calls below, top=0/flagY=0 so the
                // full lane area (0..waveBottom) is in play, matching the mockup's
                // drawFlags(g,W,H) full-canvas-height treatment. Only painted once
                // real stem data exists -- the pre-separation placeholder has no
                // meaningful slice content to overlay (mirrors the pre-D3 band,
                // which only ever showed under the composite wave regardless).
                const int selS = p.selectedPad.load();
                const double srS = (keep != nullptr) ? keep->sampleRate : 0.0;
                paintFlagsCuePlayheads (g, w, 0, waveBottom, 0, selS, srS);
            }
            // (stemsReady==false: ctaRect stays cleared from the top-of-paint reset above --
            // no CTA is painted this frame, so no rect should be armed.)

            // scrollbar strip stays live in STEMS view too (shared chrome, unchanged geometry)
            g.setColour (juce::Colour (0xff090a0b));
            g.fillRect (0, waveBottom, w, sb);
            if (cachedLen > 0)
            {
                const float thS = (float) (viewStart / (double) cachedLen) * (float) w;
                const float thE = (float) ((viewStart + viewSpan) / (double) cachedLen) * (float) w;
                g.setColour (juce::Colour (0xff9a9da1).withAlpha (0.7f));
                g.fillRoundedRectangle (thS, (float) waveBottom + 2.0f,
                                        juce::jmax (8.0f, thE - thS), (float) sb - 4.0f, 3.0f);
            }
            return;
        }

        // D3: COMPOSITE branch -- the lanes band is retired entirely (its paint
        // block, the h>60 content gate, and the bandH jlimit(78,250) floor
        // computation all deleted per this task's own scope line: "composite =
        // full-height wave exactly as the bandH==0 layout already renders"). What
        // remains below is that pre-existing bandH==0 layout, unchanged: rulerH is
        // still hardcoded 0 (no top ruler, never present in the mockup either
        // view -- SS2/SS9), bandH/bandTop collapse to their bandH==0 constants so
        // flagY/top compute exactly as they always did whenever no band was
        // showing. stemBandTop/stemBandH are zeroed (not deleted -- D1 SS9
        // "repurposed", D4 rewires their consumers) so the dormant band hit-test
        // in mouseDown/mouseMove keeps compiling but never matches a real click
        // (stemBandH<=0 always fails its own `> 0` guard).
        const int rulerH  = 0;
        const int bandTop = rulerH + 2;
        const int bandH   = 0;
        const int flagH   = 15;
        const int flagY   = bandTop + bandH;             // bandH==0: no +9 gap term
        const int top     = flagY + flagH + 1;          // waveform / region top
        stemBandTop = bandTop; stemBandH = bandH;
        flagBarY = flagY; flagBarH = flagH;
        const int sel = p.selectedPad.load();
        const double sr = (keep != nullptr) ? keep->sampleRate : 0.0;

        // time ruler across the top
        if (rulerH > 0 && sr > 0.0)
        {
            g.setColour (Theme::t3);
            g.setFont (Theme::mono (9.0f));
            for (int t = 0; t <= 4; ++t)
            {
                const double frac = t / 4.0;
                const double secs = (viewStart + frac * viewSpan) / sr;
                const int mm = (int) (secs / 60.0);
                const int ssd = ((int) secs) % 60;
                const juce::String lbl = juce::String (mm) + ":" + juce::String (ssd).paddedLeft ('0', 2);
                const int tx = (int) (frac * (float) (w - 1));
                const auto just = t == 0 ? juce::Justification::topLeft
                                : t == 4 ? juce::Justification::topRight
                                         : juce::Justification::centredTop;
                g.drawText (lbl, juce::jlimit (0, w - 44, tx - 22), 1, 44, rulerH - 2, just);
            }
        }

        // C1: composite view no longer tints slice regions behind the wave — the
        // flags carry pad-source colour (C2). No per-slice fill here by design.

        // coordinate grid (map texture)
        {
            g.setColour (juce::Colours::white.withAlpha (0.022f));
            for (int gx = 0; gx < w; gx += 64)
                g.drawVerticalLine (gx, (float) top, (float) waveBottom);
            const float baseY = (float) top + (float) (waveBottom - top) * 0.5f;
            g.setColour (Theme::accent.withAlpha (0.08f));
            g.drawHorizontalLine ((int) baseY, 0.0f, (float) w);
        }

        // waveform — amber composite with the mockup's vertical gradient (.85/.28/.70):
        // one gradient brush spans the band; every column picks it up by y position
        const float mid  = (float) top + (float) (waveBottom - top) * 0.5f;
        const float half = (float) (waveBottom - top) * 0.48f;
        {
            juce::ColourGradient wg (Theme::accent.withAlpha (0.85f), 0.0f, mid - half,
                                     Theme::accent.withAlpha (0.70f), 0.0f, mid + half, false);
            wg.addColour (0.5, Theme::accent.withAlpha (0.28f));
            g.setGradientFill (wg);
        }
        const int nP = (int) peaks.size();
        for (int x = 0; x < juce::jmin (w, nP); ++x)
        {
            const auto pk = peaks[(size_t) x];
            g.drawVerticalLine (x, mid - pk.second * half, mid - pk.first * half);
        }

        // beat/bar grid (2B): faint tempo grid shown when SNAP is on, so handle-snap
        // is predictable. Bars brightest, beats next, subdivisions faintest.
        if (p.snapEnabled.load())
        {
            const double beat = p.samplesPerBeat();
            double step = p.gridStepSamples();
            if (beat > 0.0 && step > 0.0)
            {
                const double sppx = juce::jmax (1.0, viewSpan) / (double) w;   // samples/pixel
                while (step / sppx < 6.0 && step < beat * 4.0) step *= 2.0;     // widen if too dense
                if (step / sppx >= 5.0)
                {
                    const double barLen = beat * 4.0;
                    const long long k0 = (long long) std::floor (viewStart / step);
                    const long long k1 = (long long) std::ceil ((viewStart + viewSpan) / step);
                    for (long long k = juce::jmax<long long> (0, k0); k <= k1; ++k)
                    {
                        const double pos = (double) k * step;
                        const float gx = sampleToX ((int) pos);
                        if (gx < 0.0f || gx > (float) w) continue;
                        const double rBar  = pos / barLen, rBeat = pos / beat;
                        const bool onBar  = std::abs (rBar  - std::round (rBar))  < 0.01;
                        const bool onBeat = std::abs (rBeat - std::round (rBeat)) < 0.01;
                        g.setColour (juce::Colours::white
                                     .withAlpha (onBar ? 0.20f : (onBeat ? 0.11f : 0.06f)));
                        g.drawVerticalLine ((int) gx, (float) top, (float) waveBottom);
                    }
                }
            }
        }

        // C2/D3: flags, active cue region, and playheads — shared full-height paint
        // used by BOTH the composite wave and the STEMS lanes (D3 scope: "render
        // full-height THROUGH the lanes... verify it runs in the STEMS branch").
        // Extracted verbatim (byte-identical body) into paintFlagsCuePlayheads() so
        // the STEMS branch can call the exact same code instead of duplicating it —
        // no second edit path, per ground rule 3.
        paintFlagsCuePlayheads (g, w, top, waveBottom, flagY, sel, sr);

        // C1: composite-view time ruler along the bottom of the wave, muted mono
        // (mockup drawComposite: rgba(155,161,168,.5) @ 10px mono, five even marks).
        // Composite-only (no stem lanes) — height-relative so it survives C3's
        // 160px hero without edits.
        if (bandH == 0 && sr > 0.0)
        {
            g.setColour (Theme::t2.withAlpha (0.5f));
            g.setFont (Theme::mono (10.0f * 1.12f));
            const int rulerY = waveBottom - 13;
            for (int t = 0; t <= 4; ++t)
            {
                const double frac = t / 4.0;
                const double secs = (viewStart + frac * viewSpan) / sr;
                const int mm = (int) (secs / 60.0);
                const int ssd = ((int) secs) % 60;
                const juce::String lbl = juce::String (mm) + ":" + juce::String (ssd).paddedLeft ('0', 2);
                const int tx = (int) (frac * (float) (w - 1));
                const auto just = t == 0 ? juce::Justification::bottomLeft
                                : t == 4 ? juce::Justification::bottomRight
                                         : juce::Justification::centredBottom;
                g.drawText (lbl, juce::jlimit (0, w - 44, tx - 22), rulerY, 44, 12, just);
            }
        }

        // scrollbar strip
        g.setColour (juce::Colour (0xff090a0b));
        g.fillRect (0, waveBottom, w, sb);
        const float thS = (float) (viewStart / (double) cachedLen) * (float) w;
        const float thE = (float) ((viewStart + viewSpan) / (double) cachedLen) * (float) w;
        g.setColour (juce::Colour (0xff9a9da1).withAlpha (0.7f));
        g.fillRoundedRectangle (thS, (float) waveBottom + 2.0f,
                                juce::jmax (8.0f, thE - thS), (float) sb - 4.0f, 3.0f);

        // zoom hint
        if (viewSpan < (double) cachedLen)
        {
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.setFont (10.0f);
            g.drawText ("double-click: full view", w - 130, top + 2, 126, 12, juce::Justification::right);
        }
    }

    // ------------------------------------------------------------ interaction
    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = DragMode::none;
        dragPad = -1;
        if (cachedLen <= 0) { if (onRequestLoad) onRequestLoad(); return; }

        // D2 R2-fix: the STEMS-placeholder "SEPARATE STEMS" CTA, BEFORE any other
        // hit-test. Guarded on ctaRect being non-empty, which paint() guarantees
        // is true ONLY on the frames where the placeholder is actually the
        // painted content (STEMS requested, stems not yet ready, a source is
        // loaded) -- see paint()'s unconditional top-of-frame ctaRect reset, so a
        // stale rect from a different state can never eat a click here.
        if (! ctaRect.isEmpty() && ctaRect.contains (e.position))
        {
            if (onRequestStemSeparation) onRequestStemSeparation();
            return;
        }

        const int w = getWidth(), h = getHeight();
        const int sb = 11;

        // D4: STEMS-view lane mute/solo x-zones. Live ONLY when the painted
        // view is STEMS (heroReq==1, per paint()'s own branch selector --
        // gent::sanitizeHeroView(p.heroView.load()), same predicate) AND real
        // lanes are showing (stemsReady == gent::resolveHeroView(...)==1; the
        // placeholder/CTA path above already claimed its own click via ctaRect
        // and returned, so reaching here with heroReq==1 && !stemsReady means
        // the click landed outside the CTA chip -- no lane geometry exists to
        // test in that state, so it falls through same as COMPOSITE).
        //
        // Hit-test priority (docs/STEM_VIEW_MODEL.md SS8, point 1-2):
        //   1. lane plate (mute) / solo-box zones claim clicks ONLY within
        //      their own x-range, at that lane's y -- gent::laneZoneAt is the
        //      SAME x-classification the dormant band code used (D4 ctest:
        //      exact boundary constants extracted verbatim), composed with
        //      gent::laneIndexAt for the y->lane mapping (D3 extraction).
        //   2. everything else (including clicks inside a lane's y-range but
        //      outside its mute/solo x-zones -- the wave-column middle of a
        //      lane) FALLS THROUGH unchanged to scrollbar / pan / flag-select
        //      / handle-drag / placeStart below -- the exact fix for the
        //      whole-band-early-return landmine (REDESIGN_TASK_D.md landmine
        //      flag 3): today's `return` for ANY in-band y is now a narrower
        //      return that only fires for the two real hit zones.
        // The dormant whole-band block (pre-D4: `if (stemBandH > 0 && e.y >=
        // stemBandTop && e.y < stemBandTop + stemBandH) { ...unconditional
        // return... }`) is deleted -- its replacement is this block, gated on
        // heroReq/stemsReady + laneIndexAt/laneZoneAt instead of stemBandH.
        const int heroReq = gent::sanitizeHeroView (p.heroView.load());
        const int waveBottom = h - sb - bottomChromeH;
        const bool stemsReady = heroReq == 1 && gent::resolveHeroView (heroReq, p.hasStems()) == 1;
        if (stemsReady && e.y >= 0 && e.y < waveBottom)
        {
            const int lane = gent::laneIndexAt (e.y, 0, waveBottom);
            const auto zone = gent::laneZoneAt (e.x, w, stemLabW, stemSoloW);
            if (zone == gent::LaneZone::solo)
            {
                p.setStemSoloed (lane, ! p.isStemSoloed (lane));
                ++p.uiDirty;
                repaint();
                return;
            }
            if (zone == gent::LaneZone::mute)
            {
                p.setStemMuted (lane, ! p.isStemMuted (lane));
                ++p.uiDirty;
                repaint();
                return;
            }
            // zone == wave: falls through to the shared paths below (scrollbar,
            // pan, flag-select, handle drags, placeStart) -- no early return.
        }

        if (e.y >= h - sb - bottomChromeH)                   // scrollbar (shifted above the chip band)
        {
            drag = DragMode::scroll;
            const double clickedSample = (double) e.x / (double) juce::jmax (1, w) * (double) cachedLen;
            if (clickedSample < viewStart || clickedSample > viewStart + viewSpan)
                setView (clickedSample - viewSpan * 0.5, viewSpan);
            scrollGrab = (double) e.x / (double) juce::jmax (1, w) * (double) cachedLen - viewStart;
            return;
        }

        if (e.mods.isMiddleButtonDown())                     // middle-drag pans
        {
            drag = DragMode::pan;
            panAnchorX = e.x;
            panAnchorStart = viewStart;
            return;
        }

        // flag pennants: click one to SELECT its pad (then its window/handles are
        // editable). Tested before the edge hit-tests (which are x-only) so a flag
        // click selects rather than grabbing the start edge underneath it.
        //
        // D4 fix (STEM_VIEW_MODEL.md SS8, "FIX THE D3-FLAGGED STALE GEOMETRY"):
        // in STEMS view, paint() calls paintFlagsCuePlayheads(..., flagY=0, ...)
        // (full-height pennants painted from the lane area's own top, per D3),
        // NOT the composite's flagBarY/flagBarH row. Testing against composite's
        // flagBarY/flagBarH here in STEMS view would hit-test against the WRONG
        // geometry (flagBarY collapses to 2 whenever a band isn't showing, per
        // D3's bandH==0 comment above -- coincidentally close to STEMS' own 0,
        // but flagBarH's 15 is the right SIZE regardless; only the origin
        // differs). Use the STEMS-correct pennant-row zone [0, flagH) --
        // consistent with composite's own "pennant-row-only" convention, just
        // anchored at the STEMS branch's flagY=0 instead of composite's flagY=2.
        const int flagHitY = (heroReq == 1) ? 0 : flagBarY;   // STEMS: flagY=0; COMPOSITE: unchanged
        if (e.y >= flagHitY && e.y < flagHitY + flagBarH)
        {
            for (int i = 15; i >= 0; --i)   // top-most flag (highest index, drawn last) wins the click
            {
                if (p.getCue (i) < 0) continue;
                const float xs = sampleToX (p.getCue (i));
                if (xs < -16.0f || xs > (float) w) continue;   // off-screen: no pennant drawn, skip
                const float fw = (i == p.selectedPad.load()) ? 22.0f : 18.0f;
                const float fx = juce::jlimit (0.0f, (float) w - fw, xs);
                if ((float) e.x >= fx && (float) e.x <= fx + fw)
                {
                    p.selectedPad = i;
                    ++p.uiDirty;
                    repaint();
                    return;
                }
            }
        }

        // 1A: the selected pad's own start/end handles win a shared boundary —
        // test them BEFORE any other pad's handle (so clicking pad5.end / pad6.start
        // grabs the selected pad's edge, not whichever the scan finds first).
        {
            const int sel = p.selectedPad.load();
            if (p.getCue (sel) >= 0)
            {
                const float ds = std::abs (sampleToX (p.getCue (sel)) - (float) e.x);
                const float de = std::abs (endHandleX (sel)         - (float) e.x);
                if (de <= 5.0f && de <= ds)                 // selected end handle (closer wins)
                {
                    if (e.mods.isRightButtonDown())
                    { p.pushUndo(); p.setCueEnd (sel, -1); repaint(); return; }
                    // F1: no pushUndo/setCueEnd here — arm the engine only, edit is lazy.
                    drag = DragMode::endEdge; dragPad = sel;
                    handleDragBegin (dragEngine, p, sel, HandleDragEngine::Handle::end, e.x,
                                     xToSample (juce::roundToInt (endHandleX (sel))));
                    if (onHandleGrabbed) onHandleGrabbed (HandleDragEngine::Handle::end);
                    return;
                }
                if (ds <= 5.0f)                                   // selected start edge
                {
                    drag = DragMode::startEdge; dragPad = sel;
                    handleDragBegin (dragEngine, p, sel, HandleDragEngine::Handle::cue, e.x);
                    if (onHandleGrabbed) onHandleGrabbed (HandleDragEngine::Handle::cue);
                    return;
                }
            }
        }

        // end handles (right-click resets one to auto)
        const int hp = hitEndHandle (e.x);
        if (hp >= 0)
        {
            if (e.mods.isRightButtonDown())
            {
                p.pushUndo();
                p.setCueEnd (hp, -1);
                repaint();
                return;
            }
            p.selectedPad = hp;
            drag = DragMode::endEdge;
            dragPad = hp;
            handleDragBegin (dragEngine, p, hp, HandleDragEngine::Handle::end, e.x,
                              xToSample (juce::roundToInt (endHandleX (hp))));
            if (onHandleGrabbed) onHandleGrabbed (HandleDragEngine::Handle::end);
            return;
        }

        // start edges
        const int sp = hitStartEdge (e.x);
        if (sp >= 0)
        {
            p.selectedPad = sp;
            drag = DragMode::startEdge;
            dragPad = sp;
            handleDragBegin (dragEngine, p, sp, HandleDragEngine::Handle::cue, e.x);
            if (onHandleGrabbed) onHandleGrabbed (HandleDragEngine::Handle::cue);
            return;
        }

        // otherwise: place the audition cursor here (scrubs preview if running).
        // SNAP pulls the live cursor onto grid lines + placed cues.
        drag = DragMode::placeStart;
        dragPad = -1;
        const int s = p.snapCursor (xToSample (e.x));
        p.setAssignCursor (s);
        if (p.isPreviewing())
            p.startPreview (s);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (cachedLen <= 0) return;
        switch (drag)
        {
            case DragMode::scroll:
            {
                const double s = (double) e.x / (double) juce::jmax (1, getWidth()) * (double) cachedLen;
                setView (s - scrollGrab, viewSpan);
                break;
            }
            case DragMode::pan:
            {
                const double spp = viewSpan / (double) juce::jmax (1, getWidth());
                setView (panAnchorStart - (double) (e.x - panAnchorX) * spp, viewSpan);
                break;
            }
            case DragMode::startEdge:
            case DragMode::endEdge:
                if (dragPad >= 0 && dragEngine.isActive())
                {
                    // F1: relative-drag engine — same accumulator/lazy-undo/snap decision
                    // for both handles, one call site, no per-surface fork (ground rule 1).
                    // Collapse affordance is still "~8 screen px of the start line", converted
                    // to THIS view's own sample-space tolerance so the byte-for-byte behaviour
                    // at every zoom level is unchanged (8px * current samples-per-pixel).
                    const double samplesPerPixel = viewSpan / (double) juce::jmax (1, getWidth());
                    const int tol = (int) (8.0 * samplesPerPixel);
                    // F2: Shift = fine mode, 0.10x rate, read per mouse event so mid-drag
                    // press/release re-anchors implicitly via the accumulator (no jump).
                    const double rate = e.mods.isShiftDown() ? 0.10 : 1.0;
                    // F3: Alt bypasses snap for this event only, read per event (same
                    // pattern as rate) so releasing Alt mid-drag re-engages capture.
                    const bool altDown = e.mods.isAltDown();
                    handleDragMove (dragEngine, p, e.x, samplesPerPixel, tol, rate, altDown);
                    repaint();
                }
                break;
            case DragMode::placeStart:
            {
                const int s = p.snapCursor (xToSample (e.x));   // SNAP: grid + placed cues
                p.setAssignCursor (s);
                if (p.isPreviewing())
                    p.startPreview (s);
                repaint();
                break;
            }
            case DragMode::none:
            default:
                break;
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        handleDragEnd (dragEngine);
        drag = DragMode::none;
        dragPad = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        fullView();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (cachedLen <= 0) return;
        if (e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY))
        {
            const float d = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
            setView (viewStart - (double) d * viewSpan * 1.5, viewSpan);
            return;
        }
        // zoom anchored at the mouse position
        const double anchor = xToSampleD (e.x);
        const double factor = std::pow (0.82, (double) wheel.deltaY * 10.0);
        const double newSpan = juce::jlimit (256.0, (double) cachedLen, viewSpan * factor);
        const double frac = (double) e.x / (double) juce::jmax (1, getWidth());
        setView (anchor - frac * newSpan, newSpan);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (cachedLen <= 0) { setMouseCursor (juce::MouseCursor::PointingHandCursor); return; }

        // D2 R2-fix: pointing-hand feedback over the STEMS-placeholder CTA, same
        // ctaRect paint()'s hover tint reads. Repaint ONLY on a hover-state
        // transition (ctaHoverLast), same cheap-dirty-watch idiom as hoverLane
        // just below -- not on every pixel of mouse movement while hovering.
        if (! ctaRect.isEmpty())
        {
            const bool overCta = ctaRect.contains (e.position);
            setMouseCursor (overCta ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
            if (overCta != ctaHoverLast) { ctaHoverLast = overCta; repaint(); }
            if (overCta) return;
        }

        const bool edge = hitEndHandle (e.x) >= 0 || hitStartEdge (e.x) >= 0;
        setMouseCursor (edge ? juce::MouseCursor::LeftRightResizeCursor
                             : juce::MouseCursor::NormalCursor);
        // D4: rewired to the real STEMS-view full-lane geometry (the dormant
        // stemBandH/stemBandTop band test is deleted -- stemBandH is always 0
        // now that the composite band paint is retired, D3). hoverLane feeds
        // paint()'s hover-reveal S-box (D3 paint, "hoverLane == s || soloed"),
        // so it must track the SAME (0, waveBottom) lane geometry mouseDown's
        // laneZoneAt/laneIndexAt block above uses, gated the same way (STEMS
        // painted view + real lanes showing -- no hover reveal makes sense
        // over the placeholder, which has no per-lane wave data to reveal).
        const int heroReqHover = gent::sanitizeHeroView (p.heroView.load());
        const bool stemsReadyHover = heroReqHover == 1
                                    && gent::resolveHeroView (heroReqHover, p.hasStems()) == 1;
        const int waveBottomHover = getHeight() - 11 - bottomChromeH;
        int hl = -1;
        if (stemsReadyHover && e.y >= 0 && e.y < waveBottomHover)
            hl = gent::laneIndexAt (e.y, 0, waveBottomHover);
        if (hl != hoverLane) { hoverLane = hl; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverLane != -1) { hoverLane = -1; repaint(); }
        if (ctaHoverLast) { ctaHoverLast = false; repaint(); }   // D2 R2-fix
    }

private:
    enum class DragMode { none, placeStart, startEdge, endEdge, scroll, pan };

    // -------------------------------------------------------------- mapping
    float sampleToX (int s) const
    {
        return (float) (((double) s - viewStart) / juce::jmax (1.0, viewSpan) * (double) getWidth());
    }
    double xToSampleD (int x) const
    {
        return viewStart + (double) x / (double) juce::jmax (1, getWidth()) * viewSpan;
    }
    int xToSample (int x) const
    {
        return juce::jlimit (0, juce::jmax (0, cachedLen - 1), (int) xToSampleD (x));
    }

    int hitStartEdge (int mx) const
    {
        const int sel = p.selectedPad.load();
        if (cachedLen <= 0) return -1;
        if (p.getCue (sel) >= 0 && std::abs (sampleToX (p.getCue (sel)) - (float) mx) <= 5.0f) return sel;
        for (int i = 0; i < 16; ++i)
            if (p.getCue (i) >= 0 && std::abs (sampleToX (p.getCue (i)) - (float) mx) <= 5.0f) return i;
        return -1;
    }
    // x of a pad's end handle. An OPEN/collapsed slice has no window end — its grip
    // sits just right of the start line (drag it right to re-open into a window).
    float endHandleX (int i) const
    {
        if (p.isOpenSlice (i)) return sampleToX (p.getCue (i)) + 14.0f;
        return sampleToX (p.getEffectiveCueEnd (i));
    }
    int hitEndHandle (int mx) const
    {
        const int sel = p.selectedPad.load();
        if (cachedLen <= 0) return -1;
        if (p.getCue (sel) >= 0 && std::abs (endHandleX (sel) - (float) mx) <= 5.0f) return sel;
        for (int i = 0; i < 16; ++i)
            if (p.getCue (i) >= 0 && std::abs (endHandleX (i) - (float) mx) <= 5.0f) return i;
        return -1;
    }

    // C2/D3: flags, active cue region, and playheads — extracted verbatim (byte-
    // identical body) from the pre-D3 composite paint so BOTH the composite wave
    // and the D3 STEMS lanes can call the exact same painting code (D3 scope:
    // "the existing C2 painting — verify it runs in the STEMS branch, z-above
    // lanes"). No logic changed here, only parameterized on (top, waveBottom,
    // flagY) so each caller supplies its own vertical geometry -- composite keeps
    // its existing flagY-row-above-wave layout; the STEMS branch passes top=0/
    // flagY=0 so flags/cue-region/playheads span the full lane area, matching the
    // mockup's drawFlags(g,W,H) full-canvas-height treatment (docs/STEM_VIEW_MODEL.md
    // SS2, "full-height...not just a flagBarY strip").
    void paintFlagsCuePlayheads (juce::Graphics& g, int w, int top, int waveBottom,
                                 int flagY, int sel, double sr)
    {
        // C2: active cue region — the selected pad's slice window gets the amber
        // translucent fill + glowing border (mockup drawFlags: fillStyle
        // rgba(232,153,58,.10) then strokeRect rgba(255,184,92,.65) w2). Only when
        // the selected pad has a real closed window (open/gated slices have no
        // window to fill — C3's strip covers that case with its OPEN tag). Painted
        // before the flags/handles loop (wave < cue region < flags/handles) so the
        // region doesn't overdraw the selected pad's line glow or end-handle bracket.
        if (p.getCue (sel) >= 0 && ! p.isOpenSlice (sel))
        {
            const float rs = sampleToX (p.getCue (sel));
            const float re = sampleToX (p.getEffectiveCueEnd (sel));
            const float ra = juce::jmin (rs, re), rb = juce::jmax (rs, re);
            if (rb >= 0.0f && ra <= (float) w)
            {
                const float ca = juce::jmax (0.0f, ra), cb = juce::jmin ((float) w, rb);
                auto region = juce::Rectangle<float> (ca, (float) top, cb - ca, (float) (waveBottom - top));
                g.setColour (Theme::accent.withAlpha (0.10f));
                g.fillRect (region);
                g.setColour (Theme::glow.withAlpha (0.65f));
                g.drawRect (region.reduced (1.0f), 2.0f);
            }
        }

        // region edges, flags, end handles + active-window highlight
        for (int i = 0; i < 16; ++i)
        {
            if (p.getCue (i) < 0) continue;                  // unassigned
            const auto col = padMapHue (i, p);                // C2: neutral for FULL/multi-stem
            const bool open   = p.isOpenSlice (i);
            const bool selPad = (i == sel);
            const float xs = sampleToX (p.getCue (i));
            const float xe = endHandleX (i);                 // window end, or open re-open grip
            const bool explicitEnd = p.getCueEnd (i) >= 0;   // open (== kOpenSlice) reads as false
            const float a = selPad ? 1.0f : 0.55f;

            if (xs >= -16.0f && xs <= (float) w)
            {
                // C2: soft hue glow behind the slice line (mockup drawFlags: stroked at
                // globalAlpha .85 with the flag's shadowColor bleeding onto it) —
                // feathered fills widening outward from the line, cheapest bloom that
                // survives JUCE's lack of a shadowBlur primitive.
                for (int gl = 3; gl >= 1; --gl)
                {
                    const float gw = (float) gl * 2.6f;
                    const float galpha = a * 0.16f / (float) gl;
                    g.setColour (col.withAlpha (galpha));
                    g.fillRect (xs - gw * 0.5f, (float) top, gw, (float) (waveBottom - top));
                }

                // start line (selected is brighter + 2px so the active edge stands out)
                g.setColour (col.withAlpha (a));
                if (selPad) g.fillRect ((float) xs - 1.0f, (float) top, 2.0f, (float) (waveBottom - top));
                else        g.drawVerticalLine ((int) xs, (float) top, (float) waveBottom);

                // angled pennant flag (map marker) sits just above the waveform
                const float fw = selPad ? 22.0f : 18.0f;
                const float fh = selPad ? 16.0f : 14.0f;
                const float fy = (float) flagY;
                const float fx = juce::jlimit (0.0f, (float) w - fw, xs);
                juce::Path flag;
                flag.startNewSubPath (fx, fy);
                flag.lineTo (fx + fw, fy);
                flag.lineTo (fx + fw - fh * 0.5f, fy + fh * 0.5f);
                flag.lineTo (fx + fw, fy + fh);
                flag.lineTo (fx, fy + fh);
                flag.closeSubPath();
                // flag glow (mockup shadowColor=col, shadowBlur=9): reuse the shared
                // feathered-rect bloom against the flag's bounding box.
                Theme::featherGlow (g, flag.getBounds().expanded (1.0f), 2.0f,
                                    col.withAlpha (0.5f), 6.0f, 4);
                g.setColour (selPad ? col.brighter (0.25f) : col);
                g.fillPath (flag);
                g.setColour (juce::Colours::black.withAlpha (0.92f));
                g.setFont (juce::Font (selPad ? 11.0f : 10.0f, juce::Font::bold));
                g.drawText (juce::String (i + 1), (int) fx, (int) fy, (int) (fw - fh * 0.4f), (int) fh,
                            juce::Justification::centred);

                // selected territory tag: "CUE n · 14.2s", or "CUE n · OPEN" when gated
                if (selPad && sr > 0.0)
                {
                    const juce::String tag = open
                        ? ("CUE " + juce::String (i + 1) + "  OPEN")
                        : ("CUE " + juce::String (i + 1)
                           + juce::String::formatted ("  %.1fs",
                                 (double) (p.getEffectiveCueEnd (i) - p.getCue (i)) / sr));
                    g.setFont (juce::Font (9.0f, juce::Font::bold));
                    const float tw = g.getCurrentFont().getStringWidthFloat (tag) + 12.0f;
                    auto tr = juce::Rectangle<float> (juce::jlimit (0.0f, (float) w - tw, xs + 2.0f),
                                                      (float) top + 3.0f, tw, 15.0f);
                    g.setColour (Theme::well.withAlpha (0.85f));
                    g.fillRoundedRectangle (tr, 4.0f);
                    g.setColour (Theme::accent);
                    g.drawText (tag, tr, juce::Justification::centred);
                }
            }

            if (open)
            {
                // OPEN/gated slice: a ▶ gate affordance just right of the start line
                // (drag the faint grip right to re-open into a window). No closed region.
                if (xs >= -16.0f && xs <= (float) w)
                {
                    const float gy = (float) top + (float) (waveBottom - top) * 0.5f;
                    juce::Path tri;
                    tri.addTriangle (xs + 4.0f, gy - 6.0f, xs + 4.0f, gy + 6.0f, xs + 13.0f, gy);
                    g.setColour (col.withAlpha (selPad ? 0.95f : 0.6f));
                    g.fillPath (tri);
                    g.setColour (col.withAlpha (selPad ? 0.5f : 0.3f));   // the re-open grip
                    g.drawVerticalLine ((int) (xs + 14.0f), gy - 9.0f, gy + 9.0f);
                }
            }
            else if (xe >= 0.0f && xe <= (float) w)
            {
                g.setColour (col.withAlpha (explicitEnd ? a : a * 0.45f));
                g.drawVerticalLine ((int) xe, (float) top, (float) waveBottom);
                // end handle: small bracket near the bottom
                juce::Path ph;
                ph.addTriangle (xe, (float) waveBottom - 12.0f,
                                xe - 7.0f, (float) waveBottom,
                                xe, (float) waveBottom);
                g.fillPath (ph);
            }
        }

        // assign cursor: dashed cream marker
        {
            const int ac = p.getAssignCursor();
            if (ac >= 0)
            {
                const float ax = sampleToX (ac);
                if (ax >= 0.0f && ax <= (float) w)
                {
                    g.setColour (Theme::glow.withAlpha (0.8f));
                    const float dash[2] = { 5.0f, 4.0f };
                    g.drawDashedLine ({ { ax, (float) top }, { ax, (float) waveBottom } }, dash, 2, 1.4f);
                }
            }
        }

        // preview playhead: bright cream line
        {
            const int pv = p.getPreviewPos();
            if (pv >= 0)
            {
                const float vx = sampleToX (pv);
                if (vx >= 0.0f && vx <= (float) w)
                {
                    g.setColour (Theme::glow);
                    g.drawVerticalLine ((int) vx, (float) top, (float) waveBottom);
                    g.setColour (Theme::glow.withAlpha (0.25f));
                    g.fillRect (vx - 1.5f, (float) top, 3.0f, (float) (waveBottom - top));
                }
            }
        }

        // hint when nothing is assigned yet
        {
            bool anyAssigned = false;
            for (int i = 0; i < 16 && ! anyAssigned; ++i)
                anyAssigned = p.getCue (i) >= 0;
            if (! anyAssigned)
            {
                g.setColour (Theme::accent.withAlpha (0.8f));
                g.setFont (juce::Font (11.0f, juce::Font::bold));
                g.drawText ("click/scrub to a spot, then tap an empty pad to drop its cue",
                            6, top + 2, w - 12, 14, juce::Justification::left);
            }
        }

        // C2: glowing playheads — one moving line per sounding pad, in that pad's
        // colour. Mockup's drawFlags playhead: a horizontal linear-gradient bloom
        // (transparent -> col @.30 -> transparent, +-14px) behind a bright 3px core.
        for (int i = 0; i < 16; ++i)
        {
            const int pp = p.getPadPlayPos (i);
            if (pp < 0) continue;
            const float px = sampleToX (pp);
            if (px < 0.0f || px > (float) w) continue;
            const auto col = padMapHue (i, p);                // C2: neutral for FULL/multi-stem
            const auto bloom = col.brighter (0.35f);
            juce::ColourGradient pg (bloom.withAlpha (0.0f), px - 14.0f, 0.0f,
                                     bloom.withAlpha (0.0f), px + 14.0f, 0.0f, false);
            pg.addColour (0.5, bloom.withAlpha (0.30f));
            g.setGradientFill (pg);
            g.fillRect (px - 14.0f, (float) top, 28.0f, (float) (waveBottom - top));
            g.setColour (col.brighter (0.7f).withAlpha (0.95f));
            g.fillRect (px - 1.5f, (float) top, 3.0f, (float) (waveBottom - top));
            juce::Path tip;
            tip.addTriangle (px - 4.0f, (float) top, px + 4.0f, (float) top, px, (float) top + 6.0f);
            g.fillPath (tip);
        }
    }

    void setView (double newStart, double newSpan)
    {
        newSpan = juce::jlimit (256.0, (double) juce::jmax (256, cachedLen), newSpan);
        newStart = juce::jlimit (0.0, juce::jmax (0.0, (double) cachedLen - newSpan), newStart);
        if (std::abs (newStart - viewStart) < 0.5 && std::abs (newSpan - viewSpan) < 0.5)
            return;
        viewStart = newStart;
        viewSpan = newSpan;
        rebuildPeaks();
    }

    // ----------------------------------------------------------------- data
    void timerCallback() override
    {
        auto s = p.getSource();
        if (s.get() != cachedSrc || getWidth() != cachedW)
        {
            cachedSrc = s.get();
            cachedW = getWidth();
            cachedLen = s != nullptr ? s->buffer.getNumSamples() : 0;
            keep = s;                                        // hold a reference for safe reads
            viewStart = 0.0;
            viewSpan = juce::jmax (256.0, (double) cachedLen);
            rebuildPeaks();
        }
        const int d = p.uiDirty.load();
        if (d != lastDirty) { lastDirty = d; repaint(); }

        // C4: the map's selected-pad cue region / bold line / bold flag depend on
        // p.selectedPad, but selection changes (pad-grid click on an ALREADY-assigned
        // pad, or a MIDI-triggered selection with follow off) don't always bump
        // uiDirty or lastTriggerCount — watch it directly here, same pattern
        // SliceDetailStrip already uses for its own selection-dependent hash, so the
        // map never lags the strip by more than one 30Hz tick.
        {
            const int sel = p.selectedPad.load();
            if (sel != lastSel) { lastSel = sel; repaint(); }
        }

        // D2: repaint when the EFFECTIVE hero view changes without a click --
        // stems can arrive/vanish asynchronously (separation completing on the
        // worker thread, or a new file load dropping hasStems() to false) while
        // the sticky request stays constant; fold both message-thread-only reads
        // into one dirty hash, same lastSel/lastGrainHash precedent above/at
        // SliceDetailStrip. Message-thread-only: heroView.load() and hasStems()
        // are both read here on the UI timer, never from processBlock.
        {
            const int hv = gent::sanitizeHeroView (p.heroView.load());
            const int hh = hv * 2 + (p.hasStems() ? 1 : 0);
            if (hh != lastHeroHash) { lastHeroHash = hh; repaint(); }
        }

        // refresh stem ribbons when separation finishes / stems change
        {
            auto ss = p.getStems();
            const int sg = ss != nullptr ? ss->generation : -1;
            if (sg != lastStemGen) { lastStemGen = sg; rebuildStemPeaks(); repaint(); }
        }

        // snap the view to the most recently triggered pad's region -- but only
        // on a FRESH ASSIGNMENT or a DIFFERENT pad than the previous trigger
        // (PREPACK_UX U1); a re-trigger of the same already-assigned pad must
        // never move the view (Joe: hero zoom should hold while inspecting).
        const int trig = p.lastTriggerCount.load();
        if (trig != lastTrig)
        {
            lastTrig = trig;
            const int padIdx = p.lastTriggerPad.load();
            const int asg = p.lastAssignCount.load();
            const bool freshAssign = (asg != lastAssign);
            const bool newPad = (padIdx != lastTrigPadSeen);
            lastAssign = asg;
            lastTrigPadSeen = padIdx;
            if (follow && padIdx >= 0 && cachedLen > 0 && (freshAssign || newPad))
            {
                const double s = (double) p.getCue (padIdx);
                const double e = (double) p.getEffectiveCueEnd (padIdx);
                const double regionLen = juce::jmax (256.0, e - s);
                const double span = juce::jmax (256.0, regionLen * 1.3);
                setView (s - (span - regionLen) * 0.5, span);
            }
        }

        // keep playheads moving smoothly while anything is sounding
        bool any = p.getPreviewPos() >= 0;
        for (int i = 0; i < 16 && ! any; ++i)
            any = p.getPadPlayPos (i) >= 0;
        if (any || wasPlaying)
            repaint();
        wasPlaying = any;
    }

    void rebuildPeaks()
    {
        peaks.clear();
        if (keep == nullptr || cachedW <= 0 || cachedLen <= 0)
        {
            repaint();
            return;
        }
        const auto& b = keep->buffer;
        const float* d = b.getReadPointer (0);
        peaks.resize ((size_t) cachedW, { 0.0f, 0.0f });
        const double spp = viewSpan / (double) cachedW;
        // decimate when zoomed way out so rebuilds stay instant
        const int stride = juce::jmax (1, (int) (spp / 1024.0));

        for (int x = 0; x < cachedW; ++x)
        {
            const int a  = juce::jlimit (0, cachedLen - 1, (int) (viewStart + spp * x));
            const int e2 = juce::jlimit (a + 1, cachedLen, (int) (viewStart + spp * (x + 1)) + 1);
            float lo = 0.0f, hi = 0.0f;
            for (int i = a; i < e2; i += stride)
            {
                lo = juce::jmin (lo, d[i]);
                hi = juce::jmax (hi, d[i]);
            }
            peaks[(size_t) x] = { lo, hi };
        }
        rebuildStemPeaks();
        repaint();
    }

    void rebuildStemPeaks()
    {
        for (auto& sp : stemPeaks) sp.clear();
        auto ss = p.getStems();
        keepStems = ss;
        if (ss == nullptr || cachedW <= 0 || cachedLen <= 0)
            return;
        const double spp = viewSpan / (double) cachedW;
        for (int s = 0; s < 6; ++s)
        {
            const auto& b = ss->buffers[(size_t) s];
            const int sl = b.getNumSamples();
            if (sl <= 0 || b.getNumChannels() < 1) continue;
            const float* d = b.getReadPointer (0);
            const double ratio = (double) sl / (double) cachedLen;
            auto& out = stemPeaks[(size_t) s];
            out.resize ((size_t) cachedW, { 0.0f, 0.0f });
            const int stride = juce::jmax (1, (int) (spp * ratio / 1024.0));
            for (int x = 0; x < cachedW; ++x)
            {
                const int a  = juce::jlimit (0, sl - 1, (int) ((viewStart + spp * x) * ratio));
                const int e2 = juce::jlimit (a + 1, sl, (int) ((viewStart + spp * (x + 1)) * ratio) + 1);
                float lo = 0.0f, hi = 0.0f;
                for (int i = a; i < e2; i += stride) { lo = juce::jmin (lo, d[i]); hi = juce::jmax (hi, d[i]); }
                out[(size_t) x] = { lo, hi };
            }
        }
    }

    GentSamplerAudioProcessor& p;
    SourceSample::Ptr keep;
    void* cachedSrc = nullptr;
    int cachedW = 0, cachedLen = 0, lastDirty = -1;
    int lastSel = -1;   // C4: watched independently so bare selection changes repaint
    int lastHeroHash = -1;   // D2: sanitizeHeroView()*2 + hasStems() -- effective-view dirty watch
    double viewStart = 0.0, viewSpan = 1.0;
    std::vector<std::pair<float, float>> peaks;
    std::array<std::vector<std::pair<float, float>>, 6> stemPeaks;
    StemSet::Ptr keepStems;
    int lastStemGen = -2;
    int stemBandTop = 18, stemBandH = 0;   // D4: set by the composite paint branch (bandH==0 layout);
                                            // no longer read by any hit-test -- STEMS-view lane hit-tests
                                            // (mouseDown/mouseMove) now compute their own (0, waveBottom)
                                            // geometry locally instead of consuming these fields.
    int flagBarY = 0, flagBarH = 15;        // flag pennant row (set in paint), used for flag clicks
                                            // (COMPOSITE uses flagBarY directly; STEMS uses flagBarH only,
                                            // with its own y-origin of 0 -- see mouseDown's flagHitY)
    int hoverLane = -1;                     // stem lane under the mouse (for solo reveal)
    int stemLabW = 60, stemSoloW = 18;   // C3 review nit: stemRulerH removed (dead — rulerH is hardcoded 0)
    int bottomChromeH = 0;                  // C2: reserved band for the .ov.bl/.ov.br chip plates

    // D2 R2-fix: the STEMS-placeholder "SEPARATE STEMS" CTA's painted rect, set
    // ONLY on the paint frames where the placeholder is actually drawn (STEMS
    // requested AND stems not yet ready), and explicitly invalidated (empty
    // rect) on every other paint (COMPOSITE view, stems ready, no source) so a
    // stale rect from a prior state can never eat a click in a state where no
    // CTA is on screen -- mouseDown/mouseMove gate on ctaRect.isEmpty() before
    // hit-testing it.
    juce::Rectangle<float> ctaRect;
    bool ctaHoverLast = false;   // D2 R2-fix: mouseMove's dirty-watched hover state for ctaRect

    DragMode drag = DragMode::none;
    int dragPad = -1;
    HandleDragEngine dragEngine;   // F1: shared relative-drag state for CUE/END gestures
    bool follow = true, wasPlaying = false;
    int lastTrig = -1;
    int lastAssign = -1;         // PREPACK_UX U1: last-seen p.lastAssignCount -- fresh-assign detector
    int lastTrigPadSeen = -1;    // PREPACK_UX U1: last-seen p.lastTriggerPad -- different-pad detector
    double scrollGrab = 0.0, panAnchorStart = 0.0;
    int panAnchorX = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

// ---------------------------------------------------------------------------
//  SliceDetailStrip — C3: the selected pad's slice, zoomed to its region (+-15%
//  context each side), with functionally draggable CUE/END handles. Every edit
//  routes through the SAME processor calls the main-map WaveformView handles use
//  (p.pushUndo() / p.setCue() / p.setCueEnd() / p.snapEnabled() / nearestGridLine()
//  / nearestTransient()) — this component owns no separate edit/undo/snap path.
//  Layout per mockup `.detail`: dmeta (118px, left) | dwave (flex, centre) |
//  dread (150px, right).
class SliceDetailStrip : public juce::Component,
                         private juce::Timer
{
public:
    explicit SliceDetailStrip (GentSamplerAudioProcessor& proc) : p (proc)
    {
        setOpaque (false);
        setWantsKeyboardFocus (true);   // F4: clicking the strip puts keyboard focus in the
                                         // plugin so unhandled keys (arrows) bubble to the
                                         // editor's keyPressed (single handler, ground rule 1)
        startTimerHz (30);
    }

    // F4: fired right after handleDragBegin arms a CUE/END gesture from a mouse grab on
    // this surface, so the editor can mirror it into its own armed-handle nudge target
    // (SLICE_FEEL_TASK.md F4 — "set also when a handle is grabbed by mouse on EITHER
    // surface"). Not fired for the grain-marker drag (F5's concern, not a nudge target).
    std::function<void (HandleDragEngine::Handle)> onHandleGrabbed;

    // F4: armed-handle affordance — the editor owns the armed-handle state (nudge
    // target) and pushes it here purely for the strip's own paint() to render the ring;
    // the strip has no nudge logic of its own (ground rule 1: one decision tree).
    void setArmedHandle (HandleDragEngine::Handle h)   { if (h != armedHandle) { armedHandle = h; repaint(); } }

    // F4: default nudge increment = 1 strip-pixel = stripSpp samples, read directly from
    // the strip's own current zoom window ((zoomHi-zoomLo)/(waveR-waveL)) — the editor
    // has no independent notion of the strip's zoom, so it asks the strip for it rather
    // than re-deriving/duplicating the computation (ground rule 1).
    double stripSpp() const
    {
        return (double) juce::jmax (1, zoomHi - zoomLo) / (double) juce::jmax (1, waveR - waveL);
    }

    void paint (juce::Graphics& g) override
    {
        const int w = getWidth(), h = getHeight();
        const int metaW = 118, readW = 150;
        const auto metaR = juce::Rectangle<int> (0, 0, metaW, h);
        const auto readR = juce::Rectangle<int> (w - readW, 0, readW, h);
        waveL = metaW; waveR = w - readW;
        const auto waveRect = juce::Rectangle<int> (waveL, 0, juce::jmax (0, waveR - waveL), h);

        const int sel = p.selectedPad.load();
        const bool assigned = p.getCue (sel) >= 0;
        const auto col = padMapHue (sel, p);

        // ---- dmeta: SLICE DETAIL / PAD n . STEM (pad's stem hue; FULL = neutral) ----
        {
            g.setColour (Theme::t3);
            g.setFont (Theme::ui (7.5f * 1.12f, true).withExtraKerningFactor (0.22f));
            g.drawText ("SLICE DETAIL", metaR.withTrimmedLeft (12).withTrimmedTop (10).withHeight (12),
                        juce::Justification::centredLeft);
            g.setFont (Theme::mono (11.0f * 1.12f, true));
            g.setColour (assigned ? col : Theme::t3);
            const juce::String stemName = assigned ? stemNameFor (sel) : juce::String();
            const juce::String val = assigned
                ? (juce::String ("PAD ") + juce::String (sel + 1) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + stemName)
                : juce::String ("PAD ") + juce::String (sel + 1);
            g.drawText (val, metaR.withTrimmedLeft (12).withTrimmedTop (26).withHeight (16),
                        juce::Justification::centredLeft);
        }

        // ---- dwave: zoomed slice, or a quiet placeholder for an empty pad ----
        if (! assigned || cachedLen <= 0)
        {
            g.setColour (Theme::t3);
            g.setFont (Theme::ui (10.5f));
            g.drawText ("no region assigned", waveRect, juce::Justification::centred);
            cueX = endX = -1.0f;   // no handles to hit-test
        }
        else
        {
            const int cue = p.getCue (sel);
            const int end = p.getEffectiveCueEnd (sel);
            const bool open = p.isOpenSlice (sel);
            // F1: freeze the zoom window for the duration of an active CUE/END gesture
            // (AC-F1.5) — recomputing zoomLo/zoomHi live off cue/end here every repaint
            // would make the waveform background swim under the handle mid-drag even
            // though rebuildPeaks() itself is skipped. Recomputed again once the gesture
            // ends (mouseUp clears the flag; the next tick's timerCallback rebuilds).
            if (! gestureZoomFrozen)
            {
                const int span = juce::jmax (1, end - cue);
                const int ctx  = (int) (span * 0.15);
                zoomLo = juce::jmax (0, cue - ctx);
                zoomHi = juce::jmin (cachedLen - 1, end + ctx);
            }
            const int zoomSpan = juce::jmax (1, zoomHi - zoomLo);

            auto xOf = [&] (int s) { return (float) waveL + (float) (s - zoomLo) / (float) zoomSpan * (float) (waveR - waveL); };
            cueX = xOf (cue);
            endX = open ? (float) waveR - 10.0f : xOf (end);

            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (waveRect);
                const float mid = (float) h * 0.5f;
                const float half = (float) h * 0.42f;
                const int nP = (int) peaks.size();
                for (int x = waveL; x < waveR && (x - waveL) < nP; ++x)
                {
                    const auto pk = peaks[(size_t) (x - waveL)];
                    const bool inSlice = (float) x >= cueX && (float) x <= endX;
                    g.setColour (inSlice ? col.withAlpha (0.9f) : col.withAlpha (0.28f));
                    g.drawVerticalLine (x, mid - pk.second * half, mid - pk.first * half);
                }
                // transient ticks (existing Analyzer onsets, zoomed to this window) — P1:
                // density-capped so a long slice's hundreds of onsets don't solid-band.
                // Pre-count visible onsets, fade alpha by average spacing, then thin the
                // draw pass to a 6px minimum stride (onsets are pre-sorted in timerCallback).
                int nVis = 0;
                for (int t : onsets)
                    if (t >= zoomLo && t <= zoomHi) ++nVis;
                const float avgSpacing = (float) (waveR - waveL) / (float) juce::jmax (1, nVis);
                const float tickAlpha = juce::jmap (juce::jlimit (6.0f, 24.0f, avgSpacing), 6.0f, 24.0f, 0.10f, 0.22f);
                g.setColour (juce::Colours::white.withAlpha (tickAlpha));
                float lastDrawnX = -1.0e9f;
                for (int t : onsets)
                {
                    if (t < zoomLo || t > zoomHi) continue;
                    const float tx = xOf (t);
                    if (tx - lastDrawnX < 6.0f) continue;
                    lastDrawnX = tx;
                    g.fillRect (tx - 0.75f, (float) h * 0.06f, 1.5f, (float) h * 0.16f);
                }
                // dim outside the cue/end region
                g.setColour (juce::Colours::black.withAlpha (0.45f));
                if (cueX > (float) waveL) g.fillRect ((float) waveL, 0.0f, cueX - (float) waveL, (float) h);
                if (! open && endX < (float) waveR) g.fillRect (endX, 0.0f, (float) waveR - endX, (float) h);

                // granular freeze/position marker (cooler accent — Theme::bass, matching
                // the mockup's own cool-toned .dv value colour for this strip)
                if (p.grainOnFor (sel))
                {
                    const float gx = xOf (juce::jlimit (cue, juce::jmax (cue, end), cue + (int) (p.getGrainPosFor (sel) * (end - cue))));
                    const bool frozen = p.grainFreezeFor (sel);
                    g.setColour (Theme::bass.withAlpha (frozen ? 0.95f : 0.7f));
                    g.fillRect (gx - (frozen ? 1.5f : 1.0f), 0.0f, frozen ? 3.0f : 2.0f, (float) h);
                    juce::Path tip;
                    tip.addTriangle (gx - 5.0f, (float) h, gx + 5.0f, (float) h, gx, (float) h - 8.0f);
                    g.fillPath (tip);
                }

                // live playhead (reuses the same per-pad play position the hero uses)
                const int pp = p.getPadPlayPos (sel);
                if (pp >= zoomLo && pp <= zoomHi)
                {
                    const float px = xOf (pp);
                    g.setColour (Theme::glow.withAlpha (0.95f));
                    g.fillRect (px - 1.2f, 0.0f, 2.4f, (float) h);
                }

                // handles: amber glow line + triangle cap (mockup dwave handle glyphs)
                // F4: armed-handle affordance (SLICE_FEEL_TASK.md F4) — the handle most
                // recently grabbed/arrow-armed for arrow-key nudge renders its triangle cap
                // full-alpha Theme::accent (unchanged fill, matches today) PLUS a 1px accent
                // outline ring so it's visibly distinct; the other cap is untouched (no ring).
                auto drawHandle = [&] (float x, bool isOpenEnd, bool isArmed)
                {
                    g.setColour (Theme::glow.withAlpha (0.35f));
                    g.fillRect (x - 3.0f, 0.0f, 6.0f, (float) h);
                    g.setColour (Theme::accent);
                    g.fillRect (x - 1.5f, 0.0f, 3.0f, (float) h);
                    juce::Path cap;
                    cap.addTriangle (x - 5.0f, 0.0f, x + 5.0f, 0.0f, x, 7.0f);
                    g.fillPath (cap);
                    if (isArmed)
                    {
                        g.setColour (Theme::accent);
                        g.strokePath (cap, juce::PathStrokeType (1.0f));
                    }
                    if (isOpenEnd)
                    {
                        g.setColour (Theme::well.withAlpha (0.85f));
                        auto tag = juce::Rectangle<float> (x - 24.0f, (float) h - 13.0f, 24.0f, 11.0f);
                        g.fillRoundedRectangle (tag, 3.0f);
                        g.setColour (Theme::accent.withAlpha (0.85f));
                        g.setFont (Theme::mono (8.0f * 1.12f, true));
                        g.drawText ("OPEN", tag, juce::Justification::centred);
                    }
                };
                drawHandle (cueX, false, armedHandle == HandleDragEngine::Handle::cue);
                drawHandle (endX, open, armedHandle == HandleDragEngine::Handle::end);
            }
        }

        // ---- dread: CUE / END / LEN mono readouts, live ----
        {
            auto src = p.getSource();
            const double sr = (src != nullptr) ? src->sampleRate : 0.0;
            auto fmtTime = [&] (int s) -> juce::String
            {
                if (sr <= 0.0 || s < 0) return "--";
                const double secs = (double) s / sr;
                const int mm = (int) (secs / 60.0);
                return juce::String (mm) + ":" + juce::String::formatted ("%05.2f", secs - 60.0 * mm);
            };
            juce::String cueS = "--", endS = "--", lenS = "--";
            if (assigned && sr > 0.0)
            {
                const int cue = p.getCue (sel);
                const int end = p.getEffectiveCueEnd (sel);
                cueS = fmtTime (cue);
                endS = p.isOpenSlice (sel) ? juce::String ("OPEN") : fmtTime (end);
                lenS = p.isOpenSlice (sel) ? juce::String ("OPEN") : juce::String ((end - cue) / sr, 2) + "s";
            }
            auto row = [&] (int y, const char* k, const juce::String& v)
            {
                g.setColour (Theme::t3);
                g.setFont (Theme::ui (7.0f * 1.12f).withExtraKerningFactor (0.20f));
                const auto kr = juce::Rectangle<int> (readR.getX(), y, 44, 14);
                g.drawText (juce::String (k), kr, juce::Justification::centredLeft);
                g.setColour (Theme::t1);
                g.setFont (Theme::mono (10.0f * 1.12f));
                g.drawText (v, readR.getRight() - 12 - 90, y, 90, 14, juce::Justification::centredRight);
            };
            row (h / 2 - 22, "CUE", cueS);
            row (h / 2 - 6,  "END", endS);
            row (h / 2 + 10, "LEN", lenS);
        }
    }

    // ------------------------------------------------------------ interaction
    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = DragMode::none;
        const int sel = p.selectedPad.load();
        if (p.getCue (sel) < 0 || cachedLen <= 0) return;
        if (cueX < 0.0f) return;

        const float dc = std::abs (cueX - (float) e.x);
        const float de = std::abs (endX - (float) e.x);
        // F1: arm the shared engine only — no pushUndo/edit here (lazy on first move).
        if (de <= 6.0f && de <= dc)
        {
            drag = DragMode::endEdge;
            // PREPACK_UX U2: exact inverse of paint()'s xOf lambda (:1582), evaluated
            // at the drawn endX member, so an open slice's anchor equals its grip
            // (waveR-10) instead of len-1 (see handleDragBegin's isOpenSlice gate).
            const int openAnchor = (waveR > waveL)
                ? zoomLo + (int) (((double) (endX - waveL) / (double) juce::jmax (1, waveR - waveL))
                                    * (double) juce::jmax (1, zoomHi - zoomLo))
                : -1;
            handleDragBegin (dragEngine, p, sel, HandleDragEngine::Handle::end, e.x, openAnchor);
            gestureZoomFrozen = true;
            if (onHandleGrabbed) onHandleGrabbed (HandleDragEngine::Handle::end);
        }
        else if (dc <= 6.0f)
        {
            drag = DragMode::startEdge;
            handleDragBegin (dragEngine, p, sel, HandleDragEngine::Handle::cue, e.x);
            gestureZoomFrozen = true;
            if (onHandleGrabbed) onHandleGrabbed (HandleDragEngine::Handle::cue);
        }
        else if (p.grainOnFor (sel) && waveR > waveL)
        {
            // F5: granular position marker — same hit-test tolerance as the CUE/END
            // handles, now armed through the SAME shared engine (handleDragBegin
            // seeds anchorSample from the marker's current sample position, so
            // there's no jump at grab even if the click lands off-centre in the
            // 6px zone — AC-F5.1). No gestureZoomFrozen (grain doesn't move
            // cue/end, so the zoom window this strip shows can't move either —
            // freezing it would be a no-op) and no onHandleGrabbed (armed-handle
            // affordance is a cut-point/nudge concept; grain has neither).
            const int cue = p.getCue (sel), end = p.getEffectiveCueEnd (sel);
            const int zoomSpan = juce::jmax (1, zoomHi - zoomLo);
            const float gx = (float) waveL + (float) ((cue + (int) (p.getGrainPosFor (sel) * (end - cue))) - zoomLo)
                                / (float) zoomSpan * (float) (waveR - waveL);
            if (std::abs (gx - (float) e.x) <= 6.0f)
            {
                drag = DragMode::grainPos;
                handleDragBegin (dragEngine, p, sel, HandleDragEngine::Handle::grain, e.x);
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag == DragMode::none || cachedLen <= 0) return;
        const int zoomSpan = juce::jmax (1, zoomHi - zoomLo);

        switch (drag)
        {
            case DragMode::startEdge:
            case DragMode::endEdge:
            {
                // F1: relative-drag engine — same accumulator/lazy-undo/snap decision the
                // hero map uses, one call site (ground rule 1). Collapse affordance is the
                // same ~8px rule, converted to THIS strip's own sample-space tolerance at
                // its own (much tighter) zoom.
                const double samplesPerPixel = (double) zoomSpan / (double) juce::jmax (1, waveR - waveL);
                const int tol = (int) (8.0 * samplesPerPixel);
                // F2: Shift = fine mode, 0.10x rate, read per mouse event so mid-drag
                // press/release re-anchors implicitly via the accumulator (no jump).
                const double rate = e.mods.isShiftDown() ? 0.10 : 1.0;
                // F3: Alt bypasses snap for this event only, read per event (same
                // pattern as rate) so releasing Alt mid-drag re-engages capture.
                const bool altDown = e.mods.isAltDown();
                handleDragMove (dragEngine, p, e.x, samplesPerPixel, tol, rate, altDown);
                break;
            }
            case DragMode::grainPos:
            {
                // F5: same accumulator as CUE/END (AC-F5.3) — this strip's spp, and
                // Shift = 0.10x fine rate (F2's rate hook), but NO altDown/snap (the
                // engine's grain branch never calls resolveSnap regardless of what's
                // passed, so there's nothing to gate here either way).
                const double samplesPerPixel = (double) zoomSpan / (double) juce::jmax (1, waveR - waveL);
                const double rate = e.mods.isShiftDown() ? 0.10 : 1.0;
                handleDragMove (dragEngine, p, e.x, samplesPerPixel, 0, rate);
                break;
            }
            default: break;
        }
        repaint();
        ++p.uiDirty;   // C4: nudge the hero to repaint too — single source of truth
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        // F1: release the zoom freeze on gesture end so timerCallback's hash check
        // (which was skipped mid-gesture) re-evaluates and rebuilds peaks at the
        // post-drag cue/end window on the very next tick.
        gestureZoomFrozen = false;
        handleDragEnd (dragEngine);
        drag = DragMode::none;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool nearHandle = cueX >= 0.0f
            && (std::abs (cueX - (float) e.x) <= 6.0f || std::abs (endX - (float) e.x) <= 6.0f);
        setMouseCursor (nearHandle ? juce::MouseCursor::LeftRightResizeCursor
                                   : juce::MouseCursor::NormalCursor);
    }

private:
    enum class DragMode { none, startEdge, endEdge, grainPos };

    juce::String stemNameFor (int pad) const
    {
        const auto m = p.getPadStemMask (pad);
        if (m == 0) return "FULL";
        static const char* n[6] = { "DRM", "BASS", "VOX", "GTR", "PNO", "OTH" };
        if ((std::uint8_t) (m & (m - 1)) == 0)
            for (int k = 0; k < 6; ++k) if (m == (std::uint8_t) (1u << k)) return n[k];
        return "MIX";
    }

    void timerCallback() override
    {
        auto s = p.getSource();
        const int sel = p.selectedPad.load();
        // rebuild peaks when the source, selection, zoom window, or width changes
        juce::int64 hh = (juce::int64) sel * 97 + (juce::int64) p.getCue (sel) * 7
                        + (juce::int64) p.getEffectiveCueEnd (sel);
        // F1: during an active CUE/END gesture the zoom window is frozen (AC-F1.5) —
        // skip the hash-triggered rebuild so cue/end changes mid-drag don't re-zoom;
        // source/width changes still apply immediately even mid-gesture. Recompute
        // on mouseUp: gestureZoomFrozen clears there and the very next tick sees the
        // post-drag cue/end in the hash and rebuilds normally.
        const bool sourceOrWidthChanged = (s.get() != cachedSrc || getWidth() != cachedW);
        if (sourceOrWidthChanged || (! gestureZoomFrozen && hh != cachedHash))
        {
            cachedSrc = s.get();
            cachedW = getWidth();
            cachedHash = hh;
            keep = s;
            cachedLen = s != nullptr ? s->buffer.getNumSamples() : 0;
            onsets = p.getOnsetPositions();
            std::sort (onsets.begin(), onsets.end());   // P1: guarantee time order for the tick-density pass
            rebuildPeaks (sel);
        }
        const int d = p.uiDirty.load();
        if (d != lastDirty) { lastDirty = d; repaint(); }

        // C4: the granular freeze/position marker reads p.grainOnFor/getGrainPosFor/
        // grainFreezeFor directly in paint(), but none of those (APVTS knob/toggle,
        // driven straight off the inspector's attachments) bump uiDirty — watch a
        // cheap folded hash here (repaint only, no rebuildPeaks: the marker overlay
        // doesn't need new peak data) so the marker tracks the knob live instead of
        // waiting on an unrelated dirty bump.
        {
            const juce::int64 gh = (p.grainOnFor (sel) ? 1 : 0)
                                  + (p.grainFreezeFor (sel) ? 2 : 0)
                                  + (juce::int64) (p.getGrainPosFor (sel) * 100000.0f) * 4;
            if (gh != lastGrainHash) { lastGrainHash = gh; repaint(); }
        }

        bool playing = p.getPadPlayPos (sel) >= 0;
        if (playing || wasPlaying) repaint();
        wasPlaying = playing;
    }

    void rebuildPeaks (int sel)
    {
        peaks.clear();
        if (keep == nullptr || cachedLen <= 0 || p.getCue (sel) < 0) { repaint(); return; }
        waveL = 118; waveR = juce::jmax (waveL, getWidth() - 150);
        const int cols = juce::jmax (0, waveR - waveL);
        if (cols <= 0) { repaint(); return; }

        const int cue = p.getCue (sel);
        const int end = p.getEffectiveCueEnd (sel);
        const int span = juce::jmax (1, end - cue);
        const int ctx  = (int) (span * 0.15);
        zoomLo = juce::jmax (0, cue - ctx);
        zoomHi = juce::jmin (cachedLen - 1, end + ctx);
        const int zoomSpan = juce::jmax (1, zoomHi - zoomLo);

        const auto& b = keep->buffer;
        const float* d = b.getReadPointer (0);
        peaks.resize ((size_t) cols, { 0.0f, 0.0f });
        const double spp = (double) zoomSpan / (double) cols;
        const int stride = juce::jmax (1, (int) (spp / 512.0));
        for (int x = 0; x < cols; ++x)
        {
            const int a  = juce::jlimit (0, cachedLen - 1, (int) (zoomLo + spp * x));
            const int e2 = juce::jlimit (a + 1, cachedLen, (int) (zoomLo + spp * (x + 1)) + 1);
            float lo = 0.0f, hi = 0.0f;
            for (int i = a; i < e2; i += stride) { lo = juce::jmin (lo, d[i]); hi = juce::jmax (hi, d[i]); }
            peaks[(size_t) x] = { lo, hi };
        }
        repaint();
    }

    GentSamplerAudioProcessor& p;
    SourceSample::Ptr keep;
    void* cachedSrc = nullptr;
    int cachedW = 0, cachedLen = 0, lastDirty = -1;
    juce::int64 cachedHash = std::numeric_limits<juce::int64>::min();
    juce::int64 lastGrainHash = std::numeric_limits<juce::int64>::min();   // C4: grain marker watch
    std::vector<std::pair<float, float>> peaks;
    std::vector<int> onsets;
    int zoomLo = 0, zoomHi = 0;
    int waveL = 118, waveR = 0;
    float cueX = -1.0f, endX = -1.0f;
    DragMode drag = DragMode::none;
    HandleDragEngine dragEngine;      // F1: shared relative-drag state for CUE/END gestures
    bool gestureZoomFrozen = false;   // F1: freeze zoomLo/zoomHi while a handle gesture is active
    bool wasPlaying = false;
    // F4: editor-pushed paint-only affordance state (nudge target lives in the editor —
    // this is just what the strip's drawHandle() reads to ring the armed cap).
    HandleDragEngine::Handle armedHandle = HandleDragEngine::Handle::cue;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SliceDetailStrip)
};

// ---------------------------------------------------------------------------
class PadGrid : public juce::Component,
                private juce::Timer
{
public:
    explicit PadGrid (GentSamplerAudioProcessor& proc) : p (proc)
    {
        startTimerHz (30);
    }

    // set by the editor: render a pad -> temp WAV for drag-out (returns {} on failure)
    std::function<juce::File (int)> makePadFile;

    void paint (juce::Graphics& g) override
    {
        const int sel = p.selectedPad.load();
        for (int i = 0; i < 16; ++i)
        {
            const auto r = padBounds (i).toFloat().reduced (3.5f);
            const auto col = padSourceColour (i, p);
            const bool playing = p.isPadPlaying (i);
            const bool assigned = p.getCue (i) >= 0;

            // mockup .pad face: raised dark gradient (empty pads slightly darker),
            // lit with the pad hue while playing
            const juce::Colour faceHi = playing ? col.withAlpha (0.55f)
                                     : assigned ? Theme::padFaceHi : Theme::padFaceEmptyHi;
            const juce::Colour faceLo = playing ? col.darker (0.4f).withAlpha (0.55f)
                                     : assigned ? Theme::padFaceLo : Theme::padFaceEmptyLo;
            // .pad box-shadow 0 2px 5px black .4 (soft under-shadow)
            Theme::dropShadow (g, r, 8.0f, 5.5f, 2.2f, 0.40f, 4);
            juce::ColourGradient face (faceHi, r.getX(), r.getY(), faceLo, r.getX(), r.getBottom(), false);
            g.setGradientFill (face);
            g.fillRoundedRectangle (r, 8.0f);

            // border: edge-lo with edge-hi top rim
            g.setColour (Theme::edgeLo());
            g.drawRoundedRectangle (r, 8.0f, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (playing ? 0.18f : 0.07f));
            g.drawLine (r.getX() + 8.0f, r.getY() + 0.8f, r.getRight() - 8.0f, r.getY() + 0.8f, 1.1f);

            // .pad.sel — accent ring + real outer bloom (box-shadow 0 0 16px .22)
            if (i == sel)
            {
                Theme::featherGlow (g, r, 8.0f, Theme::accent.withAlpha (0.30f), 3.4f, 4);
                g.setColour (Theme::accent.withAlpha (0.8f));
                g.drawRoundedRectangle (r, 8.0f, 1.4f);
            }

            // mini-waveform: centre-mirrored columns (mockup pad canvas draws
            // fillRect(x, mid-a, 2, a*2) — symmetric around the vertical middle)
            {
                const auto& v = padPk[(size_t) i];
                if (! v.empty())
                {
                    juce::Graphics::ScopedSaveState ss (g);
                    juce::Path clip; clip.addRoundedRectangle (r, 8.0f);
                    g.reduceClipRegion (clip);
                    const float mwH  = r.getHeight() * 0.40f;      // max half-amplitude
                    const float midY = r.getCentreY() + r.getHeight() * 0.06f;
                    const float colW = r.getWidth() / (float) v.size();
                    g.setColour ((assigned ? col : Theme::t3).withAlpha (0.50f));
                    for (size_t c = 0; c < v.size(); ++c)
                    {
                        const float a = juce::jmax (0.02f, v[c]) * mwH;
                        g.fillRect (r.getX() + colW * (float) c + 1.0f, midY - a,
                                    juce::jmax (1.5f, colW - 2.5f), a * 2.0f);
                    }
                }
            }

            // hue dot, top-right (mockup .pad .dot: 5px, hue glow; brighter while playing)
            if (assigned)
            {
                const float led = 5.0f;
                const auto ledR = juce::Rectangle<float> (r.getRight() - led - 7.0f, r.getY() + 7.0f, led, led);
                g.setColour (col.withAlpha (playing ? 0.65f : 0.35f));
                g.fillEllipse (ledR.expanded (playing ? 4.0f : 2.5f));
                g.setColour (playing ? col.brighter (0.6f) : col);
                g.fillEllipse (ledR);
            }

            // number, top-left (mockup .pad .num: mono, t3; accent when selected)
            g.setColour (i == sel ? Theme::accent : Theme::t3.withAlpha (assigned ? 1.0f : 0.55f));
            g.setFont (Theme::mono (11.0f, true));
            g.drawText (juce::String (i + 1), (int) r.getX() + 8, (int) r.getY() + 6, 44, 14,
                        juce::Justification::topLeft);

            if (! assigned)
            {
                g.setColour (Theme::padPlus);
                g.setFont (Theme::ui (17.0f));
                g.drawText ("+", r, juce::Justification::centred);
            }

            // drag-out affordance (mockup .pad .grip): braille grip, bottom-right, on hover
            if (assigned && i == hoverPad)
            {
                g.setColour (Theme::t3);
                g.setFont (Theme::mono (10.0f));
                g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\xa0\xbf")),
                            (int) r.getRight() - 18, (int) r.getBottom() - 16, 14, 12,
                            juce::Justification::centred);
            }

            // 'CLEARED' flash for ~700 ms after a right-click clear
            if (p.clearedFlash.load() == i)
            {
                const juce::int64 since = (juce::int64) juce::Time::getMillisecondCounter() - p.clearedFlashTime.load();
                if (since < 700)
                {
                    g.setColour (Theme::accent.withAlpha (0.85f * (1.0f - (float) since / 700.0f)));
                    g.setFont (Theme::mono (11.0f, true));
                    g.drawText ("CLEARED", r, juce::Justification::centred);
                }
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = padAt (e.getPosition());
        if (i < 0) return;
        p.selectedPad = i;
        if (e.mods.isRightButtonDown())          // right-click clears the pad (undoable)
        {
            if (p.getCue (i) >= 0)
            {
                p.pushUndo();
                p.clearCue (i);
                p.clearedFlash = i;
                p.clearedFlashTime = juce::Time::getMillisecondCounter();
            }
            repaint();
            return;
        }
        // left-click: assigned pad plays; unassigned pad drops a cue at the playhead
        if (p.getCue (i) < 0)
        {
            p.assignPadCue (i, true);   // message thread: ok to snap the dropped cue
            repaint();
            return;
        }
        pressed = i;
        p.uiTrigger (i, true);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // a deliberate drag (not a tap) on an assigned pad drags its rendered WAV out
        if (dragging || pressed < 0 || e.getDistanceFromDragStart() < 10)
            return;
        const int i = pressed;
        p.uiTrigger (i, false);        // this gesture is a drag-out, not an audition
        pressed = -1;
        dragging = true;
        if (makePadFile != nullptr)
        {
            const auto f = makePadFile (i);
            if (f.existsAsFile())
                juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false);
        }
        dragging = false;
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (pressed >= 0)
            p.uiTrigger (pressed, false);
        pressed = -1;
        dragging = false;
    }

    // hover affordance: a pad with audio is drag-out-able -> grab cursor + a corner
    // grip glyph (drawn in paint) hint that you can drag it to the DAW. Empty pads: none.
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int i = padAt (e.getPosition());
        const bool canDrag = (i >= 0 && p.getCue (i) >= 0);
        setMouseCursor (canDrag ? juce::MouseCursor::DraggingHandCursor
                                : juce::MouseCursor::NormalCursor);
        if (i != hoverPad) { hoverPad = i; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverPad != -1) { hoverPad = -1; repaint(); }
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

private:
    juce::Rectangle<int> padBounds (int i) const
    {
        const int cw = getWidth() / 4, chh = getHeight() / 4;
        const int col = i % 4, row = 3 - i / 4;   // pad 1 = bottom-left (MPC style)
        return { col * cw, row * chh, cw, chh };
    }

    int padAt (juce::Point<int> pt) const
    {
        for (int i = 0; i < 16; ++i)
            if (padBounds (i).contains (pt))
                return i;
        return -1;
    }

    void timerCallback() override
    {
        auto s = p.getSource();
        juce::int64 hh = 0;
        for (int i = 0; i < 16; ++i)
            hh = hh * 131 + (juce::int64) p.getCue (i) * 7 + (juce::int64) p.getCueEnd (i);
        if (s.get() != padSrcId || hh != padCueHash)
        {
            padSrcId = s.get();
            padCueHash = hh;
            rebuildPadPeaks (s);
        }
        repaint();
    }

    void rebuildPadPeaks (SourceSample::Ptr s)
    {
        for (auto& v : padPk) v.clear();
        if (s == nullptr) return;
        const auto& b = s->buffer;
        const int N = b.getNumSamples();
        if (N <= 0) return;
        const float* d = b.getReadPointer (0);
        const int cols = 46;
        for (int i = 0; i < 16; ++i)
        {
            const int a = p.getCue (i);
            if (a < 0) continue;
            const int e = juce::jlimit (a + 1, N, p.getEffectiveCueEnd (i));
            auto& v = padPk[(size_t) i];
            v.assign ((size_t) cols, 0.0f);
            const double span = (double) (e - a) / (double) cols;
            const int stride = juce::jmax (1, (int) (span / 256.0));
            for (int c = 0; c < cols; ++c)
            {
                const int x0 = juce::jlimit (0, N - 1, a + (int) (span * c));
                const int x1 = juce::jlimit (x0 + 1, N, a + (int) (span * (c + 1)) + 1);
                float mx = 0.0f;
                for (int x = x0; x < x1; x += stride) mx = juce::jmax (mx, std::abs (d[x]));
                v[(size_t) c] = juce::jmin (1.0f, mx);
            }
        }
    }

    GentSamplerAudioProcessor& p;
    int pressed = -1;
    int hoverPad = -1;                            // pad under the cursor (drag-out affordance)
    bool dragging = false;                        // true while an external drag-out is in flight
    std::array<std::vector<float>, 16> padPk;     // per-pad mini-waveform envelope
    const void* padSrcId = nullptr;
    juce::int64 padCueHash = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadGrid)
};

// ---------------------------------------------------------------------------
//  DragChip: small handle the user drags OUT of the plugin window.
//  On drag it builds a file (WAV slice or captured MIDI) and hands it to the
//  OS as an external drag — drop it straight into the FL playlist or a folder.
class DragChip : public juce::Component
{
public:
    DragChip (const juce::String& text, std::function<juce::File()> fileMaker)
        : label (text), make (std::move (fileMaker)) {}

    void paint (juce::Graphics& g) override
    {
        const bool hover = isMouseOver();
        Theme::paintChip (g, getLocalBounds().toFloat().reduced (1.0f), false, hover, false, 0);
        g.setColour (hover ? Theme::t1 : Theme::t2);
        g.setFont (Theme::chipFont());
        g.drawText (juce::String (juce::CharPointer_UTF8 ("\xe2\xa0\xbf ")) + label.toUpperCase(),
                    getLocalBounds(), juce::Justification::centred);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging || e.getDistanceFromDragStart() < 8)
            return;
        dragging = true;
        if (make != nullptr)
        {
            const auto f = make();
            if (f.existsAsFile())
                juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false);
        }
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }   // hover chip state
    void mouseExit  (const juce::MouseEvent&) override { repaint(); }

private:
    juce::String label;
    std::function<juce::File()> make;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DragChip)
};

// ---------------------------------------------------------------------------
//  PitchValueChip — PHASE E1.3: the header PITCH control as a drag-value
//  readout (mockup-approved). Rotary-HV mechanics give a relative vertical
//  drag with no jump-on-click; the LnF "valueChip" property renders the bare
//  mono value instead of an arc knob. Double-click opens an inline editor;
//  right-click / ctrl-click resets to 0 st.
class PitchValueChip : public juce::Slider
{
public:
    PitchValueChip()
    {
        setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        getProperties().set ("valueChip", true);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.mods.isCtrlDown())
        {
            setValue (0.0, juce::sendNotificationSync);        // reset to 0 st
            return;
        }
        juce::Slider::mouseDown (e);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        editor = std::make_unique<juce::TextEditor>();
        addAndMakeVisible (*editor);
        editor->setBounds (getLocalBounds());
        editor->setJustification (juce::Justification::centred);
        editor->setFont (Theme::mono (13.0f));
        editor->setColour (juce::TextEditor::backgroundColourId, Theme::well);
        editor->setColour (juce::TextEditor::textColourId, Theme::t1);
        editor->setColour (juce::TextEditor::outlineColourId, Theme::accent.withAlpha (0.55f));
        editor->setColour (juce::TextEditor::focusedOutlineColourId, Theme::accent.withAlpha (0.8f));
        editor->setText (getTextFromValue (getValue()), juce::dontSendNotification);
        editor->selectAll();
        editor->onReturnKey  = [this] { commitEditor (true);  };
        editor->onEscapeKey  = [this] { commitEditor (false); };
        editor->onFocusLost  = [this] { commitEditor (true);  };
        editor->grabKeyboardFocus();
    }

private:
    void commitEditor (bool apply)
    {
        if (editor == nullptr)
            return;                                            // re-entry guard (focus-lost during teardown)
        auto* ed = editor.release();
        const juce::String typed = ed->getText();
        ed->setVisible (false);
        removeChildComponent (ed);
        juce::MessageManager::callAsync ([ed] { delete ed; }); // never delete inside its own callback
        if (apply)
            setValue (getValueFromText (typed), juce::sendNotificationSync);
        repaint();
    }

    std::unique_ptr<juce::TextEditor> editor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchValueChip)
};

// ---------------------------------------------------------------------------
//  MidiLed — PHASE E1.5: passive MIDI-activity indicator (no chip chrome,
//  non-interactive). Driven from the editor timer off lastTriggerCount.
class MidiLed : public juce::Component
{
public:
    MidiLed() { setInterceptsMouseClicks (false, false); }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().withSizeKeepingCentre (7.0f, 7.0f);
        if (lit)
        {
            g.setColour (Theme::accent.withAlpha (0.30f));
            g.fillEllipse (r.expanded (3.5f));                 // soft bloom
            g.setColour (Theme::glow);
            g.fillEllipse (r);
        }
        else
        {
            g.setColour (juce::Colour (0xff2a2f36));           // dark idle dot
            g.fillEllipse (r);
        }
    }

    bool lit = false;
};

// ---------------------------------------------------------------------------
//  GentLookAndFeel — Redesign Phase A skin. One control language: every
//  button/toggle/combo is a chip (Theme::paintChip), every rotary is the
//  approved arc knob, every slider textbox is a recessed mono value field.
//  Per-component variants ride on component properties, set once at build:
//    "chip"    : "ghost" | "primary"        (default chip otherwise)
//    "stemHue" : ARGB int -> .stem chip in that hue
//    "bipolar" : true -> arc fills from 12 o'clock outward
class GentLookAndFeel : public juce::LookAndFeel_V4
{
public:
    GentLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, Theme::canvas);
        setColour (juce::TextButton::textColourOffId, Theme::t2);
        setColour (juce::TextButton::textColourOnId, Theme::accentTextOn);
        setColour (juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::textColourId, Theme::t2);
        setColour (juce::ComboBox::arrowColourId, Theme::t3);
        setColour (juce::PopupMenu::backgroundColourId, Theme::panel);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::accent.withAlpha (0.85f));
        setColour (juce::PopupMenu::textColourId, Theme::t1);
        setColour (juce::PopupMenu::highlightedTextColourId, Theme::inkOnAccent);
        setColour (juce::Slider::thumbColourId, Theme::knobTick);
        setColour (juce::Slider::trackColourId, Theme::accent);
        setColour (juce::Slider::backgroundColourId, Theme::knobTrack);
        setColour (juce::Slider::textBoxTextColourId, Theme::t1);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId, Theme::t1);
        setColour (juce::ToggleButton::textColourId, Theme::t2);
        setColour (juce::TextEditor::backgroundColourId, Theme::well);
        setColour (juce::TextEditor::textColourId, Theme::t1);
        setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.4f));
        setColour (juce::TextEditor::outlineColourId, Theme::edgeLo());
    }

    static int chipKind (const juce::Component& c)
    {
        const auto k = c.getProperties()["chip"].toString();
        return k == "primary" ? 2 : (k == "ghost" ? 1 : (k == "overlay" ? 3 : 0));
    }
    static juce::Colour stemHueOf (const juce::Component& c)
    {
        const auto v = c.getProperties()["stemHue"];
        return v.isVoid() ? juce::Colours::transparentBlack : juce::Colour ((juce::uint32) (juce::int64) v);
    }

    // ---- chips: TextButton ----
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                               bool hover, bool down) override
    {
        const auto r = b.getLocalBounds().toFloat().reduced (1.5f);
        // per-component ACTIVE tint (REC capturing / PREVIEW playing): the 15 Hz timer
        // sets buttonColourId while active and removes it when idle — honor it as a
        // filled state chip so transport feedback survives the chip language.
        if (b.isColourSpecified (juce::TextButton::buttonColourId))
        {
            const auto tint = b.findColour (juce::TextButton::buttonColourId);
            const auto face = Theme::chipFace (r);
            Theme::featherGlow (g, face, 7.0f, tint.withAlpha (0.5f), Theme::chipGlowMargin (r) + 2.5f);
            juce::ColourGradient bg (tint.brighter (0.10f), face.getX(), face.getY(),
                                     tint, face.getX(), face.getBottom(), false);
            g.setGradientFill (bg);
            g.fillRoundedRectangle (face, 7.0f);
            g.setColour (tint.darker (0.4f));
            g.drawRoundedRectangle (face, 7.0f, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.25f));
            g.drawLine (face.getX() + 6.0f, face.getY() + 0.9f, face.getRight() - 6.0f, face.getY() + 0.9f, 1.1f);
            return;
        }
        Theme::paintChip (g, r, b.getToggleState(), hover, down, chipKind (b), stemHueOf (b), b.isEnabled());
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool hover, bool) override
    {
        if (b.isColourSpecified (juce::TextButton::buttonColourId))
            g.setColour (Theme::inkOnAccent);                              // active-tinted chip
        else
            g.setColour (b.isEnabled()
                             ? Theme::chipText (b.getToggleState(), hover, chipKind (b), stemHueOf (b))
                             : Theme::t3.withAlpha (0.8f));
        g.setFont (Theme::chipFont());
        g.drawText (b.getButtonText().toUpperCase(), b.getLocalBounds(), juce::Justification::centred);
    }

    // ---- chips: ToggleButton (same language; toggle state = .on) ----
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                           bool hover, bool down) override
    {
        const bool on = b.getToggleState();
        Theme::paintChip (g, b.getLocalBounds().toFloat().reduced (1.5f),
                          on, hover, down, chipKind (b), stemHueOf (b), b.isEnabled());
        g.setColour (b.isEnabled() ? Theme::chipText (on, hover, chipKind (b), stemHueOf (b))
                                   : Theme::t3.withAlpha (0.8f));
        g.setFont (Theme::chipFont());
        g.drawText (b.getButtonText().toUpperCase(), b.getLocalBounds(), juce::Justification::centred);
    }

    // ---- chips: ComboBox -> caret chip ----
    void drawComboBox (juce::Graphics& g, int width, int height, bool down,
                       int, int, int, int, juce::ComboBox& box) override
    {
        const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (1.0f);
        Theme::paintChip (g, r, false, box.isMouseOver(), down, chipKind (box));
        // caret "▾" inside the inset face (drawn as a triangle so no font dependency)
        const float cx = r.getRight() - Theme::kChipGlowMargin - 9.0f, cy = r.getCentreY();
        juce::Path caret;
        caret.addTriangle (cx - 3.5f, cy - 2.0f, cx + 3.5f, cy - 2.0f, cx, cy + 2.5f);
        g.setColour (Theme::t3);
        g.fillPath (caret);
    }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return Theme::chipFont(); }
    void positionComboBoxText (juce::ComboBox& box, juce::Label& l) override
    {
        l.setBounds (1, 1, box.getWidth() - 18, box.getHeight() - 2);
        l.setFont (Theme::chipFont());
        l.setColour (juce::Label::textColourId, Theme::t2);
        l.setJustificationType (juce::Justification::centred);
        l.setMinimumHorizontalScale (0.72f);   // squish long labels (CHOKE - OFF) instead of wrapping
    }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return Theme::chipFont(); }
    juce::Font getPopupMenuFont() override { return Theme::ui (12.5f); }

    // mockup .kwrap order is knob / label / value — the rotary area sits at the top,
    // the textbox (.kv) at the very bottom, and the caption Label lives in the gap
    // between them (positioned by the layout code, overlapping the slider bounds).
    juce::Slider::SliderLayout getSliderLayout (juce::Slider& s) override
    {
        if (! s.isRotary())
            return juce::LookAndFeel_V4::getSliderLayout (s);
        juce::Slider::SliderLayout layout;
        auto b = s.getLocalBounds();
        const int tbH = juce::jlimit (10, 14, b.getHeight() / 4);
        layout.textBoxBounds = { b.getCentreX() - juce::jmin (25, b.getWidth() / 2),
                                 b.getBottom() - tbH,
                                 juce::jmin (50, b.getWidth()), tbH };
        const int gap = juce::jlimit (0, 10, b.getHeight() - tbH - juce::jmin (b.getWidth(), b.getHeight() - tbH));
        layout.sliderBounds = b.withHeight (b.getHeight() - tbH - gap);
        return layout;
    }

    // ---- slider textboxes -> recessed mono value fields (.kv) ----
    void drawLabel (juce::Graphics& g, juce::Label& l) override
    {
        if ((bool) l.getProperties()["overlayPill"])
        {
            // hero overlay text (status / filename): translucent well pill behind, so
            // the text stays legible over waveform content
            auto r = l.getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (Theme::well.withAlpha (0.82f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.drawRoundedRectangle (r, 5.0f, 1.0f);
            g.setColour (l.findColour (juce::Label::textColourId));
            g.setFont (l.getFont());
            g.drawText (l.getText(), l.getLocalBounds().reduced (7, 0),
                        juce::Justification::centredLeft);
            return;
        }
        if (l.findParentComponentOfClass<juce::Slider>() != nullptr)
        {
            // .kv — recessed mono value field: well fill + real inset shadow
            auto r = l.getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (Theme::well);
            g.fillRoundedRectangle (r, 4.5f);
            Theme::innerShadow (g, r, 4.5f, 2.6f, 0.6f, 3);
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.drawRoundedRectangle (r, 4.5f, 1.0f);
            g.setColour (l.isEnabled() ? Theme::t1 : Theme::t3);
            g.setFont (Theme::kvFont());
            if (! l.isBeingEdited())
                g.drawText (l.getText(), l.getLocalBounds(), juce::Justification::centred);
            return;
        }
        juce::LookAndFeel_V4::drawLabel (g, l);
    }

    // ---- the approved arc knob (mockup Knob class, 270° sweep) ----
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float, juce::Slider& s) override
    {
        // PHASE E1.3: "valueChip" sliders render as a bare mono readout (the
        // header PITCH chip) — drag mechanics stay rotary-HV, no arc drawn.
        if (s.getProperties().contains ("valueChip"))
        {
            g.setFont (Theme::mono (13.0f));
            g.setColour (s.isEnabled() ? Theme::t1 : Theme::t3);
            g.drawText (s.getTextFromValue (s.getValue()),
                        juce::Rectangle<int> (x, y, width, height), juce::Justification::centred);
            return;
        }
        const auto  sq = juce::Rectangle<int> (x, y, width, height).toFloat();
        const float S  = juce::jmin (sq.getWidth(), sq.getHeight());
        const auto  c  = sq.getCentre();
        const float r  = S * 0.43f;
        // JUCE angles: radians clockwise from 12 o'clock. -135°..+135° = 270° sweep.
        const float aStart = -juce::MathConstants<float>::pi * 0.75f;
        const float aEnd   =  juce::MathConstants<float>::pi * 0.75f;
        const float va     = aStart + sliderPos * (aEnd - aStart);
        const bool  bipolar  = (bool) s.getProperties()["bipolar"];
        const bool  enabled  = s.isEnabled();
        const bool  hover    = s.isMouseOverOrDragging() && enabled;

        // recessed well + inner shadow
        g.setColour (Theme::well);
        g.fillEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f);
        {
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clip; clip.addEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f);
            g.reduceClipRegion (clip);
            juce::ColourGradient sh (juce::Colours::black.withAlpha (0.55f), c.x, c.y - r,
                                     juce::Colours::white.withAlpha (0.05f), c.x, c.y + r, false);
            sh.addColour (0.5, juce::Colours::transparentBlack);
            g.setGradientFill (sh);
            g.fillRect (sq);
        }

        // track + value arc at ra = r*0.82, stroke r*0.09 (min 2px)
        const float ra = r * 0.82f;
        const float aw = juce::jmax (2.0f, r * 0.09f);
        juce::Path track;
        track.addCentredArc (c.x, c.y, ra, ra, 0.0f, aStart, aEnd, true);
        g.setColour (Theme::knobTrack);
        g.strokePath (track, { aw, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

        if (enabled)
        {
            juce::Path val;
            if (bipolar) val.addCentredArc (c.x, c.y, ra, ra, 0.0f, juce::jmin (0.0f, va), juce::jmax (0.0f, va), true);
            else         val.addCentredArc (c.x, c.y, ra, ra, 0.0f, aStart, va, true);
            g.setColour (Theme::accent);
            g.strokePath (val, { aw, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

            // glow bloom at the arc tip (cached sprite; radius r*0.18, +25% on hover)
            const float gx = c.x + ra * std::sin (va), gy = c.y - ra * std::cos (va);
            const float gr = juce::jmax (3.0f, r * 0.18f);
            g.setOpacity (juce::jmin (1.0f, 0.5f * (hover ? 1.25f : 1.0f)));
            g.drawImage (Theme::glowSprite(), juce::Rectangle<float> (gx - gr, gy - gr, gr * 2.0f, gr * 2.0f));
            g.setOpacity (1.0f);
        }
        else
        {
            // disabled: same value-arc geometry, muted, no glow
            juce::Path val;
            if (bipolar) val.addCentredArc (c.x, c.y, ra, ra, 0.0f, juce::jmin (0.0f, va), juce::jmax (0.0f, va), true);
            else         val.addCentredArc (c.x, c.y, ra, ra, 0.0f, aStart, va, true);
            g.setColour (Theme::knobTrack.brighter (0.15f));
            g.strokePath (val, { aw, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });
        }

        // raised body + soft drop shadow (mockup: shadowBlur r*.18, offY r*.06) + top rim
        const float br = r * 0.72f;
        {
            const auto bodyRect = juce::Rectangle<float> (c.x - br, c.y - br, br * 2.0f, br * 2.0f);
            Theme::dropShadow (g, bodyRect, br, juce::jmax (2.5f, r * 0.22f), r * 0.07f, 0.6f, 4);
        }
        juce::ColourGradient body (Theme::knobBodyHi, c.x, c.y - br, Theme::knobBodyLo, c.x, c.y + br, false);
        g.setGradientFill (body);
        g.fillEllipse (c.x - br, c.y - br, br * 2.0f, br * 2.0f);
        juce::Path rim;   // top highlight arc (mockup: PI*1.15..PI*1.85 in canvas coords = over the top)
        rim.addCentredArc (c.x, c.y, br, br, 0.0f, -juce::MathConstants<float>::pi * 0.35f,
                           juce::MathConstants<float>::pi * 0.35f, true);
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.strokePath (rim, { 1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

        // tick r*0.36 -> r*0.62
        const float t0 = r * 0.36f, t1 = r * 0.62f;
        g.setColour (! enabled ? Theme::t3 : (hover ? Theme::accent : Theme::knobTick));
        g.drawLine (c.x + t0 * std::sin (va), c.y - t0 * std::cos (va),
                    c.x + t1 * std::sin (va), c.y - t1 * std::cos (va),
                    juce::jmax (2.0f, r * 0.07f));
    }

    // ---- PHASE E2.2: tabular figures — every slider value box renders in the
    //      mono face so digits are fixed-width and values don't jitter while
    //      dragging (Theme::mono is the numeric variant the skin already uses
    //      for the header readouts).
    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = juce::LookAndFeel_V4::createSliderTextBox (s);
        l->setFont (Theme::mono (10.0f));
        return l;
    }

    // ---- BLEED: linear slider as a recessed track + accent fill + glow tip ----
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float, juce::Slider::SliderStyle style,
                           juce::Slider& s) override
    {
        if (style != juce::Slider::LinearHorizontal)
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, 0, 0, style, s);
            return;
        }
        const float cy = (float) y + (float) height * 0.5f;
        auto track = juce::Rectangle<float> ((float) x, cy - 2.5f, (float) width, 5.0f);
        g.setColour (Theme::well);
        g.fillRoundedRectangle (track, 2.5f);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.drawRoundedRectangle (track, 2.5f, 1.0f);
        if (s.isEnabled())
        {
            g.setColour (Theme::accent);
            g.fillRoundedRectangle (track.withRight (juce::jmax (track.getX() + 2.0f, sliderPos)), 2.5f);
            const float gr = 7.0f;
            g.setOpacity (0.5f);
            g.drawImage (Theme::glowSprite(), juce::Rectangle<float> (sliderPos - gr, cy - gr, gr * 2.0f, gr * 2.0f));
            g.setOpacity (1.0f);
            g.setColour (Theme::knobTick);
            g.fillEllipse (sliderPos - 3.0f, cy - 3.0f, 6.0f, 6.0f);
        }
    }
};

// ---------------------------------------------------------------------------
// TRIGGER mode segment (mockup .seg .s): label + small sublabel inside a shared
// recessed well; the active segment is an amber-tinted glowing pill. `pos`
// (0 left, 1 mid, 2 right) rounds only the well's outer corners. Drives playMode.
struct TrigPad : juce::Component
{
    juce::String label, word;
    int  icon = 0;                 // retained field (0 GATE, 1 ONE-SHOT, 2 LATCH)
    int  pos  = 1;                 // 0 = left segment, 1 = middle, 2 = right
    bool active = false;
    std::function<void()> onClick;

    TrigPad() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void setActive (bool a) { if (active != a) { active = a; repaint(); } }
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        // the shared well bed (segment slice: round only this segment's outer side)
        const float rad = 7.0f;
        juce::Path bed;
        bed.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), rad, rad,
                                 pos == 0, pos == 2, pos == 0, pos == 2);
        g.setColour (Theme::well);
        g.fillPath (bed);
        {   // inner top shadow line, clipped to the bed so it can't leak past rounded corners
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (bed);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRect (r.getX(), r.getY() + 1.0f, r.getWidth(), 1.5f);
        }

        const bool hover = isMouseOver();
        auto pill = r.reduced (3.5f);
        if (active)
        {
            // .seg .s.on — amber pill with a soft bloom (box-shadow 0 0 10px .25)
            Theme::featherGlow (g, pill, 5.0f, Theme::accent.withAlpha (0.25f), 4.0f, 4);
            juce::ColourGradient bg (Theme::accent.withAlpha (0.28f), pill.getX(), pill.getY(),
                                     Theme::accent.withAlpha (0.12f), pill.getX(), pill.getBottom(), false);
            g.setGradientFill (bg);
            g.fillRoundedRectangle (pill, 5.0f);
            g.setColour (Theme::accent.withAlpha (0.30f));
            g.drawRoundedRectangle (pill, 5.0f, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawLine (pill.getX() + 5.0f, pill.getY() + 0.9f, pill.getRight() - 5.0f, pill.getY() + 0.9f, 1.1f);
        }

        const juce::Colour fg  = active ? Theme::accentTextOn : (hover ? Theme::t2 : Theme::t3);
        const juce::Colour sub = active ? Theme::accentTextOn.withAlpha (0.6f) : Theme::t3;
        auto tr = r.reduced (2.0f);
        tr.removeFromTop (tr.getHeight() * 0.16f);
        g.setColour (fg);
        g.setFont (Theme::ui (10.0f, true).withExtraKerningFactor (0.08f));
        g.drawText (label, tr.removeFromTop (14.0f), juce::Justification::centred);
        g.setColour (sub);
        g.setFont (Theme::ui (7.5f).withExtraKerningFactor (0.12f));
        g.drawText (word, tr, juce::Justification::centredTop);
    }
};

// ---------------------------------------------------------------------------
// D2: hero COMPOSITE<->STEMS segmented control (mockup #viewSeg, HTML 247-253;
// CSS .seg/.seg .s 90-102) — the same segmented-well primitive TrigPad already
// established (Phase A/B chrome convention, REDESIGN_TASK_D.md ground rule 6),
// but a dedicated 2-way struct rather than a reuse of TrigPad itself: TrigPad
// is wired to the 3-way playMode trigger and carries a <small> subtitle line
// the mockup's #viewSeg segments don't have (no "word" text under
// "Composite"/"Stems"), and this control's fonts must carry the later ×1.12
// CSS-px visual correction TrigPad predates (docs/STEM_VIEW_MODEL.md D2 scope;
// CLAUDE.md style precedent already in force at SliceDetailStrip call sites).
// Per the D1 addendum ("Joe ruling 2026-07-03, DECISION-2/3 interplay"), the
// STEMS branch is never suppressed by stem availability (a placeholder is
// always available), so displayed == requested == painted: `active` here is
// driven by the sanitized sticky REQUEST (gent::sanitizeHeroView(heroView)),
// the same predicate that selects WaveformView's paint branch.
struct HeroViewSeg : juce::Component
{
    juce::String label;   // "Composite" / "Stems" -- no subtitle line (mockup silent on one)
    int  pos    = 0;       // 0 = left segment, 2 = right segment (rounds only that outer side)
    bool active = false;   // reflects the sanitized sticky request (D1 addendum)
    std::function<void()> onClick;

    HeroViewSeg() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void setActive (bool a) { if (active != a) { active = a; repaint(); } }

    // Shared by paint() and layoutContent(): the mockup's .seg segments are
    // text-sized (CSS padding), not equal halves — a fixed 58px half truncated
    // "COMPOSITE" to "COMPOS…" (D6 capture finding). 10px side padding ≈ the
    // mockup's .s padding.
    static juce::Font labelFont()
    {
        return Theme::ui (9.0f * 1.12f, true).withExtraKerningFactor (0.10f);
    }
    int preferredWidth() const
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (labelFont(), label.toUpperCase(), 0.0f, 0.0f);
        return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth()) + 20;
    }
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const float rad = 7.0f;
        juce::Path bed;
        bed.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), rad, rad,
                                 pos == 0, pos == 2, pos == 0, pos == 2);
        g.setColour (Theme::well);
        g.fillPath (bed);
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (bed);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRect (r.getX(), r.getY() + 1.0f, r.getWidth(), 1.5f);
        }

        const bool hover = isMouseOver();
        auto pill = r.reduced (3.5f);
        if (active)   // .seg .s.on -- amber pill with a soft bloom (box-shadow 0 0 10px .25)
        {
            Theme::featherGlow (g, pill, 5.0f, Theme::accent.withAlpha (0.25f), 4.0f, 4);
            juce::ColourGradient bg (Theme::accent.withAlpha (0.28f), pill.getX(), pill.getY(),
                                     Theme::accent.withAlpha (0.12f), pill.getX(), pill.getBottom(), false);
            g.setGradientFill (bg);
            g.fillRoundedRectangle (pill, 5.0f);
            g.setColour (Theme::accent.withAlpha (0.30f));
            g.drawRoundedRectangle (pill, 5.0f, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawLine (pill.getX() + 5.0f, pill.getY() + 0.9f, pill.getRight() - 5.0f, pill.getY() + 0.9f, 1.1f);
        }

        const juce::Colour fg = active ? Theme::accentTextOn : (hover ? Theme::t2 : Theme::t3);
        g.setColour (fg);
        // .seg .s -- 9px CSS, 600 weight, .1em tracking, uppercase; ×1.12 visual correction
        g.setFont (labelFont());
        g.drawText (label.toUpperCase(), r, juce::Justification::centred);
    }
};

// ---------------------------------------------------------------------------
// SECTIONS_SPEC.md PART 3: the SLICE split-chip. Replaces the old plain
// sliceMenu TextButton with a chip that has two click zones: the main body
// (label) re-runs the CURRENTLY selected slice mode, and a narrow caret zone
// on the right opens the existing styled PopupMenu (mode picker + params).
// Built entirely from established primitives — Theme::paintChip for the face
// (same hover/down handling as GentLNF::drawButtonBackground), Theme::chipFont()
// for the label, and the exact caret-triangle idiom from
// GentLNF::drawComboBox (h:2338-2342 at spec-authoring time) for the caret —
// no new visual language.
struct SplitChip : juce::Component, public juce::SettableTooltipClient
{
    juce::String label;                 // synced by the editor's 15 Hz timer
    std::function<void()> onRun;        // main-zone click
    std::function<void()> onMenu;       // caret-zone click

    SplitChip() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    static constexpr float kCaretZoneW = 18.0f;

    void setLabel (const juce::String& l) { if (label != l) { label = l; repaint(); } }

    // HeroViewSeg::preferredWidth() precedent — measure the current label with
    // the chip font, add the caret zone + the same side margins paintChip's
    // face inset already accounts for.
    int preferredWidth() const
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (Theme::chipFont(), label.toUpperCase(), 0.0f, 0.0f);
        return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth())
               + (int) kCaretZoneW + 20;
    }

    juce::Rectangle<float> caretZone() const
    {
        auto r = getLocalBounds().toFloat();
        return r.removeFromRight (kCaretZoneW);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { repaint(); }
    void mouseMove  (const juce::MouseEvent&) override { repaint(); }
    void mouseDown  (const juce::MouseEvent&) override { down = true; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        down = false;
        repaint();
        if (! e.mods.isLeftButtonDown())   // PREPACKAGE_AUDIT.md #15: right/middle-
            return;                        // click must not trigger the action; the
                                            // pressed visual above always resolves.
        if (! contains (e.getPosition()))
            return;
        if (caretZone().contains (e.position))
        {
            if (onMenu) onMenu();
        }
        else
        {
            if (onRun) onRun();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const bool hover = isMouseOver();
        const auto r = getLocalBounds().toFloat().reduced (1.5f);
        // GentLookAndFeel::chipKind() reads the same "chip" property convention
        // (e.g. "overlay" for the hero .ov chips) other chips use — this keeps
        // the existing cpp:681 property-set wiring working unchanged.
        Theme::paintChip (g, r, false, hover, down, GentLookAndFeel::chipKind (*this));

        auto face = Theme::chipFace (r);
        auto textZone = face;
        auto caretR = textZone.removeFromRight (kCaretZoneW);

        // thin divider before the caret zone (low-alpha hairline, matches the
        // other faint separators already in this LnF's chip language)
        g.setColour (Theme::t3.withAlpha (0.25f));
        g.drawLine (caretR.getX(), face.getY() + 3.0f, caretR.getX(), face.getBottom() - 3.0f, 1.0f);

        g.setColour (hover ? Theme::t1 : Theme::t2);
        g.setFont (Theme::chipFont());
        g.drawText (label.toUpperCase(), textZone.reduced (6.0f, 0.0f), juce::Justification::centredLeft);

        // caret triangle — exact idiom from GentLNF::drawComboBox
        const float cx = r.getRight() - Theme::kChipGlowMargin - 9.0f, cy = r.getCentreY();
        juce::Path caret;
        caret.addTriangle (cx - 3.5f, cy - 2.0f, cx + 3.5f, cy - 2.0f, cx, cy + 2.5f);
        g.setColour (Theme::t3);
        g.fillPath (caret);
    }

private:
    bool down = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplitChip)
};

// ---------------------------------------------------------------------------
// A plain container that holds the whole UI at a FIXED design size; the editor
// applies an AffineTransform to scale it as one unit (aspect-locked proportional
// zoom), so every child scales together and no layout reflows on resize. Its
// paint/resized delegate back to the editor's paintContent()/layoutContent().
struct ScaleContainer : juce::Component
{
    std::function<void (juce::Graphics&)> onPaint;
    std::function<void()> onResized;
    void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
    void resized() override               { if (onResized) onResized(); }
};

class GentSamplerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        public juce::FileDragAndDropTarget,
                                        public juce::KeyListener,
                                        private juce::Timer
{
public:
    explicit GentSamplerAudioProcessorEditor (GentSamplerAudioProcessor&);
    ~GentSamplerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;   // film grain + vignette (cached images)
    void resized() override;
    // fixed design size — the mockup's own 1040x700, laid out 1:1 then AffineTransform-scaled
    static constexpr int kDesignW = 1040, kDesignH = 700;
    void paintContent (juce::Graphics&);   // draws the UI at design size (scaled by root)
    void layoutContent();                  // lays out all children at design size (scaled by root)
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int, int) override;
    bool keyPressed (const juce::KeyPress& k, juce::Component* origin) override;

private:
    void timerCallback() override;
    void attachPad (int pad);
    juce::File tempDir() const;
    static int rootIndexOf (const juce::String& keyText);   // -1 if unknown

    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    GentLookAndFeel lnf;            // must outlive every child component
    GentSamplerAudioProcessor& p;
    ScaleContainer root;           // holds the whole UI; AffineTransform-scaled in resized()
    WaveformView wave;
    SliceDetailStrip sliceDetail;
    PadGrid pads;

    juce::TextButton loadBtn { "LOAD" }, sliceBtn { "AUTO-SLICE" };
    juce::TextButton saveKitBtn { "SAVE KIT" }, loadKitBtn { "LOAD KIT" }, exportKitBtn { "EXPORT KIT" };
    juce::TextButton recBtn { "REC" }, halfBtn { "/2" }, dblBtn { "x2" };   // E1.5: record-arm chip is just REC
    // toolbar dropdown triggers (replace the old left panel)
    SplitChip sliceMenu;   // SECTIONS_SPEC.md PART 3: split chip (main zone runs, caret opens the menu)
    juce::TextButton kitMenu { "KIT" }, exportMenu { "EXPORT" };
    juce::TextButton previewBtn { "PREVIEW" }, clearBtn { "CLEAR..." }, undoBtn { juce::CharPointer_UTF8 ("\xe2\x86\xb6") }, redoBtn { juce::CharPointer_UTF8 ("\xe2\x86\xb7") };
    juce::TextButton fullBtn { "FULL VIEW" };   // map-tools: zoom waveform to whole sample
    juce::ComboBox sliceMode, tempoMode, keyPick, playMode, chokeBox, ftypeBox, qualityBox;
    juce::ToggleButton kbBtn { "KEYBOARD" }, sliceStop { "STOP AT NEXT CUE" }, followBtn { "FOLLOW" };
    juce::ToggleButton snapBtn { "SNAP" }, loopBtn { "LOOP" }, revBtn { "REVERSE" };
    juce::ToggleButton grainBtn { "GRAIN" }, freezeBtn { "FREEZE" };   // per-pad granular
    // 0.3: grain on/freeze push undo BEFORE the toggle mutates its APVTS param.
    // Button::sendClickMessage calls Button::Listener::buttonClicked (which is
    // what ButtonAttachment uses to commit the new value) BEFORE it invokes
    // onClick — so a plain onClick lambda would snapshot the ALREADY-CHANGED
    // value. This tiny Listener is added once in the constructor, before any
    // ButtonAttachment exists (those are (re)constructed per pad switch in
    // attachPad() and always addListener() at the end of the list), so it is
    // guaranteed to run first on every click for the lifetime of the editor.
    // AMENDMENT 0.3-A: applySnap() restores grainOn/grainFreeze via
    // apvts.getParameterAsValue(), which synchronously drives this button's
    // ButtonAttachment -> Button::setToggleState(..., sendNotificationSync)
    // -> sendClickMessage() -> every registered Button::Listener, including
    // THIS one — so an undo()/redo() restoring grainOn would otherwise
    // re-trigger pushUndo() from inside undo()/redo() itself, corrupting the
    // history stack. isRestoringSnap() is true only for applySnap()'s own
    // duration (message thread only), so a genuine user click is unaffected.
    struct GrainTogglePush : juce::Button::Listener
    {
        GentSamplerAudioProcessor* proc = nullptr;
        void buttonClicked (juce::Button*) override
        {
            if (proc != nullptr && ! proc->isRestoringSnap())
                proc->pushUndo();
        }
    };
    GrainTogglePush grainOnPush, grainFreezePush;
    juce::TextButton   transcribeBtn { "TRANSCRIBE" };                 // per-pad audio-to-MIDI
    juce::ToggleButton transcribeQuantBtn { "QUANTIZE" };              // snap notes to the grid
    juce::Label        transcribeLbl;                                  // status / note count
    juce::ToggleButton velBtn { "VELOCITY" };                         // MIDI velocity -> level (global)
    juce::TextButton   clearPadBtn { "CLEAR" };                        // clear the selected pad

    // ---- stem separation UI (Stage 3) ----
    juce::TextButton sepStemsBtn { "SEPARATE STEMS" };
    juce::Label      stemStatusLbl;
    std::array<juce::TextButton, 7> srcTag;     // PAD SOURCE: FULL, DRM, BASS, VOX, GTR, PNO, OTH
    std::array<TrigPad, 3> trigSeg;              // TRIGGER: Gate / One-shot / Latch (drives playMode)
    std::array<HeroViewSeg, 2> viewSeg;          // D2: hero COMPOSITE<->STEMS seg (mockup #viewSeg)
    juce::Label fileLbl, bpmLbl, playLbl, ratioLbl, titleLbl, padTitle, ppL, plL, paL, prL, pcL, psL, ppanL;   // E1.1: padRead retired with the header PAD chip
    juce::Label padMetaLbl, padMeta2Lbl, chokeLbl, pcoL, preL, ftypeLbl, pbL;   // pad meta lines (.m1/.m2) + choke + FILTER + bleed captions
    juce::Label gsL, gdL, gpL, gyL, gtL;   // granular knob captions (size/density/position/spray/pitch)
    PitchValueChip masterPitch;                                // E1.3: header PITCH as a drag-value chip
    MidiLed midiLed;                                           // E1.5: passive MIDI-activity LED
    int ledTrigSeen = -1, ledHold = 0;                         // E1.5: LED drive state (timer-polled)
    juce::Slider padPitch, padLevel, padAtt, padRel, padCrush, padSpeed, padPan, padCutoff, padReso, padBleed;
    juce::Slider padGrainSize, padGrainDens, padGrainPos, padGrainSpray, padGrainPitch;

    std::unique_ptr<DragChip> midiChip;        // performance-capture MIDI drag-out
    std::unique_ptr<DragChip> transcribeChip;  // transcribed-MIDI drag-out (Basic Pitch)

    std::unique_ptr<SA> aMaster, aPitch, aLevel, aAtt, aRel, aCrush, aSpeed, aPan, aCutoff, aReso, aBleed;
    std::unique_ptr<SA> aGrainSize, aGrainDens, aGrainPos, aGrainSpray, aGrainPitch;
    std::unique_ptr<BA> aKb, aSlice, aLoop, aReverse, aGrainOn, aGrainFreeze;
    std::unique_ptr<CA> aTempoMode, aMode, aChoke, aFType;

    int attachedPad = -1;
    juce::String keyItemsBuiltFor;
    juce::TooltipWindow tooltips { this, 600 };
    juce::Rectangle<int> headerRect, toolbarRect, inspRect, displayRect, padsRect;   // layout zones (railRect/stemRect retired)
    juce::Rectangle<int> detailRect;       // C3: Slice Detail strip bounds (mockup .detail)
    std::vector<std::pair<juce::Point<int>, juce::String>> sectionLabels;
    std::vector<std::pair<juce::Point<int>, juce::String>> hdrEyebrows;   // E1.2: dimmer per-control tier under the zone labels
    std::vector<int> hdrDividers;          // x positions of header group separators
    std::vector<int> inspDividers;         // y positions of inspector section separators
    std::vector<juce::Rectangle<int>> grpRects;   // mockup .grp boxes (GRANULAR / MIDI groups)
    std::vector<juce::Rectangle<int>> kdivRects;  // mockup .kdiv vertical knob-row dividers
    bool granularShown = false;            // cached GRAIN-expanded state -> reflow on change
    juce::Image vignetteImg;               // 256x256 radial vignette tile (built once, stretched -> elliptical)
    juce::Image noiseImg;                  // 96x96 film-grain tile (built once)
    juce::Image frameImg;                  // low-res frame-canvas radial (built once, stretched -> elliptical)
    std::unique_ptr<juce::FileChooser> chooser;

    // ---- F4: arrow-key nudge state (SLICE_FEEL_TASK.md F4) — editor-held per spec's
    // file plan; the nudge target is "the handle most recently grabbed on either
    // surface for the selected pad", defaulting to CUE and resetting to CUE whenever
    // the selected pad changes. lastNudgeMs backs the 600ms undo-coalescing window.
    HandleDragEngine::Handle armedHandle = HandleDragEngine::Handle::cue;
    int    armedHandlePad = -1;          // selected pad the armed state was last set for
    juce::uint32 lastNudgeMs = 0;
    bool   nudgeUndoPending = false;     // true once a nudge has edited since the last coalesce window closed
    void nudgeHandle (bool right, bool fine);   // F4 helper: single nudge edit path (PluginEditor.cpp)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GentSamplerAudioProcessorEditor)
};
