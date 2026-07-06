#include "ModelDownloader.h"

namespace GentModels
{
namespace
{
    // Public, no-auth Hugging Face mirror (MIT-licensed Demucs weights).
    // A full URL is <base> + <filename>, e.g.
    //   https://huggingface.co/illicitish/gentsampler-models/resolve/main/htdemucs_6s_0.onnx
    constexpr const char* kBaseUrl =
        "https://huggingface.co/illicitish/gentsampler-models/resolve/main/";

    struct ModelFile
    {
        const char* name;
        juce::int64 size;        // exact byte size (also the progress denominator)
        const char* sha256;      // lowercase hex, verified after download
    };

    // SHA-256 + sizes computed from the validated local weights in
    // C:\Users\JoeyD\Desktop\GentSampler\StemTest\onnx_models on 2026-06-23.
    const ModelFile kFiles[] =
    {
        { "htdemucs_6s_0.onnx", 311822900LL, "ba7eb9f7032597943aec9047f3bf688f6232945641763996490782e3b28b576a" },
        { "htdemucs_ft_0.onnx", 370110092LL, "3ffffcd82c370e11f3e74ab35b0d2d6eb6dc1d66e9aa812d05c182016670fe37" },
        { "htdemucs_ft_1.onnx", 370110092LL, "c8e426f7a59bde9cc9877eafe9d07d520ee24004d3cc8a99e7d767a012ee93d1" },
        { "htdemucs_ft_2.onnx", 370110092LL, "9076bbe2b05fbec34527e9ad13efe922cabcaaacc0e5a539fccf932dfc29e72f" },
        { "htdemucs_ft_3.onnx", 370110092LL, "7b27daa40e99ec8af7007170e7d7813fb28198374146c1035930fc79c2875e27" },
    };
    constexpr int kNumFiles = (int) (sizeof (kFiles) / sizeof (kFiles[0]));

    // Basic Pitch (Spotify ICASSP-2022) ONNX — a separate, tiny (~225 KB) CPU model for
    // audio-to-MIDI transcription. Downloaded on first TRANSCRIBE (independent of the
    // ~1.79 GB Demucs set, which gates stem separation). Apache-2.0 (bundle NOTICE).
    // size + SHA-256 computed 2026-06-30 from basic_pitch/saved_models/icassp_2022/nmp.onnx
    // (pip basic-pitch), hosted as basic_pitch.onnx on the same HF mirror.
    const ModelFile kBasicPitch =
        { "basic_pitch.onnx", 230444LL, "2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec" };

