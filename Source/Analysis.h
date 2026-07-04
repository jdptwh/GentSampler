#pragma once
// ============================================================================
//  Analysis.h — offline audio analysis for GentSampler
//  - Onset envelope via spectral flux (FFT 1024 / hop 512)
//  - BPM via autocorrelation of the onset envelope (60–180 BPM search)
//  - Key via chromagram + Krumhansl-Schmuckler profile correlation
//  - Transient slicing via peak-picking on the onset envelope
// ============================================================================

#include <JuceHeader.h>
#include "EngineMath.h"
#include <vector>
#include <cmath>
#include <algorithm>

namespace gent
{

struct AnalysisResult
{
    double bpm = 0.0;
    juce::String key;
    std::vector<int> slices;                       // top-16 transient cuts (sample positions)
    std::vector<std::pair<int, float>> onsets;     // ALL onset peaks: (samplePos, strength 0..1)
    std::vector<gent::FrameFeatures> frames;        // PHASE3_SPEC PART 1: per-frame features
    int hop = 512;
    double frameRate = 0.0;
};

class Analyzer
{
public:
    static AnalysisResult analyze (const juce::AudioBuffer<float>& buf, double sr)
    {
        AnalysisResult r;
        const int n = buf.getNumSamples();
        if (n < 8192 || sr <= 0.0)
            return r;

        // ---- mono mixdown -------------------------------------------------
        std::vector<float> mono ((size_t) n, 0.0f);
        for (int c = 0; c < buf.getNumChannels(); ++c)
        {
            const float* p = buf.getReadPointer (c);
            for (int i = 0; i < n; ++i)
                mono[(size_t) i] += p[i];
        }
        {
            const float g = 1.0f / (float) juce::jmax (1, buf.getNumChannels());
            for (auto& v : mono) v *= g;
        }

        // ---- spectral flux onset envelope ---------------------------------
        constexpr int fftOrder = 10;
        constexpr int fftSize  = 1 << fftOrder;   // 1024
        constexpr int hop      = fftSize / 2;     // 512

        juce::dsp::FFT fft (fftOrder);
        std::vector<float> window ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (fftSize - 1)));

        const int numFrames = juce::jmax (0, (n - fftSize) / hop);
        std::vector<float> flux ((size_t) numFrames, 0.0f);
        std::vector<float> prevMag ((size_t) (fftSize / 2), 0.0f);
        std::vector<float> fftBuf ((size_t) (fftSize * 2), 0.0f);

        // PHASE3_SPEC.md PART 1 — per-frame feature band edges (Hz), named
        // per the spec's "band-edge Hz as named constexpr" instruction.
        constexpr float kBandEdgeLowHz  = 120.0f;
        constexpr float kBandEdgeMidHz  = 600.0f;
        constexpr float kBandEdgeHighHz = 3000.0f;

        r.frames.reserve ((size_t) numFrames);

