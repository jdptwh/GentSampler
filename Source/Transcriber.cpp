// =============================================================================
//  Transcriber.cpp  --  Basic Pitch (ICASSP 2022) audio-to-MIDI, in-process ONNX.
//  Port verified against spotify/basic-pitch main (constants.py, inference.py,
//  note_creation.py). v1: notes only, melodia_trick OFF, no min/max-freq constrain.
// =============================================================================

#include "Transcriber.h"
#include "StemSeparator.h"   // gentEnsureOrtLoaded()

// Manual-init: the runtime DLL + Ort::InitApi are loaded once by StemSeparator
// (gentEnsureOrtLoaded). Ort::Global::api_ is process-wide, so once that has run
// our Ort:: calls here use the same table. ZERO load-time dependency on the DLL.
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#undef ORT_API_MANUAL_INIT

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace bp
{
    // --- verified Basic Pitch constants -------------------------------------
    constexpr int    kSampleRate   = 22050;
    constexpr int    kFftHop       = 256;
    constexpr int    kWindowLenS   = 2;
    constexpr int    kAudioNSamples = kSampleRate * kWindowLenS - kFftHop;     // 43844
    constexpr int    kAnnotFps     = kSampleRate / kFftHop;                    // 86
    constexpr int    kAnnotNFrames = kAnnotFps * kWindowLenS;                  // 172
    constexpr int    kNFreqNotes   = 88;
    constexpr int    kNFreqContours = 264;
    constexpr int    kMidiOffset   = 21;
    constexpr int    kMaxFreqIdx   = 87;
    constexpr int    kEnergyTol    = 11;
    constexpr int    kNOverlapFrames = 30;
    constexpr int    kOverlapLen   = kNOverlapFrames * kFftHop;                // 7680
    constexpr int    kHopSize      = kAudioNSamples - kOverlapLen;             // 36164
    constexpr double kMagicAlignOffset = 0.0018;
}

struct BasicPitchTranscriber::Impl
{
    std::unique_ptr<Ort::Env>     env;
    std::unique_ptr<Ort::Session> session;
    std::string inputName;
    std::string noteName, onsetName, contourName;   // resolved by shape + name
    bool ready = false;

    // ---- frame -> seconds (model_frames_to_time) ----------------------------
    static double frameToSec (int f)
    {
        const double original = (double) f * (double) bp::kFftHop / (double) bp::kSampleRate;
        const double winNum   = std::floor ((double) f / (double) bp::kAnnotNFrames);
        const double winOffset = ((double) bp::kFftHop / (double) bp::kSampleRate)
                                 * ((double) bp::kAnnotNFrames - (double) bp::kAudioNSamples / (double) bp::kFftHop)
                                 + bp::kMagicAlignOffset;
        return original - winOffset * winNum;
    }
};

BasicPitchTranscriber::BasicPitchTranscriber() : impl (std::make_unique<Impl>()) {}
BasicPitchTranscriber::~BasicPitchTranscriber() = default;

bool BasicPitchTranscriber::isLoaded() const noexcept { return impl->ready; }