    // The engine requires manifest.json next to the weights, but the host serves
    // only the .onnx files, so we write the (tiny) manifest locally. This is the
    // exact export_onnx.py output for the hybrid ft + 6s model set.
    const char* kManifestJson = R"JSON({
  "models": [
    {
      "name": "htdemucs_6s",
      "sources": [ "drums", "bass", "other", "vocals", "guitar", "piano" ],
      "samplerate": 44100,
      "segment": 7.8,
      "n_submodels": 1,
      "bag_weights": [ [ 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 ] ],
      "onnx_files": [ "htdemucs_6s_0.onnx" ]
    },
    {
      "name": "htdemucs_ft",
      "sources": [ "drums", "bass", "other", "vocals" ],
      "samplerate": 44100,
      "segment": 7.8,
      "n_submodels": 4,
      "bag_weights": [
        [ 1.0, 0.0, 0.0, 0.0 ],
        [ 0.0, 1.0, 0.0, 0.0 ],
        [ 0.0, 0.0, 1.0, 0.0 ],
        [ 0.0, 0.0, 0.0, 1.0 ]
      ],
      "onnx_files": [
        "htdemucs_ft_0.onnx",
        "htdemucs_ft_1.onnx",
        "htdemucs_ft_2.onnx",
        "htdemucs_ft_3.onnx"
      ]
    }
  ]
})JSON";

    bool sha256Matches (const juce::File& f, const juce::String& expected)
    {
        juce::FileInputStream in (f);
        if (! in.openedOk())
            return false;
        const juce::SHA256 hash (in);
        return hash.toHexString().equalsIgnoreCase (expected);
    }

    bool fileComplete (const juce::File& f, const ModelFile& mf)
    {
        return f.existsAsFile() && f.getSize() == mf.size;
    }

    // WAVE1_SPEC.md F5 (audit #4 stem-engine): first-run downloads to the
    // shared Documents\GentSampler\models folder had no cross-instance/
    // cross-process coordination -- two instances hitting Separate/Transcribe
    // around the same time could both open independent FileOutputStreams on
    // the identical <name>.part path (interleaved writes) and both race the
    // finalize (delete + rename) step. One named juce::InterProcessLock
    // (Windows named mutex; released by the OS on process death, abandoned-
    // mutex safe) held around the whole download section serializes that;
    // per-file locking was rejected as unneeded complexity for a first-run-
    // only path. Blocks in a bounded enter(500) loop rather than a single
    // unbounded wait or a spin loop: each timeout re-checks whether the OTHER
    // instance already finished (isPresent), in which case this instance
    // returns success without downloading, and pushes a status string so a
    // waiting instance's UI isn't silently stalled.
    bool acquireDownloadLock (juce::InterProcessLock& ipLock,
                              const std::function<bool()>& isPresent,
                              StatusFn status)
    {
        for (;;)
        {
            if (ipLock.enter (500))
                return true;
            if (isPresent())        // the other instance finished while we waited
                return false;       // caller re-checks presence and returns success
            status ("waiting for another GentSampler to finish downloading models...");
        }
    }

    // Download one weight to <name>.part (resuming any partial), verify its
    // SHA-256, then atomically move it into place. Reports cumulative progress
    // across the whole set. Retries a few times; keeps the .part for resume on a
    // transient failure, deletes it on a checksum mismatch (bad bytes).
    bool downloadOne (const ModelFile& mf,
                      const juce::File& modelsDir,
                      juce::int64 bytesBefore,    // sum of fully-done files ahead of this one
                      juce::int64 grandTotal,
                      ProgressFn progress,
                      StatusFn   status,
                      CancelFn   cancel,
                      juce::String& errorOut)
    {
        const juce::File finalF = modelsDir.getChildFile (mf.name);
        const juce::File partF  = modelsDir.getChildFile (juce::String (mf.name) + ".part");

        if (fileComplete (finalF, mf))
            return true;

        constexpr int kMaxAttempts = 4;
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt)
        {
            if (cancel && cancel()) { errorOut = "cancelled"; return false; }

            juce::int64 resumeFrom = (partF.existsAsFile() ? partF.getSize() : 0);
            if (resumeFrom > mf.size) { partF.deleteFile(); resumeFrom = 0; }   // corrupt / oversized

            juce::URL url (juce::String (kBaseUrl) + mf.name);
            juce::WebInputStream web (url, false);
            web.withConnectionTimeout (30000).withNumRedirectsToFollow (8);
            if (resumeFrom > 0)
                web.withExtraHeaders ("Range: bytes=" + juce::String (resumeFrom) + "-");

            status ("Downloading " + juce::String (mf.name)
                    + (attempt > 1 ? "  (retry " + juce::String (attempt - 1) + ")" : juce::String()));

            if (! web.connect (nullptr))
            {
                errorOut = "could not connect for " + juce::String (mf.name);
                juce::Thread::sleep (1500);
                continue;
            }

            const int code = web.getStatusCode();
            bool append = false;
            if (resumeFrom > 0 && code == 206)        { append = true; }                 // server honoured Range
            else if (code == 200 || code == 206)      { append = false; resumeFrom = 0; } // full body
            else
            {
                errorOut = "HTTP " + juce::String (code) + " fetching " + juce::String (mf.name);
                juce::Thread::sleep (1500);
                continue;
            }

            if (! append)
                partF.deleteFile();

            std::unique_ptr<juce::FileOutputStream> out (partF.createOutputStream());
            if (out == nullptr || ! out->openedOk())
            {
                errorOut = "cannot write " + partF.getFullPathName();
                return false;   // not retryable (disk / permissions)
            }
            out->setPosition (append ? resumeFrom : 0);

            juce::int64 done = (append ? resumeFrom : 0);
            juce::HeapBlock<char> buf (1 << 16);
            bool aborted = false, transientErr = false;

            while (! web.isExhausted())
            {
                if (cancel && cancel()) { aborted = true; break; }

                const int n = web.read (buf, 1 << 16);
                if (n <= 0)
                {
                    if (done != mf.size) transientErr = true;   // stream dropped early
                    break;
                }
                if (! out->write (buf, (size_t) n))
                {
                    errorOut = "disk write failed (out of space?) for " + juce::String (mf.name);
                    out.reset();
                    return false;   // not retryable
                }
                done += n;

                const float frac = grandTotal > 0
                    ? (float) ((double) (bytesBefore + done) / (double) grandTotal) : 0.0f;
                progress (juce::jlimit (0.0f, 1.0f, frac), juce::String (mf.name));
            }

            out->flush();
            out.reset();

            if (aborted) { errorOut = "cancelled"; return false; }

            if (done != mf.size || transientErr)
            {
                // incomplete — keep .part for a resumed retry
                errorOut = "download interrupted for " + juce::String (mf.name);
                juce::Thread::sleep (1500);
                continue;
            }

            status ("Verifying " + juce::String (mf.name) + " ...");
            if (! sha256Matches (partF, mf.sha256))
            {
                partF.deleteFile();   // bad bytes: force a clean re-download next attempt
                errorOut = "checksum mismatch for " + juce::String (mf.name);
                continue;
            }

            finalF.deleteFile();
            if (! partF.moveFileTo (finalF))
            {
                errorOut = "could not finalise " + juce::String (mf.name);
                return false;
            }
            return true;
        }

        if (errorOut.isEmpty())
            errorOut = "download failed for " + juce::String (mf.name);
        return false;
    }
} // anonymous namespace

juce::int64 totalDownloadBytes()
{
    juce::int64 t = 0;
    for (auto& f : kFiles)
        t += f.size;
    return t;
}

