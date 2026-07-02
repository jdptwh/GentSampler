#pragma once

// =============================================================================
//  StemSeparator.h  --  GentSampler ONNX/DirectML stem-separation engine
//
//  Direct C++ port of the validated run_onnx.py pipeline:
//    normalize -> centered segment -> run session(s) -> bag-combine (ft)
//    -> center-trim -> triangular overlap-add -> recombine (hybrid) -> un-normalize
//
//  The STFT/iSTFT are baked INSIDE the ONNX graphs, so this engine needs no FFT.
//  Each .onnx takes float32 [1, channels, 343980] named "mix" and returns
//  float32 [1, n_sources, channels, 343980] named "stems".
//
//  Models live in a folder alongside manifest.json (produced by export_onnx.py):
//      htdemucs_6s_0.onnx
//      htdemucs_ft_0.onnx ... htdemucs_ft_3.onnx
//      manifest.json
// =============================================================================

#include <JuceHeader.h>
#include <functional>
#include <map>
#include <memory>
#include <vector>

// Stage-1 build/link proof. Writes the ONNX Runtime version (and whether a
// models folder is present) to logFile, and returns the same text. Calling this
// from plugin code forces the linker to include the engine + ONNX Runtime, so a
// successful build + readable log proves the whole engine compiles and links.
juce::String gentCheckStemEngine (const juce::File& logFile, const juce::File& modelsDir);

// Ensure the process-global ONNX Runtime is loaded from the plugin folder and its
// C++ API initialised (idempotent; returns false if onnxruntime.dll can't load).
// Other ORT consumers — e.g. the Basic Pitch transcriber — call this before creating
// their own sessions so the manual DLL-load + Ort::InitApi happens once per process.
bool gentEnsureOrtLoaded();

class StemSeparator
{
public:
    StemSeparator();
    ~StemSeparator();

    enum class Mode
    {
        Hybrid,   // ft drums/bass/vocals + 6s guitar/piano + (ft other - 6s guitar - 6s piano)
        FourStem, // htdemucs_ft only: drums, bass, vocals, other
        SixStem   // htdemucs_6s only: drums, bass, other, vocals, guitar, piano
    };

    struct Progress
    {
        int   chunksDone   = 0;
        int   chunksTotal  = 0;
        float fraction     = 0.0f;   // 0..1 over the whole job
        juce::String label;          // e.g. "htdemucs_ft (2/4)"
    };
    using ProgressFn = std::function<void (const Progress&)>;

    // Load manifest.json + create ORT sessions. useGPU walks the provider-priority
    // chain (CUDA -> CPU in v1; CoreML/WebGPU/DirectML are future inserts), falling
    // through to CPU on any failure. Returns false (and fills errorOut) on failure.
    bool initialise (const juce::File& modelsDir, bool useGPU, juce::String& errorOut);

    bool isInitialised() const noexcept { return initialised; }
    bool usingGPU() const noexcept;
    bool deviceFellBackToCpu() const noexcept;   // true if a GPU run failed and we used CPU instead
    juce::String gpuErrorMessage() const;        // why GPU failed (empty if none) — for diagnostics
    juce::String selectedProvider() const;       // the EP that built the live session: "CUDA"|"CPU"|...
    bool canUseMaxQuality() const;               // htdemucs_ft offered only when a GPU provider is usable

    // Separate `input` (any channel count; made stereo internally) sampled at
    // inputSampleRate. Returns stemName -> stereo AudioBuffer at 44100 Hz.
    // Stems are returned at true amplitude (un-normalized, no peak limiting).
    // On failure returns an empty map and fills errorOut.
    std::map<juce::String, juce::AudioBuffer<float>>
        separate (const juce::AudioBuffer<float>& input,
                  double inputSampleRate,
                  Mode mode,
                  float overlap,            // 0.25 = balanced (matches PoC)
                  ProgressFn progress,
                  juce::String& errorOut);

    static constexpr int   kModelSampleRate = 44100;
    static constexpr int   kSegmentSamples  = 343980; // 7.8 s @ 44100
    static const juce::StringArray& sixStemOrder(); // drums,bass,vocals,guitar,piano,other

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    bool initialised = false;
    bool gpuActive   = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparator)
};
