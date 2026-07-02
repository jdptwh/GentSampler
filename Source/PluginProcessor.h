#pragma once
// ============================================================================
//  PluginProcessor.h — GentSampler v1.1 audio engine
//
//  Architecture:
//   - Source sample loaded + analyzed (BPM / key / transients) on a worker
//     thread. Master time-stretch (host tempo sync) + master pitch rendered
//     OFFLINE via Signalsmith Stretch into a RenderedSample → zero-latency
//     pad triggering on the audio thread.
//   - 16 pads play from the rendered buffer. Per-pad pitch is classic
//     repitch (MPC style). Per-pad CRUSH gives SP-1200-style sample-rate
//     hold + bit reduction.
//   - Multi-out: main stereo bus + 16 optional per-pad stereo buses.
//   - Export: per-pad WAV slices, full kit + MIDI map + Flip Log,
//     captured pad-performance MIDI, .gentkit presets.
// ============================================================================

#include <JuceHeader.h>
#include "Analysis.h"
#include "StemSeparator.h"
#include "Transcriber.h"
#include <array>
#include <atomic>
#include <vector>

// parameter-ID helper shared with the editor
inline juce::String pid (int pad, const char* suffix)
{
    return "p" + juce::String (pad) + suffix;
}

// ---------------------------------------------------------------------------
struct SourceSample : juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<SourceSample>;
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
};

// 6 separated stems, aligned to the source (same sample-rate domain).
struct StemSet : juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<StemSet>;
    // order: drums, bass, vocals, guitar, piano, other
    std::array<juce::AudioBuffer<float>, 6> buffers;
    double sampleRate = 44100.0;
    int    generation = 0;
    static const char* name (int i)
    {
        static const char* n[6] = { "drums", "bass", "vocals", "guitar", "piano", "other" };
        return n[i];
    }
};

// the 6 stems after going through the SAME tempo-stretch/pitch as the master
// render, so they line up sample-for-sample with the master rendered buffer.
struct RenderedStems : juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<RenderedStems>;
    std::array<juce::AudioBuffer<float>, 6> buffers;   // rendered (stretched) domain
    double speed = 1.0;
    int    generation = 0;
    int    stemSetGeneration = 0;                      // which StemSet this came from
};

struct RenderedSample : juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<RenderedSample>;
    juce::AudioBuffer<float> buffer;     // in SOURCE sample-rate domain
    double speed = 1.0;                  // source samples consumed per rendered sample
    double sourceSampleRate = 44100.0;
    int generation = 0;
};

// a single pad's independently re-timed slice (per-pad SPEED), pitch preserved
struct PadRender : juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<PadRender>;
    juce::AudioBuffer<float> buffer;     // stretched slice, SOURCE sample-rate domain
    // the 6 stems stretched the SAME way as `buffer` (so they line up sample-for-
    // sample with it) — lets PAD SOURCE work on speed-toggled pads. Empty unless
    // stems are separated; hasStems gates use.
    std::array<juce::AudioBuffer<float>, 6> stems;
    bool   hasStems = false;             // stems[] populated & length-matched to buffer
    int    stemRenderGen = -1;           // RenderedStems generation stems came from (-1 = none)
    double sourceSampleRate = 44100.0;
    int masterGen = 0;                   // master render it was cut from
    int cueSrc = 0, endSrc = 0;          // region stamp (source domain)
    float speed = 1.0f;                  // pad time multiplier
    int generation = 0;
};

