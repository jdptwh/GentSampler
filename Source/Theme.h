#pragma once

// ============================================================================
//  Theme.h — GentSampler visual tokens (Redesign Phase A).
//  SOURCE OF TRUTH: gentsampler_redesign_v2.html (:root + component CSS).
//  Every colour/gradient/radius the skin uses lives here — no hardcoded colours
//  at call sites. Phases B-D reuse this header.
//
//  Px mapping (Phase B): the design canvas IS the mockup size (1040x700), so
//  every px value in this file is the mockup's literal CSS value, unscaled.
// ============================================================================

#include <JuceHeader.h>

namespace Theme
{
    // ---- surfaces (:root) ----
    const juce::Colour canvas   { 0xff0E1216 };
    const juce::Colour canvasTop  { 0xff131920 };   // frame radial: top-centre stop
    const juce::Colour canvasEdge { 0xff0A0D11 };   // frame radial: outer stop
    const juce::Colour panel    { 0xff161B21 };
    const juce::Colour panelHi  { 0xff1A2028 };
    const juce::Colour well     { 0xff0A0D10 };
    inline juce::Colour edgeHi()  { return juce::Colours::white.withAlpha (0.06f); }
    inline juce::Colour edgeLo()  { return juce::Colours::black.withAlpha (0.55f); }

    // ---- accent ----
    const juce::Colour accent   { 0xffE8993A };
    const juce::Colour glow     { 0xffFFB85C };
    const juce::Colour accentTextOn { 0xffFFD9A3 };   // .chip.on text
    const juce::Colour inkOnAccent { 0xff1A1207 };    // text on filled accent (.chip.primary)

    // ---- text ----
    const juce::Colour t1 { 0xffE8EAED };
    const juce::Colour t2 { 0xff9BA1A8 };
    const juce::Colour t3 { 0xff5C636B };

    // ---- stems (DRM BASS VOX GTR PNO OTH) + FULL ----
    const juce::Colour drm  { 0xffE85D5D };
    const juce::Colour bass { 0xff4D9DE0 };
    const juce::Colour vox  { 0xffE86ABF };
    const juce::Colour gtr  { 0xff3FBF7F };
    const juce::Colour pno  { 0xffC9A44D };
    const juce::Colour oth  { 0xff8B7FD6 };
    const juce::Colour fullStem { 0xffC7CCD2 };
    inline juce::Colour stem (int k)
    {
        static const juce::Colour sc[6] = { drm, bass, vox, gtr, pno, oth };
        return (k >= 0 && k < 6) ? sc[k] : fullStem;
    }

    // ---- knob (mockup Knob class) ----
    const juce::Colour knobTrack  { 0xff2A2F35 };
    const juce::Colour knobBodyHi { 0xff1C2229 };
    const juce::Colour knobBodyLo { 0xff12161B };
    const juce::Colour knobTick   { 0xffE8EAED };

    // ---- pad faces (mockup .pad / .pad.empty) ----
    const juce::Colour padFaceHi      { 0xff171D24 };
    const juce::Colour padFaceLo      { 0xff12161B };
    const juce::Colour padFaceEmptyHi { 0xff12161C };
    const juce::Colour padFaceEmptyLo { 0xff0F1318 };
    const juce::Colour padPlus       { 0xff232A32 };   // empty-pad "+" glyph

    // ---- transport-state tints (REC capture / PREVIEW active chips) ----
    const juce::Colour recActive     { 0xffC23B2A };
    const juce::Colour previewActive { 0xffE0641E };

    // ---- fonts ----
    inline juce::Font mono (float px, bool bold = false)
    {
        return { juce::Font ("Consolas", px, bold ? juce::Font::bold : juce::Font::plain) };
    }
    inline juce::Font ui (float px, bool bold = false)
    {
        return juce::Font (px, bold ? juce::Font::bold : juce::Font::plain);
    }
    // Phase B: design canvas is the mockup's own 1040x700 — geometry px are the
    // mockup literals. FONTS carry a x1.12 visual correction: a JUCE Font height
    // renders glyphs smaller than the same CSS font-size px (verified by pixel
    // side-by-sides in Phases A+B), so mockup pt x1.12 matches on screen.
    // .slabel — 8px CSS, .24em tracking, uppercase, t3
    inline juce::Font slabelFont()  { return ui (9.0f, true).withExtraKerningFactor (0.24f); }
    // .chip — 9.5px CSS, semi-bold, .12em tracking
    inline juce::Font chipFont()    { return ui (10.5f, true).withExtraKerningFactor (0.12f); }
    // .kv value fields — 9px CSS mono
    inline juce::Font kvFont()      { return mono (10.0f); }