bool BasicPitchTranscriber::ensureLoaded (const juce::File& modelFile, juce::String& errorOut)
{
    if (impl->ready)
        return true;

    if (! modelFile.existsAsFile())
    {
        errorOut = "Basic Pitch model not found: " + modelFile.getFullPathName();
        return false;
    }
    if (! gentEnsureOrtLoaded())
    {
        errorOut = "ONNX Runtime could not be loaded";
        return false;
    }

    try
    {
        impl->env = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_WARNING, "gentsampler-bp");

        Ort::SessionOptions opts;
        opts.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
        opts.SetIntraOpNumThreads ((int) juce::SystemStats::getNumCpus());

       #if JUCE_WINDOWS
        const std::wstring wpath = modelFile.getFullPathName().toWideCharPointer();
        impl->session = std::make_unique<Ort::Session> (*impl->env, wpath.c_str(), opts);
       #else
        impl->session = std::make_unique<Ort::Session> (*impl->env,
                            modelFile.getFullPathName().toRawUTF8(), opts);
       #endif

        Ort::AllocatorWithDefaultOptions alloc;

        // --- input name (single input) ---
        if (impl->session->GetInputCount() < 1) { errorOut = "model has no inputs"; return false; }
        impl->inputName = impl->session->GetInputNameAllocated (0, alloc).get();

        // --- map outputs to note/onset/contour by shape (width) + name suffix ---
        // contour = the width-264 output; of the two width-88 outputs, the trailing
        // ":1" is note and ":2" is onset (basic-pitch export); fall back to declared
        // order (lower index = note) if the names don't carry those suffixes.
        const size_t nOut = impl->session->GetOutputCount();
        struct Out { std::string name; int width; int suffix; };
        std::vector<Out> outs;
        for (size_t i = 0; i < nOut; ++i)
        {
            Out o;
            o.name = impl->session->GetOutputNameAllocated (i, alloc).get();
            const auto shape = impl->session->GetOutputTypeInfo (i)
                                   .GetTensorTypeAndShapeInfo().GetShape();
            o.width = shape.empty() ? 0 : (int) shape.back();
            o.suffix = -1;
            const auto colon = o.name.find_last_of (':');
            if (colon != std::string::npos)
                o.suffix = o.name.substr (colon + 1).front() - '0';
            outs.push_back (o);
        }

        int contourI = -1, noteI = -1, onsetI = -1;
        for (int i = 0; i < (int) outs.size(); ++i)
            if (outs[(size_t) i].width == bp::kNFreqContours) contourI = i;
        // the two non-contour outputs: assign by suffix, else by declared order
        std::vector<int> eighties;
        for (int i = 0; i < (int) outs.size(); ++i)
            if (i != contourI) eighties.push_back (i);
        for (int i : eighties)
        {
            if      (outs[(size_t) i].suffix == 1) noteI  = i;
            else if (outs[(size_t) i].suffix == 2) onsetI = i;
        }
        if (noteI < 0 || onsetI < 0)   // suffix mapping failed -> declared order
        {
            if (eighties.size() >= 2) { noteI = eighties[0]; onsetI = eighties[1]; }
        }
        if (contourI < 0 && eighties.size() >= 2)   // no 264 width? treat 3rd as contour
            for (int i = 0; i < (int) outs.size(); ++i)
                if (i != noteI && i != onsetI) contourI = i;

        // shape guard: the read loop assumes 88/88/264 strides, so reject anything else
        // (incl. a fallback-mapped output of the wrong width) -> clean fail, never OOB.
        if (noteI    >= 0 && outs[(size_t) noteI].width    != bp::kNFreqNotes)    noteI    = -1;
        if (onsetI   >= 0 && outs[(size_t) onsetI].width   != bp::kNFreqNotes)    onsetI   = -1;
        if (contourI >= 0 && outs[(size_t) contourI].width != bp::kNFreqContours) contourI = -1;

        if (noteI < 0 || onsetI < 0 || contourI < 0)
        {
            errorOut = "could not map model outputs to note/onset/contour (unexpected shapes)";
            return false;
        }
        impl->noteName    = outs[(size_t) noteI].name;
        impl->onsetName   = outs[(size_t) onsetI].name;
        impl->contourName = outs[(size_t) contourI].name;

        impl->ready = true;
        return true;
    }
    catch (const Ort::Exception& e)
    {
        errorOut = juce::String ("Basic Pitch session failed: ") + e.what();
        impl->session.reset();
        impl->env.reset();
        return false;
    }
}

// ---------------------------------------------------------------------------
//  helpers
// ---------------------------------------------------------------------------
namespace
{
    // mono downmix + linear-interp resample to 22050 Hz
    std::vector<float> toMono22k (const juce::AudioBuffer<float>& in, double inSR)
    {
        const int nCh = juce::jmax (1, in.getNumChannels());
        const int nIn = in.getNumSamples();
        std::vector<float> mono ((size_t) juce::jmax (0, nIn));
        for (int i = 0; i < nIn; ++i)
        {
            float s = 0.0f;
            for (int c = 0; c < nCh; ++c) s += in.getReadPointer (c)[i];
            mono[(size_t) i] = s / (float) nCh;
        }
        if (nIn <= 1 || std::abs (inSR - bp::kSampleRate) < 1.0)
            return mono;

        const double ratio = (double) bp::kSampleRate / inSR;       // out per in
        const int outLen = juce::jmax (1, (int) std::floor ((double) nIn * ratio));
        std::vector<float> out ((size_t) outLen);
        for (int j = 0; j < outLen; ++j)
        {
            const double srcPos = (double) j / ratio;
            const int i0 = (int) std::floor (srcPos);
            const int i1 = juce::jmin (nIn - 1, i0 + 1);
            const float frac = (float) (srcPos - (double) i0);
            out[(size_t) j] = mono[(size_t) i0] + frac * (mono[(size_t) i1] - mono[(size_t) i0]);
        }
        return out;
    }