// ---------------------------------------------------------------------------
class GentSamplerAudioProcessor : public juce::AudioProcessor,
                                  private juce::Thread
{
public:
    GentSamplerAudioProcessor();
    ~GentSamplerAudioProcessor() override;

    // --- AudioProcessor boilerplate ---
    void prepareToPlay (double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& l) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }
    const juce::String getName() const override            { return "GentSampler"; }
    bool acceptsMidi() const override                      { return true; }
    bool producesMidi() const override                     { return false; }
    bool isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 0.0; }
    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // --- public API used by the editor ---
    juce::AudioProcessorValueTreeState apvts;

    bool loadFile (const juce::File&, bool runAnalysis = true);
    void uiTrigger (int pad, bool on);
    void setCue (int pad, int samplePos, bool snap = false);   // region start (snap only on message thread)
    void clearCue (int pad);                               // unassign: pad has no region, no sound
    int  getCue (int pad) const                            { return cues[(size_t) pad].load(); }
    void setCueEnd (int pad, int samplePos);               // explicit region end; -1 = auto; collapsed = open
    int  getCueEnd (int pad) const                         { return cueEnds[(size_t) pad].load(); }
    int  getEffectiveCueEnd (int pad) const;               // explicit end, else next cue / sample end
    // cueEnds sentinels: -1 = auto (next cue / sample end), kOpenSlice = open/gated
    // ("collapsed" window: play from start, gate/release decides the cut, ignore boundaries)
    static constexpr int kOpenSlice = -2;
    bool isOpenSlice (int pad) const
    { return pad >= 0 && pad < 16 && cues[(size_t) pad].load() >= 0
             && cueEnds[(size_t) pad].load() == kOpenSlice; }
    SourceSample::Ptr getSource() const;
    double getDetectedBpm() const                          { return detectedBpm.load(); }
    juce::String getDetectedKey() const;
    juce::String getFileName() const;
    bool isPadPlaying (int pad) const;

    // slicing
    void sliceTransients();                                // analyzed transients → pads
    void sliceGrid();                                      // 16 equal divisions
    void sliceBeats (double beatsPerSlice);                // 1, 2, 4 beats per pad (uses source BPM)

    // music-aware auto-slice: transients reconciled to the beat grid (2A)
    void autoSliceMusical();                               // blend of transients + grid + snap
    int  getSliceGridDiv()    const { return sliceGridDiv.load(); }    // 0 Bar,1 Beat,2 1/8,3 1/16
    int  getSliceSensitivity()const { return sliceSensitivity.load(); }// 0 Low,1 Med,2 High
    int  getSliceSnap()       const { return sliceSnap.load(); }       // 0 Loose,1 Med,2 Tight
    void setSliceGridDiv (int v)    { sliceGridDiv.store    (juce::jlimit (0, 3, v)); }
    void setSliceSensitivity (int v){ sliceSensitivity.store(juce::jlimit (0, 2, v)); }
    void setSliceSnap (int v)       { sliceSnap.store       (juce::jlimit (0, 2, v)); }
    double samplesPerBeat() const;                         // 0 if no reliable BPM
    double gridStepSamples() const;                        // current grid division in samples (0 if none)
    int  nearestGridLine (int sourcePos) const;            // nearest grid subdivision (snap target)
    int  snapCursor (int sourcePos) const;                 // live-cursor snap: grid + placed cues (message thread)

    // tempo: detected BPM can be overridden by the user (0 = use detected)
    void setSourceBpmOverride (double bpm);
    double getSourceBpmOverride() const                    { return bpmOverride.load(); }
    double getEffectiveSourceBpm() const
    {
        const double o = bpmOverride.load();
        return o > 1.0 ? o : detectedBpm.load();
    }

    // editor size persistence
    std::atomic<int>  editorW { 900 }, editorH { 640 };

    // kits / export / capture
    bool saveKit (const juce::File& kitFile);
    bool loadKit (const juce::File& kitFile);
    bool exportPad (int pad, const juce::File& wavFile);   // pad slice → WAV (pitch/level/crush applied)
    bool exportKit (const juce::File& directory);          // 16 WAVs + Kit.mid + FlipLog.txt
    void startMidiCapture();
    void stopMidiCapture();
    bool isCapturing() const                               { return capturing.load(); }
    int  capturedEventCount() const;
    bool exportCapturedMidi (const juce::File& midFile);

    // ---- audio-to-MIDI transcription (Phase 2: Basic Pitch, on-demand per pad) ----
    void requestTranscription (int pad);                   // kick off on the worker thread
    bool isTranscribing() const        { return transcribing.load(); }
    bool transcriptionReady() const    { return transcribeReady.load(); }   // a .mid is ready to drag
    bool transcriptionFailed() const   { return transcribeFailed.load(); }
    juce::String getTranscribeStatus() const;
    juce::File   getTranscriptionFile() const;             // latest transcribed .mid (empty if none)
    void setTranscribeQuantize (bool b){ transcribeQuantize.store (b); }
    bool getTranscribeQuantize() const { return transcribeQuantize.load(); }

    std::atomic<int>  selectedPad { 0 };
    std::atomic<bool> analyzing   { false };
    std::atomic<int>  uiDirty     { 0 };

    // incoming-MIDI velocity scales voice level when on (default); off = fixed level
    void setVelToLevel (bool b)     { velToLevel.store (b); }
    bool getVelToLevel() const      { return velToLevel.load(); }

    // ---- stem separation (Stage 2) ----
    void requestStemSeparation();                          // kick off separation on worker thread
    bool isSeparating() const          { return separating.load(); }
    bool isDownloadingModels() const   { return downloadingModels.load(); } // first-run weight fetch
    void setStemUseGpu (bool b)        { stemUseGpu.store (b); }   // try DirectML GPU (auto-falls back to CPU)
    bool hasStems() const;
    float getStemProgress() const      { return stemProgress.load(); }   // 0..1
    juce::String getStemStatus() const;                    // last status / error line
    StemSet::Ptr getStems() const;

    // quality / provider (htdemucs_ft "max quality" is GPU-only; gate it in the UI)
    void setStemMaxQuality (bool b)        { stemMaxQuality = b; }
    // separation quality dial: 0 = FAST (6s, overlap 0.25), 1 = HQ (6s, overlap 0.5),
    // 2 = MAX (htdemucs_ft + 6s hybrid, overlap 0.5; ~5 model passes, CPU-heavy)
    void setStemQuality (int q)            { stemQuality.store (juce::jlimit (0, 2, q)); }
    int  getStemQuality() const            { return stemQuality.load(); }

    // ---- stem mix (global mute/solo; per-pad override comes in 2b-3) ----
    void setStemMuted  (int stem, bool b) { if (stem >= 0 && stem < 6) stemMuted[(size_t) stem].store (b); }
    void setStemSoloed (int stem, bool b) { if (stem >= 0 && stem < 6) stemSoloed[(size_t) stem].store (b); }
    bool isStemMuted   (int stem) const   { return stem >= 0 && stem < 6 && stemMuted[(size_t) stem].load(); }
    bool isStemSoloed  (int stem) const   { return stem >= 0 && stem < 6 && stemSoloed[(size_t) stem].load(); }

    // ---- per-pad stem source (2b-3): mask 0 = FULL (master); else play only the set bits ----
    std::uint8_t getPadStemMask (int pad) const { return (pad >= 0 && pad < 16) ? padStemMask[(size_t) pad].load() : 0; }
    bool isPadFull   (int pad) const            { return getPadStemMask (pad) == 0; }
    bool isPadStemOn (int pad, int stem) const  { return stem >= 0 && stem < 6 && ((getPadStemMask (pad) >> stem) & 1) != 0; }
    void setPadFull  (int pad)                  { if (pad >= 0 && pad < 16) padStemMask[(size_t) pad].store (0); }
    void setPadStemBit (int pad, int stem, bool on)
    {
        if (pad < 0 || pad >= 16 || stem < 0 || stem >= 6) return;
        std::uint8_t m = padStemMask[(size_t) pad].load();
        if (on) m = (std::uint8_t) (m | (1u << stem));
        else    m = (std::uint8_t) (m & ~(1u << stem));
        padStemMask[(size_t) pad].store ((std::uint8_t) (m & 0x3F));
    }

    // live playback feedback for the waveform view
    int  getPadPlayPos (int pad) const                     { return padPlayPos[(size_t) pad].load(); }
    std::atomic<int> lastTriggerPad { -1 };                // most recent pad hit (for view-follow)
    std::atomic<int> lastTriggerCount { 0 };

    // ---- preview transport + live cue assignment ----
    void startPreview (int sourcePos)                      { previewCmd = juce::jmax (0, sourcePos); }
    void stopPreview()                                     { previewCmd = -2; }
    bool isPreviewing() const                              { return previewingA.load(); }
    int  getPreviewPos() const                             { return previewPlayPos.load(); }
    void setAssignCursor (int sourcePos)                   { assignCursor = sourcePos; ++uiDirty; }
    int  getAssignCursor() const                           { return assignCursor.load(); }
    void assignPadCue (int pad, bool snap = false);        // drop pad's cue at the playhead
    bool hasPlayheadForAssign() const;                     // is there a position to assign to?
    std::atomic<bool> snapEnabled { false };               // snap cue placement to nearest transient
    std::atomic<bool> velToLevel  { true };                // MIDI velocity scales voice level (default on)
    std::atomic<int>  auditionPad { -1 };                  // pad to audition on the next audio block
    int nearestTransient (int sourcePos) const;

    // edit history (cue/end state)
    void pushUndo();                                       // call BEFORE a cue/end mutation
    void undo();
    void redo();
    bool canUndo() const                                   { return undoPos > 0; }
    bool canRedo() const                                   { return undoPos < (int) history.size() - 1; }
    std::atomic<int> clearedFlash { -1 };                  // pad index to flash 'cleared', -1 none
    std::atomic<juce::int64> clearedFlashTime { 0 };

    double getHostBpm() const                              { return hostBpm.load(); }
    double currentTargetSpeed() const;                     // effective stretch ratio right now

