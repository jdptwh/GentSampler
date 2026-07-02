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
    const std::uint8_t m = p.getPadStemMask (i);
    if (m != 0 && (std::uint8_t) (m & (m - 1)) == 0)
        for (int k = 0; k < 6; ++k)
            if (m == (std::uint8_t) (1u << k)) return stemColour (k);
    return padColour (i);
}

// ---------------------------------------------------------------------------
class WaveformView : public juce::Component,
                     private juce::Timer
{
public:
    explicit WaveformView (GentSamplerAudioProcessor& proc) : p (proc)
    {
        setOpaque (true);
        startTimerHz (30);
    }

    void setFollow (bool shouldFollow)                     { follow = shouldFollow; }
    void fullView()                                        { if (cachedLen > 0) setView (0.0, (double) cachedLen); }

    std::function<void()> onRequestLoad;                   // called when the empty map is clicked

    // ------------------------------------------------------------------ paint
    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::well);
        const int w = getWidth(), h = getHeight();
        const int sb = 11;                      // scrollbar strip height
        const int waveBottom = h - sb;
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

        // vertical order (mockup): stem lanes -> cue flags -> waveform
        bool showStems = false;
        for (auto& sp : stemPeaks) if (! sp.empty()) { showStems = true; break; }
        // C1: the top ruler is stem-lane furniture (mockup's drawComposite has no
        // top ruler at all — only the bottom one). Reserve it only when the stem
        // band will actually show; composite view gives that space back to the wave.
        const bool stemsShowing = showStems && h > 180;   // same gate bandH uses below
        const int rulerH  = (! peaks.empty() && stemsShowing) ? stemRulerH : 0;
        const int bandTop = rulerH + 2;
        // stem lanes are a prominent band, roughly balanced with the waveform below
        const int bandH   = stemsShowing   // Phase B hero is 196 tall; lanes must stay reachable
                                ? juce::jlimit (56, 250, (int) ((h - rulerH - 70) * 0.52)) : 0;
        const int flagH   = 15;
        const int flagY   = bandTop + bandH + (bandH > 0 ? 9 : 0);
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

        // stem ribbon lanes — the mute/solo controls (mockup: tinted pill + ribbon + hover-solo)
        if (bandH > 0)
        {
            const juce::Colour scol[6] = {
                Theme::stem (0), Theme::stem (1), Theme::stem (2),
                Theme::stem (3), Theme::stem (4), Theme::stem (5) };
            static const char* slab[6] = { "DRUMS", "BASS", "VOX", "GTR", "PNO", "OTHER" };
            const juce::Colour cLine  = Theme::knobTrack, cLine2 = Theme::knobTrack.brighter (0.25f),
                               cT3 = Theme::t3, cCue = Theme::accent, cInk = Theme::inkOnAccent,
                               cBox = Theme::panel;
            const float laneH = (float) bandH / 6.0f;
            const int   labW  = stemLabW;
            const int   soloW = stemSoloW;
            const float pillH = juce::jmin (22.0f, laneH - 4.0f);
            for (int s = 0; s < 6; ++s)
            {
                const auto& sp    = stemPeaks[(size_t) s];
                const bool muted  = p.isStemMuted (s);
                const bool soloed = p.isStemSoloed (s);
                const float ly    = (float) bandTop + laneH * (float) s;
                const float rowMid = ly + laneH * 0.5f;

                // ribbon (between pill and solo box)
                const float rx0 = 4.0f + (float) labW + 10.0f;
                const float rx1 = (float) w - (float) soloW - 12.0f;
                const float rh  = juce::jmin (laneH * 0.46f, 9.0f);
                if (! sp.empty())
                {
                    g.setColour (scol[s].withAlpha (muted ? 0.12f : 0.92f));
                    const int n2 = juce::jmin ((int) rx1, (int) sp.size());
                    for (int x = (int) rx0; x < n2; ++x)
                    {
                        const auto pk = sp[(size_t) x];
                        const float amp = juce::jmax (std::abs (pk.first), std::abs (pk.second));
                        g.drawVerticalLine (x, rowMid - amp * rh, rowMid + amp * rh);
                    }
                }

                // label pill (mute button): tinted bg, coloured text
                auto pill = juce::Rectangle<float> (4.0f, rowMid - pillH * 0.5f, (float) labW, pillH);
                if (muted)
                {
                    g.setColour (cLine); g.drawRoundedRectangle (pill, 5.0f, 1.0f);
                    g.setColour (cT3);
                }
                else
                {
                    g.setColour (scol[s].withAlpha (0.12f)); g.fillRoundedRectangle (pill, 5.0f);
                    g.setColour (scol[s].withAlpha (0.35f)); g.drawRoundedRectangle (pill, 5.0f, 1.0f);
                    g.setColour (scol[s]);
                }
                g.setFont (juce::Font (9.5f, juce::Font::bold));
                g.drawText (slab[s], pill, juce::Justification::centred);
                if (muted)
                    g.drawLine (pill.getX() + 6.0f, pill.getCentreY(), pill.getRight() - 6.0f, pill.getCentreY(), 1.0f);

                // solo "S" box (revealed on hover, or shown when soloed)
                if (hoverLane == s || soloed)
                {
                    auto sbox = juce::Rectangle<float> ((float) w - (float) soloW - 2.0f,
                                                        rowMid - (float) soloW * 0.5f,
                                                        (float) soloW, (float) soloW);
                    if (soloed) { g.setColour (cCue); g.fillRoundedRectangle (sbox, 4.0f); g.setColour (cInk); }
                    else        { g.setColour (cBox); g.fillRoundedRectangle (sbox, 4.0f);
                                  g.setColour (cLine2); g.drawRoundedRectangle (sbox, 4.0f, 1.0f);
                                  g.setColour (cT3); }
                    g.setFont (juce::Font (9.0f, juce::Font::bold));
                    g.drawText ("S", sbox, juce::Justification::centred);
                }
            }
        }

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

        // region edges, flags, end handles + active-window highlight
        for (int i = 0; i < 16; ++i)
        {
            if (p.getCue (i) < 0) continue;                  // unassigned
            const auto col = padSourceColour (i, p);
            const bool open   = p.isOpenSlice (i);
            const bool selPad = (i == sel);
            const float xs = sampleToX (p.getCue (i));
            const float xe = endHandleX (i);                 // window end, or open re-open grip
            const bool explicitEnd = p.getCueEnd (i) >= 0;   // open (== kOpenSlice) reads as false
            const float a = selPad ? 1.0f : 0.55f;

            if (xs >= -16.0f && xs <= (float) w)
            {
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

        // playheads: one moving line per sounding pad, in that pad's colour
        for (int i = 0; i < 16; ++i)
        {
            const int pp = p.getPadPlayPos (i);
            if (pp < 0) continue;
            const float px = sampleToX (pp);
            if (px < 0.0f || px > (float) w) continue;
            const auto col = padSourceColour (i, p);
            g.setColour (col.withAlpha (0.25f));
            g.fillRect (px - 1.5f, (float) top, 3.0f, (float) (waveBottom - top));
            g.setColour (col.brighter (0.7f).withAlpha (0.95f));
            g.drawVerticalLine ((int) px, (float) top, (float) waveBottom);
            juce::Path tip;
            tip.addTriangle (px - 4.0f, (float) top, px + 4.0f, (float) top, px, (float) top + 6.0f);
            g.fillPath (tip);
        }

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
        const int w = getWidth(), h = getHeight();
        const int sb = 11;

        // stem lane band: solo box (right) or label pill (left) toggles
        if (stemBandH > 0 && e.y >= stemBandTop && e.y < stemBandTop + stemBandH)
        {
            const int lane = juce::jlimit (0, 5, (int) ((e.y - stemBandTop) / (stemBandH / 6.0f)));
            if (e.x >= w - stemSoloW - 6)
                p.setStemSoloed (lane, ! p.isStemSoloed (lane));
            else if (e.x <= 4 + stemLabW + 6)
                p.setStemMuted (lane, ! p.isStemMuted (lane));
            ++p.uiDirty;
            repaint();
            return;
        }

        if (e.y >= h - sb)                                   // scrollbar
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
        if (e.y >= flagBarY && e.y < flagBarY + flagBarH)
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
                    p.pushUndo(); drag = DragMode::endEdge; dragPad = sel; return;
                }
                if (ds <= 5.0f)                                   // selected start edge
                {
                    p.pushUndo(); drag = DragMode::startEdge; dragPad = sel; return;
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
            p.pushUndo();
            drag = DragMode::endEdge;
            dragPad = hp;
            return;
        }

        // start edges
        const int sp = hitStartEdge (e.x);
        if (sp >= 0)
        {
            p.selectedPad = sp;
            p.pushUndo();
            drag = DragMode::startEdge;
            dragPad = sp;
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
                if (dragPad >= 0) { p.setCue (dragPad, xToSample (e.x), true); repaint(); }
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
            case DragMode::endEdge:
                if (dragPad >= 0)
                {
                    // open/window decision is PIXEL-based (zoom-independent): within ~8px of
                    // (or left of) the start line -> open/gated; further right -> a real window.
                    const float startX = sampleToX (p.getCue (dragPad));
                    if ((float) e.x <= startX + 8.0f)
                        p.setCueEnd (dragPad, p.getCue (dragPad));                 // collapse -> open
                    else
                    {
                        int s = xToSample (e.x);
                        if (p.snapEnabled.load())                                  // snap window end to grid
                            s = (p.gridStepSamples() > 0.0) ? p.nearestGridLine (s) : p.nearestTransient (s);
                        p.setCueEnd (dragPad, juce::jmax (p.getCue (dragPad) + 33, s));
                    }
                    repaint();
                }
                break;
            case DragMode::none:
            default:
                break;
        }
    }

    void mouseUp (const juce::MouseEvent&) override { drag = DragMode::none; dragPad = -1; }

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
        const bool edge = hitEndHandle (e.x) >= 0 || hitStartEdge (e.x) >= 0;
        setMouseCursor (edge ? juce::MouseCursor::LeftRightResizeCursor
                             : juce::MouseCursor::NormalCursor);
        int hl = -1;
        if (stemBandH > 0 && e.y >= stemBandTop && e.y < stemBandTop + stemBandH)
            hl = juce::jlimit (0, 5, (int) ((e.y - stemBandTop) / (stemBandH / 6.0f)));
        if (hl != hoverLane) { hoverLane = hl; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverLane != -1) { hoverLane = -1; repaint(); }
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

        // refresh stem ribbons when separation finishes / stems change
        {
            auto ss = p.getStems();
            const int sg = ss != nullptr ? ss->generation : -1;
            if (sg != lastStemGen) { lastStemGen = sg; rebuildStemPeaks(); repaint(); }
        }

        // snap the view to the most recently triggered pad's region
        const int trig = p.lastTriggerCount.load();
        if (trig != lastTrig)
        {
            lastTrig = trig;
            const int padIdx = p.lastTriggerPad.load();
            if (follow && padIdx >= 0 && cachedLen > 0)
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
    double viewStart = 0.0, viewSpan = 1.0;
    std::vector<std::pair<float, float>> peaks;
    std::array<std::vector<std::pair<float, float>>, 6> stemPeaks;
    StemSet::Ptr keepStems;
    int lastStemGen = -2;
    int stemBandTop = 18, stemBandH = 0;   // set in paint, used for lane clicks
    int flagBarY = 0, flagBarH = 15;        // flag pennant row (set in paint), used for flag clicks
    int hoverLane = -1;                     // stem lane under the mouse (for solo reveal)
    int stemLabW = 60, stemSoloW = 18, stemRulerH = 14;

    DragMode drag = DragMode::none;
    int dragPad = -1;
    bool follow = true, wasPlaying = false;
    int lastTrig = -1;
    double scrollGrab = 0.0, panAnchorStart = 0.0;
    int panAnchorX = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
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
    PadGrid pads;

    juce::TextButton loadBtn { "LOAD" }, sliceBtn { "AUTO-SLICE" };
    juce::TextButton saveKitBtn { "SAVE KIT" }, loadKitBtn { "LOAD KIT" }, exportKitBtn { "EXPORT KIT" };
    juce::TextButton recBtn { "REC MIDI" }, halfBtn { "/2" }, dblBtn { "x2" };
    // toolbar dropdown triggers (replace the old left panel)
    juce::TextButton sliceMenu { "SLICE" }, kitMenu { "KIT" }, exportMenu { "EXPORT" };
    juce::TextButton previewBtn { "PREVIEW" }, clearBtn { "CLEAR..." }, undoBtn { juce::CharPointer_UTF8 ("\xe2\x86\xb6") }, redoBtn { juce::CharPointer_UTF8 ("\xe2\x86\xb7") };
    juce::TextButton fullBtn { "FULL VIEW" };   // map-tools: zoom waveform to whole sample
    juce::ComboBox sliceMode, tempoMode, keyPick, playMode, chokeBox, ftypeBox, qualityBox;
    juce::ToggleButton kbBtn { "KEYBOARD" }, sliceStop { "STOP AT NEXT CUE" }, followBtn { "FOLLOW" };
    juce::ToggleButton snapBtn { "SNAP" }, loopBtn { "LOOP" }, revBtn { "REVERSE" };
    juce::ToggleButton grainBtn { "GRAIN" }, freezeBtn { "FREEZE" };   // per-pad granular
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
    juce::Label fileLbl, bpmLbl, playLbl, ratioLbl, titleLbl, padTitle, padRead, ppL, plL, paL, prL, pcL, psL, ppanL;
    juce::Label padMetaLbl, padMeta2Lbl, chokeLbl, pcoL, preL, ftypeLbl, pbL;   // pad meta lines (.m1/.m2) + choke + FILTER + bleed captions
    juce::Label gsL, gdL, gpL, gyL, gtL;   // granular knob captions (size/density/position/spray/pitch)
    juce::Slider masterPitch, padPitch, padLevel, padAtt, padRel, padCrush, padSpeed, padPan, padCutoff, padReso, padBleed;
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
    std::vector<std::pair<juce::Point<int>, juce::String>> sectionLabels;
    std::vector<int> hdrDividers;          // x positions of header group separators
    std::vector<int> inspDividers;         // y positions of inspector section separators
    std::vector<juce::Rectangle<int>> grpRects;   // mockup .grp boxes (GRANULAR / MIDI groups)
    std::vector<juce::Rectangle<int>> kdivRects;  // mockup .kdiv vertical knob-row dividers
    bool granularShown = false;            // cached GRAIN-expanded state -> reflow on change
    juce::Image vignetteImg;               // 256x256 radial vignette tile (built once, stretched -> elliptical)
    juce::Image noiseImg;                  // 96x96 film-grain tile (built once)
    juce::Image frameImg;                  // low-res frame-canvas radial (built once, stretched -> elliptical)
    juce::Rectangle<int> padChipRect;      // filled orange PAD chip in the header
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GentSamplerAudioProcessorEditor)
};