    // get_infered_onsets: onsets <- max(onsets, rescaled min-diff of frames). In place.
    void inferOnsets (std::vector<float>& onsets, const std::vector<float>& frames, int F, int W)
    {
        std::vector<float> fdiff ((size_t) F * (size_t) W, 0.0f);
        for (int t = 0; t < F; ++t)
            for (int k = 0; k < W; ++k)
            {
                const float cur = frames[(size_t) t * W + k];
                const float d1 = cur - (t >= 1 ? frames[(size_t) (t - 1) * W + k] : 0.0f);
                const float d2 = cur - (t >= 2 ? frames[(size_t) (t - 2) * W + k] : 0.0f);
                float d = juce::jmin (d1, d2);
                if (d < 0.0f) d = 0.0f;
                fdiff[(size_t) t * W + k] = d;
            }
        for (int k = 0; k < W; ++k) { fdiff[k] = 0.0f; if (F > 1) fdiff[(size_t) W + k] = 0.0f; } // first 2 rows

        float maxOn = 0.0f, maxFd = 0.0f;
        for (float v : onsets) maxOn = juce::jmax (maxOn, v);
        for (float v : fdiff)  maxFd  = juce::jmax (maxFd, v);
        if (maxFd > 0.0f)
        {
            const float scale = maxOn / maxFd;
            for (size_t i = 0; i < onsets.size(); ++i)
                onsets[i] = juce::jmax (onsets[i], fdiff[i] * scale);
        }
    }
}