        for (int f = 0; f < numFrames; ++f)
        {
            std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
            const int off = f * hop;
            for (int i = 0; i < fftSize; ++i)
                fftBuf[(size_t) i] = mono[(size_t) (off + i)] * window[(size_t) i];

            fft.performFrequencyOnlyForwardTransform (fftBuf.data());

            float s = 0.0f;
            for (int b = 0; b < fftSize / 2; ++b)
            {
                const float m = fftBuf[(size_t) b];
                s += juce::jmax (0.0f, m - prevMag[(size_t) b]);
                prevMag[(size_t) b] = m;
            }
            flux[(size_t) f] = s;

            // ---- PHASE3_SPEC.md PART 1: per-frame feature extraction --------
            // Reuses this frame's fftBuf magnitudes (already computed above,
            // no second FFT pass) and the same windowed mono samples for ZCR.
            gent::FrameFeatures ff {};
            ff.band[0] = ff.band[1] = ff.band[2] = ff.band[3] = 0.0f;

            float centWeightedSum = 0.0f;
            float centMagSum = 0.0f;

            for (int b = 0; b < fftSize / 2; ++b)
            {
                const float mag = fftBuf[(size_t) b];
                const float freq = (float) b * (float) sr / (float) fftSize;
                const float e = mag * mag;

                if (freq < kBandEdgeLowHz)          ff.band[0] += e;
                else if (freq < kBandEdgeMidHz)     ff.band[1] += e;
                else if (freq < kBandEdgeHighHz)    ff.band[2] += e;
                else                                ff.band[3] += e;

                centWeightedSum += mag * freq;
                centMagSum += mag;
            }
            ff.centroidHz = centMagSum > 0.0f ? (centWeightedSum / centMagSum) : 0.0f;

            // ZCR over the same raw (unwindowed) 1024-sample time-domain
            // region — Hann is non-negative so windowing barely perturbs sign
            // changes, but the raw samples are the cleanest, most standard
            // definition of ZCR and avoid any windowing-taper edge artifacts.
            {
                int crossings = 0;
                for (int i = 1; i < fftSize; ++i)
                {
                    const float a = mono[(size_t) (off + i - 1)];
                    const float b = mono[(size_t) (off + i)];
                    if ((a >= 0.0f) != (b >= 0.0f))
                        ++crossings;
                }
                ff.zcr = (float) crossings / (float) fftSize;
            }

            // 12-bin chroma fold — MIRRORS the key-detector's fold below
            // (same freq range 55-1760 Hz, same bin->Hz, same MIDI/pitch-class
            // fold), applied per-frame to this frame's 1024-pt magnitudes
            // instead of the global 4096-pt key-detection pass.
            {
                double chromaAccum[12] = {};
                for (int b = 1; b < fftSize / 2; ++b)
                {
                    const double freq = (double) b * sr / (double) fftSize;
                    if (freq < 55.0 || freq > 1760.0) continue;
                    const int midi = (int) std::round (69.0 + 12.0 * std::log2 (freq / 440.0));
                    chromaAccum[((midi % 12) + 12) % 12] += (double) fftBuf[(size_t) b];
                }
                for (int c = 0; c < 12; ++c)
                    ff.chroma[c] = (float) chromaAccum[c];
            }

            r.frames.push_back (ff);
        }

        r.hop = hop;
        r.frameRate = sr / (double) hop;

        if (numFrames < 16)
            return r;

        // ---- BPM via autocorrelation ---------------------------------------
        double mean = 0.0;
        for (auto v : flux) mean += v;
        mean /= (double) flux.size();

        std::vector<float> ons (flux.size());
        for (size_t i = 0; i < flux.size(); ++i)
            ons[i] = flux[i] - (float) mean;

        const double frameRate = sr / (double) hop;

        auto acAt = [&] (int lag) -> double
        {
            const int cnt = (int) ons.size() - lag;
            if (lag < 1 || cnt <= 8) return 0.0;
            double s = 0.0;
            for (int i = 0; i < cnt; ++i)
                s += (double) ons[(size_t) i] * (double) ons[(size_t) (i + lag)];
            return s / (double) cnt;
        };

        double bestScore = -1.0e30, bestBpm = 0.0;
        for (double bpm = 60.0; bpm <= 180.0; bpm += 0.25)
        {
            const int lag = (int) std::round (frameRate * 60.0 / bpm);
            if (lag < 2 || lag >= (int) ons.size() / 2) continue;
            const double sc = acAt (lag) + 0.5 * acAt (lag * 2);
            if (sc > bestScore) { bestScore = sc; bestBpm = bpm; }
        }
        r.bpm = bestBpm;