    // ------------------------------------------------------------------------
    //  DEPTH SYSTEM — real soft shadows and glows (CSS box-shadow equivalents).
    //  Every blur is rendered as concentric feathered strokes with quadratic
    //  alpha falloff: reads like a gaussian bloom, costs no per-repaint blur.
    //  All px values are mockup CSS px x1.135 (the design-scale mapping).
    // ------------------------------------------------------------------------

    // outer glow / bloom around a rounded rect (CSS: box-shadow 0 0 <blur> <colour>)
    inline void featherGlow (juce::Graphics& g, juce::Rectangle<float> r, float rad,
                             juce::Colour colour, float blur, int layers = 6)
    {
        const float step = blur / (float) layers;
        for (int i = 0; i < layers; ++i)
        {
            const float t = (float) i / (float) layers;
            const float a = colour.getFloatAlpha() * (1.0f - t) * (1.0f - t);
            g.setColour (colour.withAlpha (a));
            g.drawRoundedRectangle (r.expanded (step * (float) i + step * 0.5f),
                                    rad + step * (float) i, step + 0.8f);
        }
    }

    // soft drop shadow under a rounded rect (CSS: box-shadow 0 <offY> <blur> black)
    inline void dropShadow (juce::Graphics& g, juce::Rectangle<float> r, float rad,
                            float blur, float offY, float alpha, int layers = 5)
    {
        const auto s = r.translated (0.0f, offY);
        const float step = blur / (float) layers;
        for (int i = 0; i < layers; ++i)
        {
            const float t = (float) i / (float) layers;
            const float a = alpha * (1.0f - t) * (1.0f - t) * 0.55f;
            g.setColour (juce::Colours::black.withAlpha (a));
            g.drawRoundedRectangle (s.expanded (step * (float) i + step * 0.5f),
                                    rad + step * (float) i, step + 0.8f);
        }
        g.setColour (juce::Colours::black.withAlpha (alpha * 0.5f));
        g.fillRoundedRectangle (s.expanded (0.5f), rad);
    }

    // inner shadow hugging the inside edge of a recessed rect (CSS: inset box-shadow),
    // biased darker across the top like the mockup wells
    inline void innerShadow (juce::Graphics& g, juce::Rectangle<float> r, float rad,
                             float blur, float alpha, int layers = 4)
    {
        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip; clip.addRoundedRectangle (r, rad);
        g.reduceClipRegion (clip);
        // strokes centred on the border with growing widths: the clip keeps the inside
        // half, so alpha stacks at the edge and falls off inward — an inset box-shadow
        for (int i = layers; i >= 1; --i)
        {
            const float t = (float) i / (float) layers;               // 1 -> 1/layers
            const float a = alpha * (1.0f - t + 1.0f / (float) layers) * 0.28f;
            g.setColour (juce::Colours::black.withAlpha (a));
            g.drawRoundedRectangle (r, rad, blur * t * 2.0f);
        }
        // extra top band (inset 0 2px ... -> light from above)
        juce::ColourGradient top (juce::Colours::black.withAlpha (alpha * 0.8f), r.getX(), r.getY(),
                                  juce::Colours::transparentBlack, r.getX(), r.getY() + blur * 1.4f, false);
        g.setGradientFill (top);
        g.fillRect (r.withHeight (blur * 1.4f));
    }

    // ------------------------------------------------------------------------
    //  shared painters (the three elevation surfaces)
    // ------------------------------------------------------------------------

    // RAISED panel: soft drop shadow + panel-hi->panel gradient + top light edge
    // (mockup .insp: box-shadow 0 4px 14px rgba(0,0,0,.35))
    inline void paintRaisedPanel (juce::Graphics& g, juce::Rectangle<float> r, float rad = 10.0f)
    {
        dropShadow (g, r, rad, 16.0f, 4.5f, 0.35f);
        juce::ColourGradient bg (panelHi, r.getX(), r.getY(), panel, r.getX(), r.getY() + r.getHeight() * 0.08f, false);
        g.setGradientFill (bg);
        g.fillRoundedRectangle (r, rad);
        g.setColour (edgeLo());
        g.drawRoundedRectangle (r, rad, 1.1f);
        g.setColour (edgeHi().withMultipliedAlpha (1.6f));   // 1px top light edge, boosted to survive scaling
        g.drawLine (r.getX() + rad * 0.7f, r.getY() + 0.8f, r.getRight() - rad * 0.7f, r.getY() + 0.8f, 1.2f);
    }