// ---------------------------------------------------------------------------
//  transcribe
// ---------------------------------------------------------------------------
std::vector<BasicPitchTranscriber::Note>
BasicPitchTranscriber::transcribe (const juce::AudioBuffer<float>& slice, double sliceSampleRate,
                                   float onsetThresh, float frameThresh, float minNoteLenMs)
{
    std::vector<Note> result;
    if (! impl->ready || slice.getNumSamples() < 2)
        return result;

    // ---- Task B: mono @ 22050, head-pad, window ----
    std::vector<float> mono = toMono22k (slice, sliceSampleRate);
    const int origLen = (int) mono.size();
    if (origLen < 2) return result;

    const int headPad = bp::kOverlapLen / 2;                 // 3840
    std::vector<float> padded ((size_t) headPad + mono.size(), 0.0f);
    std::copy (mono.begin(), mono.end(), padded.begin() + headPad);
    const int paddedLen = (int) padded.size();
    const int nWindows = (paddedLen - 1) / bp::kHopSize + 1;  // == len(range(0,paddedLen,hop))

    // ---- Task C: inference per window, then unwrap (drop 15 head/tail frames) ----
    const int nOlap = bp::kNOverlapFrames / 2;               // 15
    std::vector<float> noteCat, onsetCat, contourCat;        // concatenated (frames, W)
    int F = bp::kAnnotNFrames;                               // resolved from 1st run

    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
    const char* inNames[]  = { impl->inputName.c_str() };
    const char* outNames[] = { impl->noteName.c_str(), impl->onsetName.c_str(), impl->contourName.c_str() };

    for (int w = 0; w < nWindows; ++w)
    {
        std::array<float, bp::kAudioNSamples> win {};
        const int start = w * bp::kHopSize;
        for (int i = 0; i < bp::kAudioNSamples; ++i)
        {
            const int idx = start + i;
            win[(size_t) i] = (idx < paddedLen) ? padded[(size_t) idx] : 0.0f;
        }

        std::array<int64_t, 3> inShape { 1, (int64_t) bp::kAudioNSamples, 1 };
        Ort::Value inT = Ort::Value::CreateTensor<float> (memInfo, win.data(), win.size(),
                                                          inShape.data(), inShape.size());
        std::vector<Ort::Value> outs;
        try
        {
            outs = impl->session->Run (Ort::RunOptions { nullptr }, inNames, &inT, 1, outNames, 3);
        }
        catch (const Ort::Exception&) { return result; }   // bail cleanly on any inference error

        auto shapeOf = [] (Ort::Value& v) { return v.GetTensorTypeAndShapeInfo().GetShape(); };
        const auto nShp = shapeOf (outs[0]);   // {1, F, 88}
        const int  Fw   = (nShp.size() >= 2) ? (int) nShp[nShp.size() - 2] : bp::kAnnotNFrames;
        F = Fw;
        const float* noteP = outs[0].GetTensorData<float>();
        const float* onP   = outs[1].GetTensorData<float>();
        const float* coP   = outs[2].GetTensorData<float>();

        const int lo = (Fw > 2 * nOlap) ? nOlap : 0;
        const int hi = (Fw > 2 * nOlap) ? Fw - nOlap : Fw;
        for (int t = lo; t < hi; ++t)
        {
            for (int k = 0; k < bp::kNFreqNotes; ++k)
            {
                noteCat.push_back  (noteP[(size_t) t * bp::kNFreqNotes + k]);
                onsetCat.push_back (onP  [(size_t) t * bp::kNFreqNotes + k]);
            }
            for (int k = 0; k < bp::kNFreqContours; ++k)
                contourCat.push_back (coP[(size_t) t * bp::kNFreqContours + k]);
        }
    }

    int nFrames = (int) (noteCat.size() / bp::kNFreqNotes);
    if (nFrames < 2) return result;

    // trim to the expected coverage of the real (pre-pad) audio. Reference unwrap_output
    // takes n_frames_per_window from CONSTANTS (ANNOT_N_FRAMES - N_OVERLAPPING_FRAMES = 142),
    // not the per-window output shape; trim even when small (a sub-2-frame result -> no notes).
    const int perWin = bp::kAnnotNFrames - bp::kNOverlapFrames;          // 142
    const int trimTo = (int) ((double) origLen / (double) bp::kHopSize * (double) perWin);
    if (trimTo < nFrames)
        nFrames = juce::jmax (0, trimTo);
    if (nFrames < 2) return result;

    // ---- Task D: output_to_notes_polyphonic (melodia OFF, no freq constrain) ----
    const int W = bp::kNFreqNotes;
    std::vector<float> note (noteCat.begin(), noteCat.begin() + (size_t) nFrames * W);
    std::vector<float> onset (onsetCat.begin(), onsetCat.begin() + (size_t) nFrames * W);

    inferOnsets (onset, note, nFrames, W);

    // peak-pick onsets along time (strict local maxima, edges excluded)
    // reference: round(ms/1000 * AUDIO_SAMPLE_RATE/FFT_HOP) — FLOAT 86.1328, not int 86
    const int minNoteLen = juce::jmax (1, (int) std::lround ((double) minNoteLenMs / 1000.0
                                       * ((double) bp::kSampleRate / (double) bp::kFftHop)));
    std::vector<std::pair<int,int>> onsetIdx;   // (frame, freq), frame-major order
    for (int t = 1; t < nFrames - 1; ++t)
        for (int k = 0; k < W; ++k)
        {
            const float v = onset[(size_t) t * W + k];
            if (v >= onsetThresh
                && v > onset[(size_t) (t - 1) * W + k]
                && v > onset[(size_t) (t + 1) * W + k])
                onsetIdx.emplace_back (t, k);
        }

    std::vector<float> energy (note.begin(), note.end());   // remaining_energy = copy(frames)

    // iterate onsets backwards in time (reverse of frame-major order)
    for (auto it = onsetIdx.rbegin(); it != onsetIdx.rend(); ++it)
    {
        const int startF = it->first;
        const int k      = it->second;
        if (startF >= nFrames - 1) continue;

        int i = startF + 1, kk = 0;
        while (i < nFrames - 1 && kk < bp::kEnergyTol)
        {
            if (energy[(size_t) i * W + k] < frameThresh) ++kk; else kk = 0;
            ++i;
        }
        i -= kk;
        if (i - startF <= minNoteLen) continue;

        for (int t = startF; t < i; ++t)
        {
            energy[(size_t) t * W + k] = 0.0f;
            if (k < bp::kMaxFreqIdx) energy[(size_t) t * W + (k + 1)] = 0.0f;
            if (k > 0)               energy[(size_t) t * W + (k - 1)] = 0.0f;
        }

        double sum = 0.0;
        for (int t = startF; t < i; ++t) sum += note[(size_t) t * W + k];
        const float amp = (float) (sum / juce::jmax (1, i - startF));

        Note n;
        n.startSec  = Impl::frameToSec (startF);
        n.endSec    = Impl::frameToSec (i);
        n.pitch     = k + bp::kMidiOffset;
        n.amplitude = juce::jlimit (0.0f, 1.0f, amp);
        result.push_back (n);
    }

    std::sort (result.begin(), result.end(),
               [] (const Note& a, const Note& b) { return a.startSec < b.startSec; });
    return result;
}