bool modelsPresent (const juce::File& modelsDir)
{
    if (! modelsDir.getChildFile ("manifest.json").existsAsFile())
        return false;
    for (auto& f : kFiles)
        if (! fileComplete (modelsDir.getChildFile (f.name), f))
            return false;
    return true;
}

bool ensureModelsPresent (const juce::File& modelsDir,
                          ProgressFn progress, StatusFn status, CancelFn cancel,
                          juce::String& errorOut)
{
    if (! progress) progress = [] (float, const juce::String&) {};
    if (! status)   status   = [] (const juce::String&) {};

    if (modelsPresent (modelsDir))
        return true;

    if (! modelsDir.createDirectory())
    {
        errorOut = "cannot create models folder: " + modelsDir.getFullPathName();
        return false;
    }

    // WAVE1_SPEC.md F5: one named cross-instance/cross-process lock around
    // the whole download section (see acquireDownloadLock above). enter()/
    // exit() are managed manually (not ScopedLockType) because acquisition
    // itself needs the bounded retry-with-status-callback loop below;
    // gentUnlockOnExit releases it on every return path once acquired.
    juce::InterProcessLock ipLock ("GentSampler_ModelDownload");
    if (! acquireDownloadLock (ipLock, [&] { return modelsPresent (modelsDir); }, status))
        return modelsPresent (modelsDir);   // the other instance finished while we waited
    const juce::ScopeGuard gentUnlockOnExit { [&ipLock] { ipLock.exit(); } };

    // Re-check once now that we hold the lock: another instance may have
    // finished the whole set while we were waiting to acquire.
    if (modelsPresent (modelsDir))
        return true;

    const juce::int64 grand = totalDownloadBytes();
    juce::int64 before = 0;

    for (int i = 0; i < kNumFiles; ++i)
    {
        const auto& mf = kFiles[i];

        if (! fileComplete (modelsDir.getChildFile (mf.name), mf))
        {
            if (! downloadOne (mf, modelsDir, before, grand, progress, status, cancel, errorOut))
                return false;
        }

        before += mf.size;
        progress (grand > 0 ? (float) ((double) before / (double) grand) : 1.0f, juce::String (mf.name));
    }

    // Weights are all present & verified -> write the manifest the engine needs.
    const juce::File manifest = modelsDir.getChildFile ("manifest.json");
    if (! manifest.existsAsFile())
    {
        if (! manifest.replaceWithText (juce::String (juce::CharPointer_UTF8 (kManifestJson))))
        {
            errorOut = "could not write manifest.json to " + modelsDir.getFullPathName();
            return false;
        }
    }

    status ("Models ready");
    return true;
}

// ---- Basic Pitch (transcription) — independent of the Demucs set ------------

juce::File basicPitchFile (const juce::File& modelsDir)
{
    return modelsDir.getChildFile (kBasicPitch.name);
}

bool basicPitchPresent (const juce::File& modelsDir)
{
    return fileComplete (basicPitchFile (modelsDir), kBasicPitch);
}

bool ensureBasicPitchPresent (const juce::File& modelsDir,
                              ProgressFn progress, StatusFn status, CancelFn cancel,
                              juce::String& errorOut)
{
    if (! progress) progress = [] (float, const juce::String&) {};
    if (! status)   status   = [] (const juce::String&) {};

    // Unconfigured build: size/SHA-256 are still the TODO placeholders, so a download
    // could never verify. Fail fast with a clear message instead of a doomed retry loop.
    if (kBasicPitch.size <= 0)
    {
        errorOut = "transcription model not configured in this build (host basic_pitch.onnx "
                   "and fill its size + SHA-256 in ModelDownloader.cpp)";
        return false;
    }

    if (basicPitchPresent (modelsDir))
        return true;

    if (! modelsDir.createDirectory())
    {
        errorOut = "cannot create models folder: " + modelsDir.getFullPathName();
        return false;
    }

    // WAVE1_SPEC.md F5: the Basic Pitch sibling entry point shares modelsDir
    // with the Demucs set and gets the SAME named lock (per-file locking
    // rejected as unneeded complexity for a first-run-only path).
    juce::InterProcessLock ipLock ("GentSampler_ModelDownload");
    if (! acquireDownloadLock (ipLock, [&] { return basicPitchPresent (modelsDir); }, status))
        return basicPitchPresent (modelsDir);   // the other instance finished while we waited
    const juce::ScopeGuard gentUnlockOnExit { [&ipLock] { ipLock.exit(); } };

    // Re-check once now that we hold the lock: another instance may have
    // finished this file while we were waiting to acquire.
    if (basicPitchPresent (modelsDir))
        return true;

    // single-file download/verify/resume; progress is over this one file.
    // fileComplete() re-check immediately before the download (drafter Q7.5):
    // trivially satisfied here (one file, already re-checked via
    // basicPitchPresent() just above, which IS fileComplete() for this file).
    if (! fileComplete (basicPitchFile (modelsDir), kBasicPitch))
    {
        if (! downloadOne (kBasicPitch, modelsDir, 0, kBasicPitch.size, progress, status, cancel, errorOut))
            return false;
    }

    status ("Transcription model ready");
    return true;
}
} // namespace GentModels
