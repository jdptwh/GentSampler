#pragma once

// =============================================================================
//  Transcriber.h  --  GentSampler audio-to-MIDI (Spotify Basic Pitch, ICASSP 2022)
//
//  A faithful C++ port of the basic-pitch inference + note-creation pipeline,
//  running the icassp_2022 ONNX model IN-PROCESS on the existing ONNX Runtime / CPU
//  rails (the same runtime StemSeparator loads — see gentEnsureOrtLoaded()). One
//  separate Ort::Session; no Python, no sidecar, no GPU.
//
//  Pipeline (all verified against spotify/basic-pitch main):
//    slice audio -> mono @ 22050 Hz -> window(43844, overlap 7680, head-pad 3840)
//    -> session.run -> note/onset/contour -> unwrap (drop 15 head/tail frames)
//    -> output_to_notes_polyphonic -> frame->seconds note events.
//  v1: notes only (no pitch bends), melodia_trick OFF.
// =============================================================================

#include <JuceHeader.h>
#include <memory>
#include <vector>

class BasicPitchTranscriber
{
public:
    BasicPitchTranscriber();
    ~BasicPitchTranscriber();

    // A transcribed note: times in SECONDS relative to the slice start, MIDI pitch
    // (21..108, full range), amplitude 0..1 (-> velocity = round(127*amplitude)).
    struct Note
    {
        double startSec = 0.0;
        double endSec   = 0.0;
        int    pitch    = 0;
        float  amplitude = 0.0f;
    };

    // Lazily create the CPU ORT session for basic_pitch.onnx at modelFile and resolve
    // the input/output tensor names by querying the session (note/onset/contour mapped
    // by shape + name, never hardcoded). Idempotent; returns false + errorOut on failure.
    bool ensureLoaded (const juce::File& modelFile, juce::String& errorOut);
    bool isLoaded() const noexcept;

    // Transcribe a mono/stereo slice sampled at sliceSampleRate into note events.
    // Defaults match basic-pitch inference.py. Returns an empty list if not loaded
    // or the slice is too short. Runs on the calling (worker) thread.
    std::vector<Note> transcribe (const juce::AudioBuffer<float>& slice,
                                  double sliceSampleRate,
                                  float onsetThresh  = 0.5f,
                                  float frameThresh  = 0.3f,
                                  float minNoteLenMs = 127.7f);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BasicPitchTranscriber)
};