        // ---- transient slicing (peak picking on flux) ----------------------
        {
            double var = 0.0;
            for (auto v : flux) var += ((double) v - mean) * ((double) v - mean);
            const double sd = std::sqrt (var / (double) flux.size());
            const float thresh = (float) (mean + 0.5 * sd);
            const int minGap = juce::jmax (2, (int) (0.08 * frameRate));   // 80 ms

            struct Pk { int frame; float v; };
            std::vector<Pk> peaks;
            for (int i = 1; i + 1 < (int) flux.size(); ++i)
                if (flux[(size_t) i] > thresh
                    && flux[(size_t) i] >= flux[(size_t) (i - 1)]
                    && flux[(size_t) i] >= flux[(size_t) (i + 1)])
                    peaks.push_back ({ i, flux[(size_t) i] });

            std::sort (peaks.begin(), peaks.end(), [] (const Pk& a, const Pk& b) { return a.v > b.v; });

            std::vector<int> chosen;
            for (const auto& p : peaks)
            {
                bool ok = true;
                for (int c : chosen)
                    if (std::abs (c - p.frame) < minGap) { ok = false; break; }
                if (ok) chosen.push_back (p.frame);
                if ((int) chosen.size() >= 16) break;
            }
            std::sort (chosen.begin(), chosen.end());
            for (int c : chosen) r.slices.push_back (c * hop);

            // full onset list for music-aware slicing: every local maximum above the
            // mean, with strength normalized to 0..1 (sensitivity filters it later)
            {
                float maxF = 0.0f;
                for (float v : flux) maxF = juce::jmax (maxF, v);
                const float base = (float) mean;
                const float span = juce::jmax (1.0e-6f, maxF - base);
                for (int i = 1; i + 1 < (int) flux.size(); ++i)
                    if (flux[(size_t) i] > base
                        && flux[(size_t) i] >= flux[(size_t) (i - 1)]
                        && flux[(size_t) i] >= flux[(size_t) (i + 1)])
                        r.onsets.push_back ({ i * hop,
                            juce::jlimit (0.0f, 1.0f, (flux[(size_t) i] - base) / span) });
            }

            // make sure pad 1 starts at (or near) the top of the file
            if (r.slices.empty() || r.slices[0] > (int) (0.05 * sr))
            {
                if ((int) r.slices.size() >= 16) r.slices.pop_back();
                r.slices.insert (r.slices.begin(), 0);
            }
        }

        // ---- key detection (chromagram + Krumhansl profiles) ----------------
        {
            constexpr int kOrder = 12;
            constexpr int kSize  = 1 << kOrder;   // 4096
            constexpr int kHop   = kSize;

            juce::dsp::FFT fft2 (kOrder);
            std::vector<float> w2 ((size_t) kSize);
            for (int i = 0; i < kSize; ++i)
                w2[(size_t) i] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (kSize - 1)));

            std::vector<float> buf2 ((size_t) (kSize * 2), 0.0f);
            double chroma[12] = {};
            const int frames2 = juce::jmax (0, (n - kSize) / kHop);

            for (int f = 0; f < frames2; ++f)
            {
                std::fill (buf2.begin(), buf2.end(), 0.0f);
                const int off = f * kHop;
                for (int i = 0; i < kSize; ++i)
                    buf2[(size_t) i] = mono[(size_t) (off + i)] * w2[(size_t) i];

                fft2.performFrequencyOnlyForwardTransform (buf2.data());

                for (int b = 1; b < kSize / 2; ++b)
                {
                    const double freq = (double) b * sr / (double) kSize;
                    if (freq < 55.0 || freq > 1760.0) continue;
                    const int midi = (int) std::round (69.0 + 12.0 * std::log2 (freq / 440.0));
                    chroma[((midi % 12) + 12) % 12] += (double) buf2[(size_t) b];
                }
            }

            static const double majP[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
            static const double minP[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
            static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

            auto corr = [&] (const double* prof, int rot) -> double
            {
                double mx = 0.0, my = 0.0;
                for (int i = 0; i < 12; ++i) { mx += chroma[i]; my += prof[i]; }
                mx /= 12.0; my /= 12.0;
                double num = 0.0, dx = 0.0, dy = 0.0;
                for (int i = 0; i < 12; ++i)
                {
                    const double a = chroma[(i + rot) % 12] - mx;
                    const double b = prof[i] - my;
                    num += a * b; dx += a * a; dy += b * b;
                }
                return num / std::sqrt (juce::jmax (1.0e-12, dx * dy));
            };

            double best = -2.0;
            for (int rot = 0; rot < 12; ++rot)
            {
                const double cM = corr (majP, rot);
                const double cm = corr (minP, rot);
                if (cM > best) { best = cM; r.key = juce::String (names[rot]) + " Maj"; }
                if (cm > best) { best = cm; r.key = juce::String (names[rot]) + " Min"; }
            }
        }

        return r;
    }
};

} // namespace gent