    // RECESSED well: well fill + real inset shadow + bottom edge-hi rim
    // (mockup .hero: inset 0 2px 8px rgba(0,0,0,.7))
    inline void paintWell (juce::Graphics& g, juce::Rectangle<float> r, float rad = 9.0f)
    {
        g.setColour (well);
        g.fillRoundedRectangle (r, rad);
        innerShadow (g, r, rad, 9.0f, 0.7f);
        g.setColour (edgeLo());
        g.drawRoundedRectangle (r, rad, 1.0f);
        g.setColour (edgeHi().withMultipliedAlpha (1.2f));
        g.drawLine (r.getX() + rad, r.getBottom() - 0.8f, r.getRight() - rad, r.getBottom() - 0.8f, 1.0f);
    }

    // .isep — soft gradient divider line
    inline void paintDivider (juce::Graphics& g, float x0, float x1, float y)
    {
        juce::ColourGradient dg (juce::Colours::transparentWhite, x0, y,
                                 juce::Colours::transparentWhite, x1, y, false);
        dg.addColour (0.12, juce::Colours::white.withAlpha (0.055f));
        dg.addColour (0.88, juce::Colours::white.withAlpha (0.055f));
        g.setGradientFill (dg);
        g.fillRect (juce::Rectangle<float> (x0, y, x1 - x0, 1.0f));
    }

    // ------------------------------------------------------------------------
    //  chip painter — the one control language. kind: 0 default, 1 ghost,
    //  2 primary. stemHue non-transparent => .stem chip in that hue.
    // ------------------------------------------------------------------------
    // Glow margin every chip face is inset by, leaving room INSIDE the component for
    // the outer glow feather (JUCE clips painting to bounds — CSS box-shadow doesn't).
    // ADAPTIVE: small chips (24px, the mockup norm) keep a >=19px face — the glow
    // tightens rather than starving the plate.
    constexpr float kChipGlowMargin = 4.0f;
    inline float chipGlowMargin (juce::Rectangle<float> r)
    {
        return juce::jlimit (1.5f, kChipGlowMargin, (r.getHeight() - 19.0f) * 0.5f);
    }
    inline juce::Rectangle<float> chipFace (juce::Rectangle<float> r)
    {
        return r.reduced (chipGlowMargin (r));
    }