private:
    // --- worker thread ---
    void run() override;
    void doRenderJob();
    void doPadRenderJobs();
    void doAnalysisJob();
    void doStemJob();
    void doTranscriptionJob();                              // Basic Pitch: slice -> notes -> .mid
    bool writeTranscribedMidi (const std::vector<BasicPitchTranscriber::Note>& notes,
                               const juce::File& midFile);
    void doStemRenderJob();                                 // 2b-1: stretch the 6 stems
    void stretchForRender (const juce::AudioBuffer<float>& in, double srcSR,
                           double speed, double pitch, juce::AudioBuffer<float>& out);
    // rising-edge wake: only fire notify() (a SetEvent syscall) on the false->true
    // transition, so continuous tempo automation can't syscall every audio block —
    // the render thread's wait(250) poll still catches a request set while already true.
    void requestRender() { if (! wantRender.exchange (true)) notify(); }
    void applySlices (const std::vector<int>& s, int sourceLen);

    static BusesProperties makeBuses();
    juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void applyStateTree (const juce::ValueTree& state);

    // --- voices ---
    struct Voice
    {
        bool   active = false;
        int    pad = -1, note = -1, gen = -1;
        double pos = 0.0, rate = 1.0, endPos = 0.0, loopStart = 0.0;
        bool   loop = false, reverse = false;
        float  env = 0.0f, level = 1.0f;
        float  panL = 1.0f, panR = 1.0f;   // equal-power gains, unity at centre
        float  fic1L = 0.0f, fic2L = 0.0f, fic1R = 0.0f, fic2R = 0.0f;   // TPT SVF state per channel
        float  attInc = 0.0f, relDec = 0.0f, padRelDec = 0.0f;
        int    state = 0;        // 0 attack, 1 sustain, 2 release
        bool   gate = false;
        int    srcKind = 0;      // 0 = master render, 1 = per-pad render, 2 = granular
        // SP-1200-style crush
        float  crush = 0.0f, crushQ = 1.0f;   // crushQ = 2^(bits-1), precomputed at trigger
        int    holdN = 1, holdCount = 0;
        float  heldL = 0.0f, heldR = 0.0f;
        // per-stem gain smoothing — makes mute/solo/source flips click-free.
        // The TARGET is read live each block (instant); sg[] ramps toward it.
        float  sg[6] = { 1, 1, 1, 1, 1, 1 };
        bool   sgInit = false;   // snap sg[] to target on the first stem-mixed block
        // ---- per-pad granular (srcKind == 2): a small pool of windowed grains ----
        struct Grain { bool active = false; double readPos = 0.0; double rate = 1.0; int age = 0; int length = 1; };
        static constexpr int kMaxGrains = 12;
        Grain    grains[kMaxGrains];
        double   grainSpawnAcc = 0.0;    // output-sample accumulator for the spawn schedule
        double   grainBaseRate = 1.0;    // sourceSR/fs — pitch-INDEPENDENT grain base rate
        int      grainAge = 0;           // samples since start (frozen non-gate safety cap)
        unsigned grainRng = 0x9e3779b9u; // per-voice xorshift RNG for spray (RT-safe)
    };

    void handleNoteOn (int note, float vel);
    void handleNoteOff (int note);
    void startVoice (int pad, float vel, int extraSemis, int note, bool kbMode);
    void releaseVoices (int pad, int note, bool quick, bool onlyGate);

    // export helper: renders one pad's slice (cue → next cue) with pad
    // pitch / level / crush applied, in the rendered (tempo-synced) domain
    bool renderPadSlice (int pad, juce::AudioBuffer<float>& out, double& outSampleRate);
    juce::String buildFlipLog();

    // --- shared data ---
    mutable juce::SpinLock srcLock, rendLock, infoLock, capLock;
    SourceSample::Ptr   source;
    RenderedSample::Ptr rendered;        // worker writes
    RenderedSample::Ptr active;          // audio-thread copy
    mutable juce::SpinLock padLock;
    std::array<PadRender::Ptr, 16> padRenders;   // worker writes
    std::array<PadRender::Ptr, 16> activePads;   // audio-thread copies
    int padRenderCounter = 0;

    static void offlineStretchSlice (const juce::AudioBuffer<float>& in, int start, int numIn,
                                     double sampleRate, double speed, juce::AudioBuffer<float>& out);

    std::array<std::atomic<int>, 16> cues {};
    std::array<std::atomic<int>, 16> cueEnds {};           // -1 = auto (next cue / end)
    std::array<std::atomic<int>, 16> padPlayPos {};        // SOURCE-domain playhead, -1 = silent

    // preview engine (audio-thread state + UI mirrors)
    std::atomic<int>  previewCmd { -1 };                   // -1 idle, -2 stop, >=0 start at source pos
    std::atomic<bool> previewingA { false };
    std::atomic<int>  previewPlayPos { -1 };               // SOURCE-domain, -1 = silent
    std::atomic<int>  assignCursor { -1 };                 // SOURCE-domain marker
    bool   prevActive = false;
    double prevPos = 0.0;
    float  prevStemSg[6] = { 1, 1, 1, 1, 1, 1 };   // preview stem-gain smoothing (audio thread)

    // undo history of cue/end snapshots
    struct CueSnap { std::array<int,16> cue; std::array<int,16> end; };
    std::vector<CueSnap> history;
    int undoPos = -1;
    CueSnap snapshot() const;
    void applySnap (const CueSnap&);
    std::atomic<double> detectedBpm { 0.0 };
    std::atomic<double> bpmOverride { 0.0 };
    juce::String detectedKey, fileName, filePath;   // guarded by infoLock
    std::vector<int> transientSlices;               // guarded by infoLock
    std::vector<std::pair<int,float>> transientOnsets;   // guarded by infoLock (music-aware slicing)
    std::vector<int> computeBlendedSlices() const;       // reconcile onsets <-> beat grid
    // music-aware auto-slice settings (persisted with the project)
    std::atomic<int>  sliceGridDiv     { 2 };       // 0 Bar, 1 Beat, 2 1/8, 3 1/16
    std::atomic<int>  sliceSensitivity { 1 };       // 0 Low, 1 Med, 2 High
    std::atomic<int>  sliceSnap        { 1 };       // 0 Loose, 1 Med, 2 Tight
    std::atomic<bool> analysisThenSlice { false };  // run music-aware slice when analysis completes

    std::atomic<double> hostBpm { 0.0 };
    std::atomic<bool>   wantRender { false }, wantAnalysis { false }, analysisKeepCues { false };

    // ---- stem separation state ----
    StemSeparator       stemEngine;                        // ONNX-free until initialise()
    mutable juce::SpinLock stemLock;
    StemSet::Ptr        stemSet;                           // worker writes, UI reads
    std::atomic<bool>   wantStems { false };
    std::atomic<bool>   separating { false };
    std::atomic<bool>   downloadingModels { false };  // true during first-run weight download
    std::atomic<float>  stemProgress { 0.0f };
    int                 stemGeneration = 0;
    // CPU is the stable default; GPU opt-in. Deliberately NOT persisted (session/dev-only):
    // the GPU toggle UI is hard-disabled and runtime forces this false on any DirectML failure.
    std::atomic<bool>   stemUseGpu { false };
    std::atomic<bool>   stemMaxQuality { false };  // htdemucs_ft hybrid (GPU-only); else htdemucs_6s
    std::atomic<int>    stemQuality { 1 };         // 0 FAST(6s/.25), 1 HQ(6s/.5), 2 MAX(hybrid/.5)
    int                 stemEngineMode = -1;   // -1 none, 0 CPU, 1 GPU (sessions currently loaded)
    juce::String        stemStatus;            // guarded by infoLock

    // ---- audio-to-MIDI transcription (Basic Pitch) ----
    BasicPitchTranscriber transcriber;
    std::atomic<bool>   wantTranscription { false };   // worker signal
    std::atomic<bool>   transcribing      { false };   // busy
    std::atomic<bool>   transcribeReady   { false };   // a .mid is ready to drag
    std::atomic<bool>   transcribeFailed  { false };
    std::atomic<bool>   transcribeQuantize { true };   // snap notes to the beat grid (default on)
    std::atomic<int>    transcribePad     { -1 };
    juce::File          transcribeMidi;                // latest output, guarded by infoLock
    juce::String        transcribeStatus;              // guarded by infoLock

    // rendered (tempo-stretched) stems, parallel to `rendered`
    mutable juce::SpinLock stemRendLock;
    RenderedStems::Ptr  renderedStems;                     // worker writes
    RenderedStems::Ptr  activeStems;                       // audio-thread copy
    std::atomic<bool>   wantStemRender { false };
    int                 stemRenderGeneration = 0;

    // global stem mute/solo (audio thread reads, UI/worker writes)
    std::array<std::atomic<bool>, 6> stemMuted {};
    std::array<std::atomic<bool>, 6> stemSoloed {};
    std::array<std::atomic<std::uint8_t>, 16> padStemMask {};   // 0 = FULL, else bitmask of stems
    std::atomic<double> lastRenderSpeed { 1.0 }, lastRenderPitch { 0.0 };
    int renderGeneration = 0;

    // GUI -> audio-thread trigger queue
    struct UiEvent { int pad = 0; bool on = false; };
    juce::AbstractFifo uiFifo { 64 };
    std::array<UiEvent, 64> uiEvents;

    // MIDI performance capture (pad hits → draggable .mid)
    struct CapEvent { double sec = 0.0; int pad = 0; bool on = false; float vel = 1.0f; };
    std::vector<CapEvent> capEvents;                 // guarded by capLock
    std::atomic<bool>   capturing { false };
    std::atomic<juce::int64> capSamples { 0 };
    void logCap (int pad, bool on, float vel);

    std::array<Voice, 32> voices;

    // cached raw parameter pointers
    std::atomic<float>* pMasterPitch = nullptr;
    std::atomic<float>* pTempoMode = nullptr;   // 0 off, 1 sync, 2 custom
    std::atomic<float>* pCustomBpm = nullptr;
    std::atomic<float>* pKb = nullptr;
    std::array<std::atomic<float>*, 16> pPitch {}, pLevel {}, pAtt {}, pRel {}, pMode {}, pSlice {}, pCrush {}, pSpeed {}, pPan {}, pChoke {};
    std::array<std::atomic<float>*, 16> pCutoff {}, pReso {}, pFType {}, pLoop {}, pReverse {}, pBleed {};
    // per-pad granular (srcKind 2)
    std::array<std::atomic<float>*, 16> pGrainOn {}, pGrainSize {}, pGrainDens {}, pGrainPos {},
                                        pGrainFreeze {}, pGrainSpray {}, pGrainPitch {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GentSamplerAudioProcessor)
};
