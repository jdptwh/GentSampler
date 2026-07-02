#pragma once

// =============================================================================
//  ModelDownloader.h  --  GentSampler first-run weight downloader
//
//  Pulls the five Demucs ONNX model weights (~1.79 GB total, MIT-licensed) from
//  a public Hugging Face mirror on first use, verifies each file's SHA-256, and
//  writes the manifest.json the engine expects (the host serves only the .onnx
//  files). Downloads stream to <name>.part and are atomically renamed into place
//  only after the checksum passes, so the engine never sees a partial file and
//  antivirus never scans a half-written weight.
//
//  Windows note: juce::WebInputStream uses the native WinINet stack, so HTTPS +
//  redirect-following work WITHOUT libcurl / JUCE_USE_CURL.
// =============================================================================

#include <JuceHeader.h>
#include <functional>

namespace GentModels
{
    // overall fraction 0..1, plus a short human label (e.g. "htdemucs_ft_1.onnx")
    using ProgressFn = std::function<void (float fraction, const juce::String& label)>;
    using StatusFn   = std::function<void (const juce::String& status)>;
    using CancelFn   = std::function<bool ()>;   // return true to abort the download

    // True only if all 5 .onnx files (correct byte size) AND manifest.json exist.
    bool modelsPresent (const juce::File& modelsDir);

    // Ensure every model file is present and checksum-valid, downloading whatever
    // is missing or wrong. Resumes interrupted .part files. Returns true on success
    // (the engine can now initialise) or false + errorOut on failure / cancellation.
    // Cheap no-op once everything is in place, so it is safe to call before every
    // separation. Runs synchronously on the calling (worker) thread.
    bool ensureModelsPresent (const juce::File& modelsDir,
                              ProgressFn   progress,
                              StatusFn     status,
                              CancelFn     cancel,
                              juce::String& errorOut);

    // Total bytes that a clean download would transfer (~1.79 GB).
    juce::int64 totalDownloadBytes();

    // ---- Basic Pitch transcription model (independent of the Demucs set) ----
    // The tiny (<20 MB) Spotify Basic Pitch ONNX, fetched on first TRANSCRIBE so the
    // audio-to-MIDI feature doesn't pull the ~1.79 GB stem weights (and vice versa).
    juce::File basicPitchFile (const juce::File& modelsDir);        // <modelsDir>/basic_pitch.onnx
    bool       basicPitchPresent (const juce::File& modelsDir);     // present + correct byte size
    bool       ensureBasicPitchPresent (const juce::File& modelsDir,
                                        ProgressFn progress, StatusFn status, CancelFn cancel,
                                        juce::String& errorOut);
}