    inline void paintChip (juce::Graphics& g, juce::Rectangle<float> r, bool on, bool hover, bool down,
                           int kind = 0, juce::Colour stemHue = juce::Colours::transparentBlack,
                           bool enabled = true)
    {
        const float rad = 7.0f;
        const bool isStem = ! stemHue.isTransparent();
        const float dim = enabled ? 1.0f : 0.45f;          // disabled: dim the body, kill the glow
        const auto face = chipFace (r);                    // adaptive face inset -> glow room
        const float blur = chipGlowMargin (r) + 2.5f;      // feather reach (soft tail past margin)

        auto topEdge = [&] (float alpha)
        {
            g.setColour (juce::Colours::white.withAlpha (alpha));
            g.drawLine (face.getX() + rad * 0.8f, face.getY() + 0.9f,
                        face.getRight() - rad * 0.8f, face.getY() + 0.9f, 1.1f);
        };

        if (kind == 2)                                     // .chip.primary — filled accent, strong bloom
        {
            if (enabled)
                featherGlow (g, face, rad, accent.withAlpha (down ? 0.30f : (hover ? 0.55f : 0.45f)), blur + 1.5f);
            juce::ColourGradient bg (glow.withMultipliedAlpha (dim), face.getX(), face.getY(),
                                     accent.withMultipliedAlpha (dim), face.getX(), face.getBottom(), false);
            g.setGradientFill (bg);
            g.fillRoundedRectangle (face, rad);
            g.setColour (accent.withAlpha (0.7f * dim));                        // 1px border (mockup)
            g.drawRoundedRectangle (face, rad, 1.0f);
            topEdge (0.40f * dim);
            return;
        }
        if (isStem && on)                                  // .stem.on — hue-filled + hue bloom
        {
            if (enabled)
                featherGlow (g, face, rad, stemHue.withAlpha (0.45f), blur);
            juce::ColourGradient bg (stemHue.brighter (0.12f).withMultipliedAlpha (dim), face.getX(), face.getY(),
                                     stemHue.withMultipliedAlpha (dim), face.getX(), face.getBottom(), false);
            g.setGradientFill (bg);
            g.fillRoundedRectangle (face, rad);
            g.setColour (stemHue.darker (0.55f).withMultipliedAlpha (dim));
            g.drawRoundedRectangle (face, rad, 1.0f);
            topEdge (0.30f * dim);
            return;
        }
        if (on)                                            // .chip.on — PHASE E3.2: amber TEXT/OUTLINE only.
        {                                                  // Solid fill + bloom is reserved for the primary
            g.setColour (accent.withAlpha ((down ? 0.85f : (hover ? 0.75f : 0.60f)) * dim));
            g.drawRoundedRectangle (face, rad, 1.0f);      // action and active segmented options; active-but-
            return;                                        // secondary toggles read as amber outline + text.
        }
        if (kind == 1)                                     // .chip.ghost — outline only, no shadow
        {
            if (down || hover)
            {
                g.setColour (juce::Colours::white.withAlpha (down ? 0.05f : 0.03f));
                g.fillRoundedRectangle (face, rad);
            }
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.drawRoundedRectangle (face, rad, 1.0f);
            return;
        }
        if (kind == 3)                                     // overlay chip — OPAQUE face so it reads
        {                                                  // over waveform content (hero .ov chips)
            dropShadow (g, face, rad, 4.0f, 1.5f, 0.55f, 4);
            g.setColour (juce::Colour (0xff141A21));       // opaque base under any state tint
            g.fillRoundedRectangle (face, rad);
            if (on && enabled)
            {
                // PHASE E3.2: overlay chips follow the same hierarchy — active =
                // amber outline + text on the opaque face, no bloom, no tint fill.
                g.setColour (accent.withAlpha (hover ? 0.75f : 0.60f));
            }
            else
            {
                juce::ColourGradient bg (juce::Colour (0xff1B212A).brighter (hover ? 0.06f : 0.0f),
                                         face.getX(), face.getY(),
                                         juce::Colour (0xff151A20), face.getX(), face.getBottom(), false);
                g.setGradientFill (bg);
                g.fillRoundedRectangle (face.reduced (0.0f), rad);   // gradient over the opaque base
                g.setColour (juce::Colours::black.withAlpha (0.60f));
            }
            g.drawRoundedRectangle (face, rad, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (on ? 0.10f : 0.08f));
            g.drawLine (face.getX() + rad, face.getY() + 1.0f, face.getRight() - rad, face.getY() + 1.0f, 1.0f);
            return;
        }
        // default .chip — raised dark gradient + drop shadow (0 1px 2px black .4)
        dropShadow (g, face, rad, 2.8f, 1.3f, 0.40f, 3);
        juce::ColourGradient bg (juce::Colour (0xff1B212A).brighter (down ? -0.05f : (hover ? 0.06f : 0.0f)),
                                 face.getX(), face.getY(),
                                 juce::Colour (0xff151A20), face.getX(), face.getBottom(), false);
        g.setGradientFill (bg);
        g.fillRoundedRectangle (face, rad);
        g.setColour (juce::Colours::black.withAlpha (0.50f));
        g.drawRoundedRectangle (face, rad, 1.0f);
        topEdge (0.09f);
    }

    // chip text colour per state/kind
    inline juce::Colour chipText (bool on, bool hover, int kind, juce::Colour stemHue = juce::Colours::transparentBlack)
    {
        if (kind == 2)                 return inkOnAccent;
        if (! stemHue.isTransparent()) return on ? juce::Colour (0xff0D1013) : stemHue.withAlpha (hover ? 1.0f : 0.85f);
        if (on)                        return accentTextOn;
        return hover ? t1 : t2;
    }

    // ------------------------------------------------------------------------
    //  cached radial glow sprite (mockup knob tip bloom) — rendered once,
    //  drawn scaled/tinted; no per-repaint blur.
    // ------------------------------------------------------------------------
    inline const juce::Image& glowSprite()
    {
        // TEARDOWN_FIX_SPEC.md: deliberately LEAKED (heap, never deleted). A
        // function-local static juce::Image is Direct2D-backed here (BACKLOG 0.4
        // finding); its destructor at DLL_PROCESS_DETACH runs under the Windows
        // loader lock and deadlocks inside d3d11/the NVIDIA driver — the FL
        // close-hang proven by the 2026-07-07 FL64.DMP analysis. The OS reclaims
        // the 16 KB at process exit. Never convert back to a by-value static.
        static juce::Image* img = new juce::Image ([] {
            constexpr int N = 64;
            juce::Image m (juce::Image::ARGB, N, N, true);
            juce::Graphics g (m);
            juce::ColourGradient rg (glow.withAlpha (1.0f), N * 0.5f, N * 0.5f,
                                     glow.withAlpha (0.0f), N * 0.5f, 0.0f, true);
            g.setGradientFill (rg);
            g.fillEllipse (0.0f, 0.0f, (float) N, (float) N);
            return m;
        }());
        return *img;
    }
}
