#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ModelDownloader.h"
#ifndef GENT_NO_STEMS
 #include "StemSeparator.h"
#endif
#include "signalsmith-stretch.h"
#include <limits>

// ----------------------------------------------------------------------------
//  Hann window lookup (granular grains) — built once at load, read lock-free on
//  the audio thread. ph in [0,1) -> 0..1..0 raised-cosine.
// ----------------------------------------------------------------------------
namespace {
struct HannTable
{
    static constexpr int N = 1024;
    float t[N + 1];
    HannTable()
    {
        for (int i = 0; i <= N; ++i)
            t[i] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) N));
    }
};
const HannTable gHann;

inline float hannWin (float ph) noexcept
{
    const float x = juce::jlimit (0.0f, 1.0f, ph) * (float) HannTable::N;
    const int   i = (int) x;
    const float f = x - (float) i;
    return gHann.t[i] + f * (gHann.t[i + 1] - gHann.t[i]);
}
} // namespace

// ============================================================================
//  Construction / parameters / buses
// ============================================================================

juce::AudioProcessor::BusesProperties GentSamplerAudioProcessor::makeBuses()
{
    auto b = BusesProperties().withOutput ("Main", juce::AudioChannelSet::stereo(), true);
    for (int i = 0; i < 16; ++i)
        b = b.withOutput ("Pad " + juce::String (i + 1), juce::AudioChannelSet::stereo(), false);
    return b;
}

bool GentSamplerAudioProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    if (l.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    for (int i = 1; i < l.outputBuses.size(); ++i)
    {
        const auto set = l.getChannelSet (false, i);
        if (! set.isDisabled() && set != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

GentSamplerAudioProcessor::GentSamplerAudioProcessor()
    : juce::AudioProcessor (makeBuses()),
      juce::Thread ("GentSamplerWorker"),
      apvts (*this, nullptr, "PARAMS", makeLayout())
{
    pMasterPitch = apvts.getRawParameterValue ("masterPitch");
    pTempoMode   = apvts.getRawParameterValue ("tempoMode");
    pCustomBpm   = apvts.getRawParameterValue ("customBpm");
    pKb          = apvts.getRawParameterValue ("kbMode");

    for (int i = 0; i < 16; ++i)
    {
        pPitch[(size_t) i] = apvts.getRawParameterValue (pid (i, "pitch"));
        pLevel[(size_t) i] = apvts.getRawParameterValue (pid (i, "level"));
        pAtt[(size_t) i]   = apvts.getRawParameterValue (pid (i, "att"));
        pRel[(size_t) i]   = apvts.getRawParameterValue (pid (i, "rel"));
        pMode[(size_t) i]  = apvts.getRawParameterValue (pid (i, "mode"));
        pSlice[(size_t) i] = apvts.getRawParameterValue (pid (i, "slice"));
        pSpeed[(size_t) i] = apvts.getRawParameterValue (pid (i, "speed"));
        pCrush[(size_t) i] = apvts.getRawParameterValue (pid (i, "crush"));
        pPan[(size_t) i]   = apvts.getRawParameterValue (pid (i, "pan"));
        pChoke[(size_t) i] = apvts.getRawParameterValue (pid (i, "choke"));
        pCutoff[(size_t) i] = apvts.getRawParameterValue (pid (i, "cutoff"));
        pReso[(size_t) i]  = apvts.getRawParameterValue (pid (i, "reso"));
        pFType[(size_t) i] = apvts.getRawParameterValue (pid (i, "ftype"));
        pLoop[(size_t) i]  = apvts.getRawParameterValue (pid (i, "loop"));
        pReverse[(size_t) i] = apvts.getRawParameterValue (pid (i, "reverse"));
        pBleed[(size_t) i] = apvts.getRawParameterValue (pid (i, "bleed"));
        pGrainOn[(size_t) i]     = apvts.getRawParameterValue (pid (i, "grainOn"));
        pGrainSize[(size_t) i]   = apvts.getRawParameterValue (pid (i, "grainSize"));
        pGrainDens[(size_t) i]   = apvts.getRawParameterValue (pid (i, "grainDens"));
        pGrainPos[(size_t) i]    = apvts.getRawParameterValue (pid (i, "grainPos"));
        pGrainFreeze[(size_t) i] = apvts.getRawParameterValue (pid (i, "grainFreeze"));
        pGrainSpray[(size_t) i]  = apvts.getRawParameterValue (pid (i, "grainSpray"));
        pGrainPitch[(size_t) i]  = apvts.getRawParameterValue (pid (i, "grainPitch"));
        cues[(size_t) i] = 0;
        cueEnds[(size_t) i] = -1;
        padPlayPos[(size_t) i] = -1;
        pendingAssignPos[i] = -1;   // WAVE1_SPEC.md F1: no pending MIDI tap-assign for this pad
    }

    capEvents.reserve (4096);
    startThread();
}

GentSamplerAudioProcessor::~GentSamplerAudioProcessor()
{
    // WAVE1 reviewer nit (applied by the lead): neutralize any pending
    // tap-to-cue AsyncUpdater dispatch before teardown — defense-in-depth for
    // future destruction-order changes (JUCE's ~AsyncUpdater asserts on a
    // pending update).
    cancelPendingUpdate();
    // DATA_INTEGRITY_SPEC.md ADDENDUM T: with the FLAC encodes/separation now
    // abortable (chunked writes + StemSeparator::shouldAbort, all polling
    // threadShouldExit()), the real exit latency is chunk-sized (well under
    // 1 s). The raised cap is pure headroom, not a wait -- it exists so
    // JUCE's force-kill-after-timeout (the actual heap-corruption source: a
    // worker torn down mid-encode/mid-inference) is effectively unreachable.
    stopThread (10000);
}

juce::AudioProcessorValueTreeState::ParameterLayout GentSamplerAudioProcessor::makeLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> ps;

    ps.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "masterPitch", 1 }, "Master Pitch",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 1.0f), 0.0f));

    ps.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "tempoMode", 1 }, "Tempo Mode",
        juce::StringArray { "Off", "Sync", "Custom" }, 1));

    ps.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "customBpm", 1 }, "Custom BPM",
        juce::NormalisableRange<float> (40.0f, 220.0f, 0.1f), 90.0f));

    ps.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "kbMode", 1 }, "Keyboard Mode", false));

    for (int i = 0; i < 16; ++i)
    {
        const auto nm = "Pad " + juce::String (i + 1) + " ";

        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "pitch"), 1 }, nm + "Pitch",
            juce::NormalisableRange<float> (-24.0f, 24.0f, 1.0f), 0.0f));

        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "level"), 1 }, nm + "Level",
            juce::NormalisableRange<float> (0.0f, 1.5f, 0.01f), 1.0f));

        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "att"), 1 }, nm + "Attack",
            juce::NormalisableRange<float> (0.0f, 500.0f, 1.0f, 0.4f), 1.0f));

        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "rel"), 1 }, nm + "Release",
            juce::NormalisableRange<float> (1.0f, 2000.0f, 1.0f, 0.4f), 80.0f));

        ps.push_back (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { pid (i, "mode"), 1 }, nm + "Play Mode",
            juce::StringArray { "Gate", "One-Shot", "Latch" }, 0));

        ps.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { pid (i, "slice"), 1 }, nm + "Stop At Next Cue", true));

        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "crush"), 1 }, nm + "Crush",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.0f));

        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "bleed"), 1 }, nm + "Bleed",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.0f));

        // ---- per-pad granular ----
        ps.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { pid (i, "grainOn"), 1 }, nm + "Grain On", false));
        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "grainSize"), 1 }, nm + "Grain Size",
            juce::NormalisableRange<float> (10.0f, 500.0f, 1.0f), 80.0f));
        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "grainDens"), 1 }, nm + "Grain Density",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.4f));
        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "grainPos"), 1 }, nm + "Grain Position",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
        ps.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { pid (i, "grainFreeze"), 1 }, nm + "Grain Freeze", false));
        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "grainSpray"), 1 }, nm + "Grain Spray",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.0f));
        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "grainPitch"), 1 }, nm + "Grain Pitch",
            juce::NormalisableRange<float> (-24.0f, 24.0f, 1.0f), 0.0f));

        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "speed"), 1 }, nm + "Speed",
            juce::NormalisableRange<float> (0.5f, 2.0f, 0.01f), 1.0f));

        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "pan"), 1 }, nm + "Pan",
            juce::NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f));   // -1 L .. +1 R

        ps.push_back (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { pid (i, "choke"), 1 }, nm + "Choke Group",
            juce::StringArray { "Off", "1", "2", "3", "4", "5", "6", "7", "8" }, 0));

        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "cutoff"), 1 }, nm + "Filter Cutoff",
            juce::NormalisableRange<float> (20.0f, 20000.0f, 1.0f, 0.25f), 20000.0f));

        ps.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { pid (i, "reso"), 1 }, nm + "Filter Reso",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.1f));

        ps.push_back (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { pid (i, "ftype"), 1 }, nm + "Filter Type",
            juce::StringArray { "Off", "LP", "HP", "BP" }, 0));

        ps.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { pid (i, "loop"), 1 }, nm + "Loop", false));

        ps.push_back (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { pid (i, "reverse"), 1 }, nm + "Reverse", false));
    }

    return { ps.begin(), ps.end() };
}

void GentSamplerAudioProcessor::prepareToPlay (double, int)
{
    for (auto& v : voices)
        v.active = false;
}

// ============================================================================
//  File loading / editor API
// ============================================================================

bool GentSamplerAudioProcessor::loadFile (const juce::File& f, bool runAnalysis)
{
    // KIT_SPEC.md PART B (review fix): a v2 kit load sets filePath to the
    // .gentkit itself, so any caller of this synchronous, user-initiated
    // function (drag-drop, file-browser) may hand it that ZIP. The WAV/FLAC
    // reader below can't open a ZIP -> the load would silently adopt an EMPTY
    // source (the Pad12-ghost family). Route ZIP-sniffed files to the
    // audio-only v2 loader: audio (+stems) adopted, but the KIT's saved state
    // is NOT applied here.
    // DATA_INTEGRITY_SPEC.md Change 2: a host-project restore no longer calls
    // this function at all — applyStateTree validates the stored path and
    // hands it to doRestoreLoadJob() (worker), which does its own PK sniff and
    // calls loadKitV2Audio()/decodes directly, off the message thread.
    {
        juce::FileInputStream sniff (f);
        char magic[2] = { 0, 0 };
        if (sniff.openedOk() && sniff.read (magic, 2) == 2 && magic[0] == 'P' && magic[1] == 'K')
            return loadKitV2Audio (f, runAnalysis);
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr)
        return false;

    const auto maxLen = (juce::int64) (reader->sampleRate * 600.0);   // cap at 10 minutes
    const int len = (int) juce::jmin (reader->lengthInSamples, maxLen);
    juce::AudioBuffer<float> buf ((int) reader->numChannels, len);
    reader->read (&buf, 0, len, 0, true, true);

    return adoptSourceBuffer (std::move (buf), reader->sampleRate, f.getFullPathName(), runAnalysis);
}

// KIT_SPEC.md PART B: pure extraction of loadFile()'s post-decode body (see
// this function's declaration comment in the header for the callers). Takes
// an already-decoded buffer so the v2 kit-load path (FLAC decoded from a ZIP
// entry, no on-disk source file) can reuse it verbatim; displayPath is what
// getFileName()/filePath() will show afterwards (the source file's path for
// loadFile(), the kit file's own path for a v2 kit load).
bool GentSamplerAudioProcessor::adoptSourceBuffer (juce::AudioBuffer<float>&& buf, double sampleRate,
                                                    const juce::String& displayPath, bool runAnalysis,
                                                    bool keepCues)
{
    // PREPACKAGE_AUDIT.md #14: serialize the whole body — see the lock-order
    // contract at the adoptLock member declaration (PluginProcessor.h).
    const juce::ScopedLock sl (adoptLock);

    auto* s = new SourceSample();
    s->buffer = std::move (buf);
    s->sampleRate = sampleRate;
    const int len = s->buffer.getNumSamples();

    {
        const juce::SpinLock::ScopedLockType sl (srcLock);
        source = s;
    }
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        fileName = juce::File (displayPath).getFileName();
        filePath = displayPath;
        transientOnsets.clear();   // always: stale onsets must never drive a new file's auto-slice
        featureFrames.clear();     // always: stale features must never drive a new file's classify/KIT
        featureFrameRate = 0.0;
        if (runAnalysis) { detectedKey.clear(); transientSlices.clear(); }
    }

    // D5 / docs/STEM_VIEW_MODEL.md DECISION-6: a genuinely NEW direct user load
    // (drag-drop PluginEditor.cpp:1290, file-browser PluginEditor.cpp:328 -- both
    // call loadFile(f) with the default runAnalysis=true) must drop the OLD
    // file's stemSet so hasStems() correctly reports false for the new source
    // (otherwise the STEMS view would keep showing yesterday's song's stems
    // under today's song's flags -- SS1's finding, DECISION-6's gap). The
    // The restore path (DATA_INTEGRITY_SPEC.md Change 2: doRestoreLoadJob, the
    // worker-thread decode applyStateTree hands off to) calls this with
    // runAnalysis=false and must NOT clear -- applyStateTree unconditionally
    // restores padStemMask from the "src"+i keys BEFORE the decode lands,
    // which depends on today's stemSet-survives-a-load behavior; runAnalysis
    // is exactly the existing boolean that separates the two cases (true only
    // at the two direct-load call sites, false only at the sole restore call
    // site -- confirmed by grep, no fourth caller).
    // Same stemLock discipline as the separation-completion write (cpp ~1099-
    // 1101): mute/solo (stemMuted/stemSoloed) and padStemMask are NOT touched --
    // they carry forward per DECISION-6 Option 2, not reset to defaults.
    if (runAnalysis)
    {
        const juce::SpinLock::ScopedLockType sl (stemLock);
        stemSet = nullptr;
    }
    if (runAnalysis)
    {
        // Joe-authorized extension of DECISION-6 (2026-07-03, post-R3): the
        // narration string must not survive a genuinely new load either — a
        // fresh file's STEMS placeholder was showing the PREVIOUS file's
        // stale "separation failed"/progress text. Same runAnalysis boundary,
        // stemStatus's own lock (infoLock, matching setStatus's discipline).
        // PREPACKAGE_AUDIT.md #9 (WAVE2_SPEC.md, part 2 of 3): a genuinely new
        // direct user load must also invalidate any pending stem-CACHE load —
        // otherwise a restore's still-in-flight wantStemCacheLoad request can
        // attach a completely unrelated project's cached stems to this brand
        // new source once the worker gets to it. Same infoLock as above.
        const juce::SpinLock::ScopedLockType sl (infoLock);
        stemStatus.clear();
        stemCacheKey.clear();
        wantStemCacheLoad = false;
    }

    // DATA_INTEGRITY_SPEC.md Change 2: the async restore-load path (worker,
    // doRestoreLoadJob) decodes AFTER applyStateTree has already written the
    // restored cues — keepCues=true skips this default-cue loop entirely so
    // the restored cues survive untouched. Every other caller passes
    // keepCues=false (the default) and this loop runs exactly as before.
    if (! keepCues)
    {
        for (int i = 0; i < 16; ++i)
        {
            cues[(size_t) i] = (int) ((juce::int64) len * i / 16);
            cueEnds[(size_t) i] = -1;
        }
    }

    if (runAnalysis)
    {
        detectedBpm = 0.0;
        analysisKeepCues = false;
        wantAnalysis = true;
    }

    wantRender = true;
    notify();
    ++uiDirty;
    return true;
}

SourceSample::Ptr GentSamplerAudioProcessor::getSource() const
{
    const juce::SpinLock::ScopedLockType sl (srcLock);
    return source;
}

juce::String GentSamplerAudioProcessor::getDetectedKey() const
{
    const juce::SpinLock::ScopedLockType sl (infoLock);
    return detectedKey;
}

juce::String GentSamplerAudioProcessor::getFileName() const
{
    const juce::SpinLock::ScopedLockType sl (infoLock);
    return fileName;
}

int GentSamplerAudioProcessor::nearestTransient (int sourcePos) const
{
    std::vector<int> ts;
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        ts = transientSlices;
    }
    auto src = getSource();
    const double sr = src != nullptr ? src->sampleRate : 44100.0;
    return gent::nearestTransientIn (ts, sourcePos, sr);
}

GentSamplerAudioProcessor::CueSnap GentSamplerAudioProcessor::snapshot() const
{
    CueSnap s;
    for (int i = 0; i < 16; ++i)
    {
        s.cue[(size_t) i] = cues[(size_t) i].load();
        s.end[(size_t) i] = cueEnds[(size_t) i].load();
        s.mask[(size_t) i] = padStemMask[(size_t) i].load();
        // 0.3: 7 grain params per pad, cheapest correct message-thread read —
        // the cached raw-atomic pGrain* pointers (same idiom as grainOnFor/
        // getGrainPosFor above), not a fresh apvts parameter lookup.
        auto& g = s.grain[(size_t) i];
        g[0] = pGrainOn[(size_t) i]->load();
        g[1] = pGrainSize[(size_t) i]->load();
        g[2] = pGrainDens[(size_t) i]->load();
        g[3] = pGrainPos[(size_t) i]->load();
        g[4] = pGrainFreeze[(size_t) i]->load();
        g[5] = pGrainSpray[(size_t) i]->load();
        g[6] = pGrainPitch[(size_t) i]->load();
    }
    return s;
}

void GentSamplerAudioProcessor::applySnap (const CueSnap& s)
{
    // AMENDMENT 0.3-A: guard against re-entrant pushUndo() while restoring
    // grainOn/grainFreeze below (see restoringSnap's declaration comment) —
    // scoped for the duration of this call only, message thread only. RAII
    // (R0 nit): exception-safe even though nothing on this path throws today.
    const juce::ScopedValueSetter<bool> svs (restoringSnap, true, false);
    for (int i = 0; i < 16; ++i)
    {
        cues[(size_t) i] = s.cue[(size_t) i];
        cueEnds[(size_t) i] = s.end[(size_t) i];

        // write-if-changed: skip pads whose mask already matches, so a single
        // undo/redo doesn't fire 16 needless atomic stores.
        if (padStemMask[(size_t) i].load() != s.mask[(size_t) i])
            padStemMask[(size_t) i].store (s.mask[(size_t) i]);

        // 0.3 / D-0.3c: restore all 7 grain params via the existing message-
        // thread apvts.getParameterAsValue() idiom (h:259 precedent), write-
        // if-changed to avoid a storm of listener notifications (16 pads * 7
        // params = 112 potential writes per undo step). No beginChangeGesture/
        // endChangeGesture bracketing (decided, D-0.3c): undo is a plain UI
        // write like any other, not a host-automation gesture. Consequence,
        // accepted: if the host is actively automating a grain param, its next
        // automation pass will overwrite what undo just wrote here — identical
        // to how any other UI write behaves under host automation in every
        // DAW; undo does not fight it.
        const auto& g = s.grain[(size_t) i];
        static const char* const kGrainSuffix[7] =
            { "grainOn", "grainSize", "grainDens", "grainPos", "grainFreeze", "grainSpray", "grainPitch" };
        std::atomic<float>* const kGrainPtr[7] =
        {
            pGrainOn[(size_t) i], pGrainSize[(size_t) i], pGrainDens[(size_t) i], pGrainPos[(size_t) i],
            pGrainFreeze[(size_t) i], pGrainSpray[(size_t) i], pGrainPitch[(size_t) i]
        };
        for (int k = 0; k < 7; ++k)
            if (kGrainPtr[k]->load() != g[(size_t) k])
                apvts.getParameterAsValue (pid (i, kGrainSuffix[k])) = (double) g[(size_t) k];
    }
    ++uiDirty;
}

// AMENDMENT 0.3-A: history[i] holds "the state after i tracked edits" (i.e.
// history[0] is the baseline before any edit; history[k] is the live state
// right after the k-th edit). undoPos indexes the slot the CURRENT live state
// belongs to. Between a pushUndo() call and its edit's mutation, the top slot
// is momentarily a stale copy of the previous slot — that's fine, because the
// *next* pushUndo() or undo() call always corrects ("fixes up") the slot at
// the OLD undoPos with a fresh snapshot() before moving on, capturing the true
// post-edit state right before it would otherwise be lost. The pre-amendment
// bug: the empty-history seed pushed TWO entries but every later edit only
// ever pushed ONE new entry with no fix-up of the previous slot, so the
// previous edit's true result was never recorded anywhere — undo() then
// landed one slot early, silently skipping the most recent edit.
//
// Slot diagram for edit A -> edit B -> undo -> undo -> redo -> redo
// (S0 = initial, SA = state after edit A, SB = state after edit B):
//
//   pushUndo() [before A]:  history=[S0, S0]              undoPos=1   (seed + placeholder, live=S0)
//   edit A mutates:         history=[S0, S0]              undoPos=1   (live=SA, slot 1 stale)
//   pushUndo() [before B]:  fix-up slot1 -> SA
//                           history=[S0, SA]  then push    undoPos=2
//                           history=[S0, SA, SA]                       (live=SA, slot 2 stale)
//   edit B mutates:         history=[S0, SA, SA]           undoPos=2   (live=SB, slot 2 stale)
//   undo() #1:              fix-up slot2 -> SB
//                           history=[S0, SA, SB]            undoPos=1   apply history[1]=SA   (live=SA)  correct: reverted ONLY edit B
//   undo() #2:              fix-up slot1 -> SA (no-op, already SA)
//                           history=[S0, SA, SB]            undoPos=0   apply history[0]=S0   (live=S0)  correct: reverted edit A too
//   redo() #1:              undoPos=1   apply history[1]=SA (live=SA)  correct: re-applies edit A
//   redo() #2:              undoPos=2   apply history[2]=SB (live=SB)  correct: re-applies edit B
void GentSamplerAudioProcessor::pushUndo()
{
    // drop any redo branch
    if (undoPos < (int) history.size() - 1)
        history.erase (history.begin() + undoPos + 1, history.end());
    if (history.empty())
        history.push_back (snapshot());            // idx0 = baseline, before the very first edit
    else
        history[(size_t) undoPos] = snapshot();     // fix up the current top: true state after the PREVIOUS edit
    history.push_back (snapshot());                 // new top = placeholder for "after this edit"; corrected lazily
    // (R0 note: an armed-but-abandoned gesture — push with no following edit —
    // leaves a duplicate slot, costing one extra undo click. Not a bug: no
    // state is lost/skipped, and the next pushUndo's fix-up self-heals it.)
                                                      // by the next pushUndo()/undo() call, exactly like the line above
    if (history.size() > 64)
        history.erase (history.begin());
    undoPos = (int) history.size() - 1;
}

void GentSamplerAudioProcessor::undo()
{
    if (undoPos <= 0) return;
    // fix up the current top slot with the TRUE live state (unchanged by
    // AMENDMENT 0.3-A — this line was already correct; pushUndo() was the
    // only bug) so redo can return to it, then step back exactly one edit.
    history[(size_t) undoPos] = snapshot();
    --undoPos;
    applySnap (history[(size_t) undoPos]);
}

void GentSamplerAudioProcessor::redo()
{
    if (undoPos >= (int) history.size() - 1) return;
    ++undoPos;
    applySnap (history[(size_t) undoPos]);
}

bool GentSamplerAudioProcessor::hasPlayheadForAssign() const
{
    return (previewingA.load() && previewPlayPos.load() >= 0) || assignCursor.load() >= 0;
}

void GentSamplerAudioProcessor::assignPadCue (int pad, bool snap)
{
    if (pad < 0 || pad >= 16)
        return;
    int pos = (previewingA.load() && previewPlayPos.load() >= 0) ? previewPlayPos.load() : -1;
    if (pos < 0)
        pos = assignCursor.load();
    if (pos < 0)
        return;
    pushUndo();
    setCue (pad, pos, snap);
    // Bare POINT cue: a tapped unassigned pad gets a cue ONLY — no window, no
    // end. Mark the end OPEN (kOpenSlice) so neither effectiveCueEnd nor the
    // audio path ever derive a boundary for it from the global cue scan; it
    // plays from the cue under the pad's GATE default (held = sound, release =
    // stop) until the user drags a real end in the slice-detail strip. Only this
    // pad's cue/end are written; nothing here reads or touches another pad.
    cueEnds[(size_t) pad] = kOpenSlice;
    selectedPad = pad;
    lastTriggerPad = pad;
    ++lastTriggerCount;
    ++uiDirty;
    if (! previewingA.load())
        auditionPad = pad;
}

void GentSamplerAudioProcessor::clearCue (int pad)
{
    if (pad < 0 || pad >= 16)
        return;
    cues[(size_t) pad] = -1;          // unassigned: no region, no sound
    cueEnds[(size_t) pad] = -1;
    padStemMask[(size_t) pad] = 0;    // back to FULL
    ++uiDirty;
}

void GentSamplerAudioProcessor::setCue (int pad, int samplePos, bool snap)
{
    if (pad < 0 || pad >= 16)
        return;
    // snap reads the source/transients (locks + alloc) so it is ONLY done by
    // message-thread editor callers (snap=true); the audio path passes snap=false.
    if (snap && snapEnabled.load())
        samplePos = (gridStepSamples() > 0.0) ? nearestGridLine (samplePos)
                                              : nearestTransient (samplePos);
    const auto r = gent::applyCueEdit (samplePos, cueEnds[(size_t) pad].load());
    cues[(size_t) pad] = r.cue;
    cueEnds[(size_t) pad] = r.end;                        // unchanged unless start pushed past end
}

void GentSamplerAudioProcessor::setCueEnd (int pad, int samplePos)
{
    if (pad < 0 || pad >= 16)
        return;
    cueEnds[(size_t) pad] = gent::resolveCueEndEdit (samplePos, cues[(size_t) pad].load());
}

int GentSamplerAudioProcessor::getEffectiveCueEnd (int pad) const
{
    auto src = getSource();
    const int len = src != nullptr ? src->buffer.getNumSamples() : 0;
    if (pad < 0 || pad >= 16 || len < 2)
        return juce::jmax (0, len - 1);

    const int cueSrc = cues[(size_t) pad].load();
    const int e = cueEnds[(size_t) pad].load();
    const bool sliceMode = pSlice[(size_t) pad]->load() > 0.5f;
    std::array<int, 16> allCues {};
    for (int i = 0; i < 16; ++i)
        allCues[(size_t) i] = cues[(size_t) i].load();
    return gent::effectiveCueEnd (cueSrc, e, len, sliceMode, allCues);
}

void GentSamplerAudioProcessor::applySlices (const std::vector<int>& s, int sourceLen)
{
    for (int i = 0; i < 16; ++i)
    {
        cues[(size_t) i] = i < (int) s.size() ? s[(size_t) i]
                                              : (int) ((juce::int64) sourceLen * i / 16);
        cueEnds[(size_t) i] = -1;
    }
    ++uiDirty;
}

void GentSamplerAudioProcessor::sliceTransients()
{
    auto src = getSource();
    if (src == nullptr) return;
    std::vector<int> s;
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        s = transientSlices;
    }
    if (s.empty())
    {
        analysisKeepCues = false;
        wantAnalysis = true;
        notify();
        return;
    }
    applySlices (s, src->buffer.getNumSamples());
}

void GentSamplerAudioProcessor::sliceGrid()
{
    auto src = getSource();
    if (src == nullptr) return;
    applySlices ({}, src->buffer.getNumSamples());
}

void GentSamplerAudioProcessor::sliceBeats (double beatsPerSlice)
{
    auto src = getSource();
    const double bpm = getEffectiveSourceBpm();
    if (src == nullptr || bpm <= 1.0)
        return;

    const int len = src->buffer.getNumSamples();
    const double samplesPerBeat = (60.0 / bpm) * src->sampleRate;
    std::vector<int> s;
    for (int i = 0; i < 16; ++i)
        s.push_back (juce::jmin (len - 1, (int) (i * beatsPerSlice * samplesPerBeat)));
    applySlices (s, len);
}

void GentSamplerAudioProcessor::sliceSections (int bars)
{
    auto src = getSource();
    const double bpm = getEffectiveSourceBpm();
    if (src == nullptr || bpm <= 1.0)
        return;

    const int len = src->buffer.getNumSamples();
    const double spb = (60.0 / bpm) * src->sampleRate;
    const auto result = gent::barSectionSlices (len, spb * 4.0, bars);

    if (result.sectionCount > 16)
        DBG ("SECTIONS: " << result.sectionCount << " sections at " << bars << " bars, first 16 laid");

    std::vector<int> s (16);
    for (int i = 0; i < 16; ++i)
        s[(size_t) i] = result.cue[(size_t) i];
    applySlices (s, len);
}

// KIT_SPEC.md PART A: every hit gets its own pad, in time order. Message
// thread; caller pushes undo. sliceTransients() precedent for the
// deferred-until-analyzed path (onsets don't exist yet).
void GentSamplerAudioProcessor::sliceKit (int sensitivity)
{
    auto src = getSource();
    if (src == nullptr) return;

    std::vector<std::pair<int, float>> onsets;
    getOnsetPeaks (onsets);

    if (onsets.empty())
    {
        kitSensPending = sensitivity;
        kitPending = true;
        analysisKeepCues = false;
        wantAnalysis = true;
        notify();
        return;
    }

    const auto hits = gent::kitHits (onsets, src->sampleRate, sensitivity);

    if ((int) hits.size() > 16)
        DBG ("KIT: " << hits.size() << " hits, first 16 laid");

    std::vector<int> s (16, -1);
    for (int i = 0; i < 16 && i < (int) hits.size(); ++i)
        s[(size_t) i] = hits[(size_t) i];

    applySlices (s, src->buffer.getNumSamples());
}

// ---- music-aware auto-slice (2A): transients reconciled to the beat grid --------
double GentSamplerAudioProcessor::samplesPerBeat() const
{
    auto src = getSource();
    const double bpm = getEffectiveSourceBpm();
    if (src == nullptr || bpm <= 1.0) return 0.0;
    return (60.0 / bpm) * src->sampleRate;
}

double GentSamplerAudioProcessor::gridStepSamples() const
{
    const double spb = samplesPerBeat();
    if (spb <= 0.0) return 0.0;
    switch (sliceGridDiv.load())
    {
        case 0:  return spb * 4.0;   // bar (assume 4/4)
        case 1:  return spb;         // beat
        case 2:  return spb * 0.5;   // 1/8
        default: return spb * 0.25;  // 1/16
    }
}

int GentSamplerAudioProcessor::nearestGridLine (int sourcePos) const
{
    const double step = gridStepSamples();
    if (step <= 0.0) return sourcePos;
    const double k = std::round ((double) sourcePos / step);
    return juce::jmax (0, (int) std::llround (k * step));
}

// live-cursor snap (message thread only — reads source/transients): nearest of the
// beat grid + every placed cue; with no tempo, the nearest transient/cue within 50 ms.
int GentSamplerAudioProcessor::snapCursor (int pos) const
{
    if (! snapEnabled.load()) return pos;
    const double step = gridStepSamples();
    const bool hasGrid = step > 0.0;
    const int gridCand = hasGrid ? nearestGridLine (pos) : 0;   // unused unless hasGrid
    std::array<int, 16> allCues {};
    for (int i = 0; i < 16; ++i) allCues[(size_t) i] = cues[(size_t) i].load();
    const int transientCand = hasGrid ? 0 : nearestTransient (pos);   // unused unless !hasGrid
    auto src = getSource();
    const double sr = src != nullptr ? src->sampleRate : 44100.0;
    return gent::selectSnapCursor (pos, hasGrid, gridCand, allCues, transientCand, sr);
}

std::vector<int> GentSamplerAudioProcessor::getOnsetPositions() const
{
    std::vector<std::pair<int, float>> on;
    { const juce::SpinLock::ScopedLockType sl (infoLock); on = transientOnsets; }
    std::vector<int> out;
    out.reserve (on.size());
    for (auto& o : on) out.push_back (o.first);
    return out;
}

// KIT_SPEC.md PART A: strengths-preserving copy accessor, mirroring
// getOnsetPositions() above.
void GentSamplerAudioProcessor::getOnsetPeaks (std::vector<std::pair<int, float>>& out) const
{
    const juce::SpinLock::ScopedLockType sl (infoLock);
    out = transientOnsets;
}

// P1 (PHASE3_SPEC.md PART 1): copy-under-lock accessor for the cached
// per-frame analysis features, mirroring getOnsetPositions() above.
void GentSamplerAudioProcessor::getFeatureFrames (std::vector<gent::FrameFeatures>& out, double& rate) const
{
    const juce::SpinLock::ScopedLockType sl (infoLock);
    out = featureFrames;
    rate = featureFrameRate;
}

std::vector<int> GentSamplerAudioProcessor::computeBlendedSlices() const
{
    auto src = getSource();
    if (src == nullptr) return {};
    const int    len = src->buffer.getNumSamples();
    const double sr  = src->sampleRate;

    std::vector<std::pair<int,float>> on;
    { const juce::SpinLock::ScopedLockType sl (infoLock); on = transientOnsets; }
    if (on.empty()) return {};

    // sensitivity -> onset-strength threshold (High keeps weak hits = more cuts)
    const int   sens = sliceSensitivity.load();
    const float strThresh = (sens >= 2) ? 0.06f : (sens == 1 ? 0.20f : 0.45f);

    const double step = gridStepSamples();                 // 0 if no reliable BPM -> pure transient
    // snap strength -> how far an onset can be pulled to a grid line (Tight pulls hardest)
    const int    snap = sliceSnap.load();
    const double tol  = step * ((snap >= 2) ? 0.5 : (snap == 1 ? 0.3 : 0.12));
    const int    minGap = juce::jmax ((int) (0.05 * sr),
                                      step > 0.0 ? (int) (step * 0.45) : 0);

    struct Cut { int pos; float str; };
    std::vector<Cut> cand;
    for (const auto& o : on)
    {
        if (o.second < strThresh) continue;
        int pos = o.first;
        if (step > 0.0)
        {
            const int g = nearestGridLine (pos);
            if (std::abs (pos - g) <= (int) tol) pos = g;   // snap this transient to the beat grid
        }
        cand.push_back ({ juce::jlimit (0, juce::jmax (0, len - 1), pos), o.second });
    }
    if (cand.empty()) return {};

    std::sort (cand.begin(), cand.end(), [] (const Cut& a, const Cut& b) { return a.pos < b.pos; });
    std::vector<Cut> merged;
    for (const auto& c : cand)
    {
        if (! merged.empty() && c.pos - merged.back().pos < minGap)
        {
            if (c.str > merged.back().str) merged.back() = c;   // collapse a cluster to its strongest
        }
        else merged.push_back (c);
    }

    if ((int) merged.size() > 16)                            // keep the 16 strongest, then re-sort
    {
        std::sort (merged.begin(), merged.end(), [] (const Cut& a, const Cut& b) { return a.str > b.str; });
        merged.resize (16);
        std::sort (merged.begin(), merged.end(), [] (const Cut& a, const Cut& b) { return a.pos < b.pos; });
    }

    std::vector<int> out;
    for (const auto& c : merged) out.push_back (c.pos);
    if (out.empty() || out[0] > (int) (0.05 * sr))          // pad 1 starts at the top
    {
        if ((int) out.size() >= 16) out.pop_back();
        out.insert (out.begin(), 0);
    }
    return out;
}

void GentSamplerAudioProcessor::autoSliceMusical()
{
    auto src = getSource();
    if (src == nullptr) return;

    bool haveOnsets;
    { const juce::SpinLock::ScopedLockType sl (infoLock); haveOnsets = ! transientOnsets.empty(); }
    if (! haveOnsets)                                        // not analyzed yet -> analyze, then slice
    {
        analysisThenSlice = true;
        analysisKeepCues  = false;
        wantAnalysis = true;
        notify();
        return;
    }

    auto s = computeBlendedSlices();
    if (s.empty())   // nothing passed the sensitivity gate -> fall back to the top-16 transients
    { const juce::SpinLock::ScopedLockType sl (infoLock); s = transientSlices; }
    if (! s.empty())
        applySlices (s, src->buffer.getNumSamples());
}

void GentSamplerAudioProcessor::uiTrigger (int pad, bool on)
{
    int s1, n1, s2, n2;
    uiFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0)      uiEvents[(size_t) s1] = { pad, on };
    else if (n2 > 0) uiEvents[(size_t) s2] = { pad, on };
    uiFifo.finishedWrite (juce::jmin (1, n1 + n2));
}

bool GentSamplerAudioProcessor::isPadPlaying (int pad) const
{
    for (const auto& v : voices)
        if (v.active && v.pad == pad && v.env > 0.001f)
            return true;
    return false;
}

// ============================================================================
//  Worker thread: analysis + offline stretch render
// ============================================================================

double GentSamplerAudioProcessor::currentTargetSpeed() const
{
    const double sb = getEffectiveSourceBpm();
    if (sb <= 1.0)
        return 1.0;
    const int tm = (int) pTempoMode->load();
    if (tm == 1)
    {
        const double hb = hostBpm.load();
        if (hb > 1.0)
            return juce::jlimit (0.25, 4.0, hb / sb);
    }
    else if (tm == 2)
    {
        return juce::jlimit (0.25, 4.0, (double) pCustomBpm->load() / sb);
    }
    return 1.0;
}

void GentSamplerAudioProcessor::setSourceBpmOverride (double bpm)
{
    bpmOverride = bpm > 1.0 ? bpm : 0.0;
    wantRender = true;
    notify();
    ++uiDirty;
}

void GentSamplerAudioProcessor::run()
{
    // One-time stem-engine check, OFF the plugin's load path so the sampler
    // always loads cleanly. Guarded so it can never take the plugin down.
   #ifndef GENT_NO_STEMS
    {
        auto docs      = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        auto logFile   = docs.getChildFile ("GentSampler_engine_check.txt");
        auto modelsDir = docs.getChildFile ("GentSampler").getChildFile ("models");
        try
        {
            gentCheckStemEngine (logFile, modelsDir);
        }
        catch (...)
        {
            logFile.replaceWithText ("GentSampler stem-engine check: exception while probing ONNX Runtime\n");
        }
    }
   #endif

    while (! threadShouldExit())
    {
        wait (250);
        if (threadShouldExit())
            return;
        if (wantAnalysis.exchange (false))
            doAnalysisJob();
        if (wantClassify.exchange (false))
            doClassifyJob();
        if (wantSectionReport.exchange (false))
            doSectionReportJob();
        if (wantSectionApply.exchange (false))
            doSectionApplyJob();
        if (wantKitSave.exchange (false))
            doKitSaveJob();
        // DATA_INTEGRITY_SPEC.md Change 2: the async restore-load decode must
        // land BEFORE render/stem-cache in the same wait-cycle, so a same-cycle
        // wake handles the new source first (cosmetic ordering vs stem cache —
        // stems don't reference source samples — but render legitimately needs
        // the freshly adopted source).
        if (wantRestoreLoad.exchange (false))
            doRestoreLoadJob();
        if (wantRender.exchange (false))
            doRenderJob();
        doPadRenderJobs();      // cheap staleness scan; rebuilds only what changed

        // Stage 2a trigger: explicit request, OR a one-shot sentinel file
        // (Documents\GentSampler\separate_now.txt) for testing without UI.
        bool doStems = wantStems.exchange (false);
        if (! doStems)
        {
            auto gdir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                            .getChildFile ("GentSampler");
            // no-UI test hooks (drop an empty file in Documents\GentSampler\):
            //   separate_now.txt  -> CPU, standard (htdemucs_6s)
            //   separate_gpu.txt  -> GPU provider chain (CUDA), standard (htdemucs_6s)
            //   separate_maxq.txt -> GPU provider chain (CUDA), max quality (htdemucs_ft)
            auto cpuFile  = gdir.getChildFile ("separate_now.txt");
            auto gpuFile  = gdir.getChildFile ("separate_gpu.txt");
            auto maxqFile = gdir.getChildFile ("separate_maxq.txt");
            if (getSource() != nullptr)
            {
                if (maxqFile.existsAsFile())     { maxqFile.deleteFile(); stemUseGpu = true;  stemMaxQuality = true;  doStems = true; }
                else if (gpuFile.existsAsFile()) { gpuFile.deleteFile();  stemUseGpu = true;  stemMaxQuality = false; doStems = true; }
                else if (cpuFile.existsAsFile()) { cpuFile.deleteFile();  stemUseGpu = false; stemMaxQuality = false; doStems = true; }
            }
        }
        if (doStems)
            doStemJob();

        if (wantStemRender.exchange (false))
            doStemRenderJob();

        if (wantStemCacheLoad.exchange (false))
            doStemCacheLoadJob();

        if (wantTranscription.exchange (false))
            doTranscriptionJob();
    }
}

// ----------------------------------------------------------------------------
//  Generic offline time-stretch of a buffer region (pitch preserved).
//  speed > 1 = faster/shorter, speed < 1 = slower/longer.
void GentSamplerAudioProcessor::offlineStretchSlice (const juce::AudioBuffer<float>& in, int start, int numIn,
                                                     double sampleRate, double speed, juce::AudioBuffer<float>& out)
{
    const int ch = in.getNumChannels();
    signalsmith::stretch::SignalsmithStretch<float> st;
    st.presetDefault (ch, (float) sampleRate);
    st.setTransposeSemitones (0.0f);

    const int outLen = juce::jmax (1, (int) std::ceil ((double) numIn / speed));
    const int lat = st.outputLatency() + (int) std::round ((double) st.inputLatency() / speed);

    juce::AudioBuffer<float> tmp (ch, outLen + lat + 1024);
    tmp.clear();

    std::vector<const float*> ip ((size_t) ch);
    std::vector<float*>       op ((size_t) ch);
    std::vector<float>        zeros (8192, 0.0f);
    std::vector<const float*> zp ((size_t) ch, zeros.data());

    int inPos = 0, outPos = 0;
    const int total = outLen + lat;
    while (outPos < total)
    {
        const int oc = juce::jmin (1024, total - outPos);
        const int targetIn = (int) std::round ((double) (outPos + oc) * speed);
        const int icnt = juce::jlimit (0, numIn - inPos, targetIn - inPos);
        for (int cidx = 0; cidx < ch; ++cidx)
            op[(size_t) cidx] = tmp.getWritePointer (cidx) + outPos;
        if (icnt > 0)
        {
            for (int cidx = 0; cidx < ch; ++cidx)
                ip[(size_t) cidx] = in.getReadPointer (cidx) + start + inPos;
            st.process (ip, icnt, op, oc);
            inPos += icnt;
        }
        else
        {
            const int z = juce::jlimit (16, (int) zeros.size(), (int) std::round ((double) oc * speed));
            st.process (zp, z, op, oc);
        }
        outPos += oc;
    }

    out.setSize (ch, outLen);
    for (int cidx = 0; cidx < ch; ++cidx)
        out.copyFrom (cidx, 0, tmp, cidx, lat, outLen);
}

// ----------------------------------------------------------------------------
//  Rebuild any per-pad SPEED renders whose stamp no longer matches reality.
void GentSamplerAudioProcessor::doPadRenderJobs()
{
    RenderedSample::Ptr r;
    {
        const juce::SpinLock::ScopedLockType sl (rendLock);
        r = rendered;
    }
    if (r == nullptr || r->buffer.getNumSamples() < 2)
        return;

    const int len = r->buffer.getNumSamples();

    // the stretched stems, parallel to the master render. Per-pad stems are only
    // cut when they line up sample-for-sample with the master (same length);
    // otherwise we skip them this pass (they'll catch up on a later scan).
    RenderedStems::Ptr rs;
    {
        const juce::SpinLock::ScopedLockType sl (stemRendLock);
        rs = renderedStems;
    }
    bool stemsUsable = (rs != nullptr);
    if (stemsUsable)
        for (int k = 0; k < 6; ++k)
            if (rs->buffers[(size_t) k].getNumSamples() != len) { stemsUsable = false; break; }
    const int curStemGen = stemsUsable ? rs->generation : -1;

    for (int pad = 0; pad < 16; ++pad)
    {
        if (threadShouldExit())
            return;

        const float sp = pSpeed[(size_t) pad]->load();
        if (cues[(size_t) pad].load() < 0 || std::abs (sp - 1.0f) < 0.015f)
        {
            if (padRenders[(size_t) pad] != nullptr)
            {
                const juce::SpinLock::ScopedLockType sl (padLock);
                padRenders[(size_t) pad] = nullptr;
            }
            continue;
        }

        const int cueSrc = cues[(size_t) pad].load();
        int endSrc = getEffectiveCueEnd (pad);
        // cap the slice so an open-ended pad can't queue a monster render
        endSrc = juce::jmin (endSrc, cueSrc + (int) (r->sourceSampleRate * 30.0));

        auto existing = padRenders[(size_t) pad];
        // does the slice itself (independent of stems) still match?
        const bool stampMatches = existing != nullptr
            && existing->masterGen == r->generation
            && existing->cueSrc == cueSrc
            && existing->endSrc == endSrc
            && std::abs (existing->speed - sp) < 0.005f;

        if (stampMatches && existing->stemRenderGen == curStemGen)
            continue;   // fully up to date (slice + stems)

        const int start = juce::jlimit (0, len - 2, (int) ((double) cueSrc / r->speed));
        const int stop  = juce::jlimit (start + 1, len, (int) ((double) endSrc / r->speed));

        PadRender::Ptr out = new PadRender();
        out->sourceSampleRate = r->sourceSampleRate;
        out->masterGen = r->generation;
        out->cueSrc = cueSrc;
        out->endSrc = endSrc;
        out->speed = sp;

        if (stampMatches)
        {
            // ONLY the stems changed (just separated / re-separated) — the slice is
            // identical. Reuse the existing buffer AND generation so a voice already
            // sounding on this pad isn't hard-cut (gen mismatch -> v.active=false ->
            // click); it survives and simply gains stems on its next block.
            out->buffer.makeCopyOf (existing->buffer);
            out->generation = existing->generation;
        }
        else
        {
            out->generation = ++padRenderCounter;
            offlineStretchSlice (r->buffer, start, stop - start, r->sourceSampleRate, (double) sp, out->buffer);
        }

        // stretch the 6 stems the SAME way (same slice/speed) so they line up with
        // out->buffer — this is what makes PAD SOURCE work on a speed-toggled pad,
        // and keeps the toggle instant (all stems resident, no re-derivation).
        if (stemsUsable)
        {
            const int padLen = out->buffer.getNumSamples();
            bool ok = (padLen > 0);
            for (int k = 0; k < 6 && ! threadShouldExit(); ++k)
            {
                offlineStretchSlice (rs->buffers[(size_t) k], start, stop - start,
                                     r->sourceSampleRate, (double) sp, out->stems[(size_t) k]);
                if (out->stems[(size_t) k].getNumSamples() != padLen)
                    ok = false;
            }
            out->hasStems = ok;
        }
        out->stemRenderGen = curStemGen;   // always stamp (incl. -1 when no stems)

        {
            const juce::SpinLock::ScopedLockType sl (padLock);
            padRenders[(size_t) pad] = out;
        }
        ++uiDirty;
    }
}

void GentSamplerAudioProcessor::doAnalysisJob()
{
    auto src = getSource();
    if (src == nullptr)
        return;

    // DATA_INTEGRITY_SPEC.md Change 1: snapshot restore authority at entry. A
    // project restore that lands mid-analysis (applyStateTree bumps restoreGen
    // and writes its own cues) must not have this job's slice-apply clobber
    // those restored cues afterwards — analysis DATA (onsets/features/bpm,
    // just below) still stores unconditionally; only the cue-writing applies
    // are suppressed.
    const int genAtEntry = restoreGen.load();

    analyzing = true;
    ++uiDirty;

    const auto res = gent::Analyzer::analyze (src->buffer, src->sampleRate);

    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        detectedKey = res.key;
        transientSlices = res.slices;
        transientOnsets = res.onsets;
        featureFrames = res.frames;
        featureFrameRate = res.frameRate;
    }
    detectedBpm = res.bpm;

    bool suppressed = false;

    if (analysisThenSlice.exchange (false))
    {
        if (restoreGen.load() == genAtEntry)
        {
            // a music-aware auto-slice was requested before onsets existed: do it now
            auto s = computeBlendedSlices();
            applySlices (! s.empty() ? s : res.slices, src->buffer.getNumSamples());
        }
        else
            suppressed = true;
    }
    else if (! analysisKeepCues.load())
    {
        if (restoreGen.load() == genAtEntry)
            applySlices (res.slices, src->buffer.getNumSamples());
        else
            suppressed = true;
    }

    // KIT_SPEC.md PART A: sliceKit() was called before onsets existed (set
    // kitPending + analysisKeepCues=false, so res.slices above already
    // applied first) — onsets now exist, so lay the kit immediately,
    // overwriting that auto-apply. No pushUndo here: the one CueSnap for
    // this whole chain was already pushed at the original menu click
    // (sliceKit's caller), and neither this worker nor sliceKit() itself
    // ever calls pushUndo() (grep-verified: pushUndo() has exactly one call
    // site, the message-thread menu dispatch).
    if (kitPending.exchange (false))
    {
        if (restoreGen.load() == genAtEntry)
            sliceKit (kitSensPending.load());
        else
            suppressed = true;
    }

    if (suppressed)
        DBG ("doAnalysisJob: slice apply suppressed (state restore landed mid-analysis)");

    analyzing = false;
    ++uiDirty;

    wantRender = true;
    notify();
}

// ----------------------------------------------------------------------------
//  Stem separation (Stage 2a): runs the ONNX engine on the loaded source,
//  off the audio thread. Stores 6 source-aligned stem buffers, and (proof)
//  writes them as WAVs + a log to Documents\GentSampler.
// ----------------------------------------------------------------------------
void GentSamplerAudioProcessor::requestStemSeparation()
{
    wantStems = true;
    notify();
}

// P2 wiring (PHASE3_SPEC.md PART 2 "Wiring + gate deliverable"): mirror of
// requestStemSeparation() above — sets a flag, wakes the worker, returns.
void GentSamplerAudioProcessor::requestClassifyReport()
{
    wantClassify = true;
    notify();
}

// SECTIONS Part 2 (SECTIONS_SPEC.md PART 2 + AMENDMENT P2-A): mirror of
// requestClassifyReport() above.
void GentSamplerAudioProcessor::requestSectionReport (int sensitivity)
{
    sectionSensitivityForReport = sensitivity;
    wantSectionReport = true;
    notify();
}

void GentSamplerAudioProcessor::requestSectionApply (int sensitivity)
{
    sectionSensitivityForApply = sensitivity;
    wantSectionApply = true;
    notify();
}

bool GentSamplerAudioProcessor::hasStems() const
{
    const juce::SpinLock::ScopedLockType sl (stemLock);
    return stemSet != nullptr;
}

StemSet::Ptr GentSamplerAudioProcessor::getStems() const
{
    const juce::SpinLock::ScopedLockType sl (stemLock);
    return stemSet;
}

juce::String GentSamplerAudioProcessor::getStemStatus() const
{
    const juce::SpinLock::ScopedLockType sl (infoLock);
    return stemStatus;
}

void GentSamplerAudioProcessor::doStemJob()
{
    auto src = getSource();
    if (src == nullptr)
        return;

    // PREPACKAGE_AUDIT_2.md #1 (WAVE4_SPEC.md F1): snapshot restoreGen + the
    // source identity at entry (doStemCacheLoadJob :3594 pattern). A restore
    // bumps restoreGen (applyStateTree); a direct/kit load replaces the
    // source object without bumping it — the bail condition below covers both.
    const int genAtEntry = restoreGen.load();

    auto setStatus = [this] (const juce::String& s)
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        stemStatus = s;
    };

    separating = true;
    stemProgress = 0.0f;
    ++uiDirty;
    setStatus ("separating...");

    const auto gentDir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                            .getChildFile ("GentSampler");
    const auto modelsDir = gentDir.getChildFile ("models");
    const auto logFile   = gentDir.getChildFile ("stems_log.txt");

    gentDir.createDirectory();

    // live heartbeat so progress is visible while it runs (refresh this file)
    auto heartbeat = [&] (const juce::String& s)
    {
        logFile.replaceWithText ("GentSampler stem separation\nsource: " + getFileName() + "\n" + s + "\n");
    };
    heartbeat ("status: loading models (first run loads ~1.8 GB, please wait)...");

    // ---- first-run weight download (~1.79 GB, checksum-verified, resumable) ----
    if (! GentModels::modelsPresent (modelsDir))
    {
        downloadingModels = true; stemProgress = 0.0f; ++uiDirty;

        juce::String dlErr;
        const bool got = GentModels::ensureModelsPresent (
            modelsDir,
            [this, &heartbeat] (float frac, const juce::String& label)   // progress
            {
                stemProgress = frac;
                const int pct = juce::roundToInt (frac * 100.0f);
                { const juce::SpinLock::ScopedLockType sl (infoLock);
                  stemStatus = "Downloading models " + juce::String (pct) + "%"; }
                ++uiDirty;
                heartbeat ("status: downloading models " + juce::String (pct) + "%   (" + label + ")");
            },
            [this, &heartbeat] (const juce::String& s)                   // status
            {
                { const juce::SpinLock::ScopedLockType sl (infoLock); stemStatus = s; }
                ++uiDirty;
                heartbeat ("status: " + s);
            },
            [this] { return threadShouldExit(); },                       // cancel
            dlErr);

        downloadingModels = false; ++uiDirty;

        if (! got)
        {
            setStatus ("model download failed: " + dlErr
                       + "  (or place the .onnx files in " + modelsDir.getFullPathName() + ")");
            heartbeat ("status: MODEL DOWNLOAD FAILED -> " + dlErr);
            separating = false; stemProgress = 0.0f; ++uiDirty;
            return;
        }
        stemProgress = 0.0f; ++uiDirty;   // reset the bar for the separation phase
    }

    // lazy init, and re-init if the CPU/GPU choice changed since last time
    const int wantMode = stemUseGpu ? 1 : 0;
    if (! stemEngine.isInitialised() || stemEngineMode != wantMode)
    {
        juce::String err;
        bool ok = false;
        try { ok = stemEngine.initialise (modelsDir, /*useGPU*/ stemUseGpu, err); }
        catch (const std::exception& e) { err = juce::String ("exception: ") + e.what(); }
        catch (...)                     { err = "unknown exception during init"; }

        if (! ok)
        {
            setStatus ("init failed: " + err);
            heartbeat ("status: INIT FAILED -> " + err);
            separating = false; ++uiDirty;
            return;
        }
        stemEngineMode = wantMode;
    }

    // Quality dial (CPU): FAST = 6s @ overlap 0.25; HQ = 6s @ overlap 0.5;
    // MAX = the htdemucs_ft + 6s hybrid @ overlap 0.5 (drums/bass/vocals from ft,
    // guitar/piano from 6s) — ~5 model passes, several minutes on CPU, no GPU needed.
    // (The legacy GPU-only stemMaxQuality sentinel hook still forces hybrid too.)
    const int  q          = stemQuality.load();
    const bool maxQ       = (q >= 2) || stemMaxQuality.load();
    const auto mode       = maxQ ? StemSeparator::Mode::Hybrid : StemSeparator::Mode::SixStem;
    const float sepOverlap = (q >= 1 || maxQ) ? 0.5f : 0.25f;

    heartbeat (juce::String ("status: separating (")
               + (maxQ ? "max/hybrid" : (q >= 1 ? "HQ" : "standard"))
               + ") ... 0%");

    juce::String err;
    auto progressFn = [this, &heartbeat] (const StemSeparator::Progress& p)
    {
        stemProgress = p.fraction;
        ++uiDirty;
        heartbeat ("status: separating " + juce::String (juce::roundToInt (p.fraction * 100.0f)) + "%"
                   + "   (" + p.label + ")");
    };

    const double t0 = juce::Time::getMillisecondCounterHiRes();
    std::map<juce::String, juce::AudioBuffer<float>> stems;
    try
    {
        // ADDENDUM T: abort mid-separation on a teardown request (destructor's
        // stopThread) so the worker exits promptly instead of running to
        // completion; a partial result is discarded exactly like any other
        // separation failure (stems.empty() below).
        stems = stemEngine.separate (src->buffer, src->sampleRate, mode, sepOverlap, progressFn, err,
                                     [this] { return threadShouldExit(); });
    }
    catch (const std::exception& e) { err = juce::String ("exception: ") + e.what(); }
    catch (...)                     { err = "unknown exception during separation"; }
    const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;

    if (stems.empty())
    {
        setStatus ("separation failed: " + err);
        heartbeat ("status: SEPARATION FAILED -> " + err);
        separating = false; ++uiDirty;
        return;
    }

    // store 6 stems (engine returns 44100 Hz; alignment to source domain is Stage 2b)
    auto set = new StemSet();
    set->sampleRate = (double) StemSeparator::kModelSampleRate;
    set->generation = ++stemGeneration;
    for (int i = 0; i < 6; ++i)
    {
        const juce::String key (StemSet::name (i));
        auto it = stems.find (key);
        if (it != stems.end())
            set->buffers[(size_t) i] = std::move (it->second);
    }

    // PREPACKAGE_AUDIT_2.md #1 (WAVE4_SPEC.md F1) — site 1: a restore or a
    // direct/kit load landed while separate() was running. This result
    // belongs to a superseded source; the incoming load/restore already owns
    // stemCacheKey for itself, so we must not touch it here.
    if (restoreGen.load() != genAtEntry || getSource() != src)
    {
        DBG ("doStemJob: bail (site 1), source changed during separation");
        delete set;
        setStatus ("separation discarded (source changed during separation)");
        separating = false; stemProgress = 0.0f; ++uiDirty;
        return;
    }

    {
        const juce::SpinLock::ScopedLockType sl (stemLock);
        stemSet = set;
    }

    // ---- KIT_SPEC.md PART C: disk stem cache (best-effort, never fails the
    // separation) — FLAC-encode the 6 stems to Documents\GentSampler\stemcache\
    // <key>\stemN.flac so a later host-project reopen can restore them without
    // re-separating. Any encode failure deletes the partial <key> dir and
    // leaves stemCacheKey empty (cache miss, same as never having cached).
    {
        const juce::String key = computeStemKey (src);
        if (key.isNotEmpty())
        {
            const auto cacheRoot = gentDir.getChildFile ("stemcache");
            const auto readme = cacheRoot.getChildFile ("README.txt");
            if (! readme.existsAsFile())
            {
                cacheRoot.createDirectory();
                readme.replaceWithText ("GentSampler stem cache — safe to delete; stems re-separate on demand.");
            }

            const auto keyDir = cacheRoot.getChildFile (key);
            keyDir.createDirectory();

            bool cacheOk = true;
            for (int i = 0; i < 6 && cacheOk; ++i)
            {
                // ADDENDUM T: teardown request mid cache-write -> treat like
                // any other cache-write failure (partial keyDir removed below).
                if (threadShouldExit())
                {
                    DBG ("doStemJob: stem cache aborted (teardown) before stem " << i);
                    cacheOk = false;
                    break;
                }
                auto& buf = set->buffers[(size_t) i];
                if (buf.getNumSamples() == 0)
                    continue;   // skip empty stems, mirrors doKitSaveJob
                juce::String cacheErr;
                const auto dest = keyDir.getChildFile ("stem" + juce::String (i) + ".flac");
                if (encodeFlacTo (dest, buf, set->sampleRate, cacheErr) == juce::File())
                {
                    DBG ("doStemJob: stem cache encode failed: " << cacheErr);
                    cacheOk = false;
                }
            }

            // PREPACKAGE_AUDIT_2.md #1 (WAVE4_SPEC.md F1) — site 2: the encode
            // loop runs for seconds-to-minutes, a real second window after the
            // stemSet from site 1 was already published. On bail here we must
            // RETRACT that published set (pointer-identity check under
            // stemLock) and leave stemCacheKey untouched in BOTH directions —
            // the incoming load/restore already owns it for itself; clearing
            // it here would clobber that. The just-written cache dir on disk
            // is left in place: it's keyed to the old source's content hash,
            // benign, and correct if that source ever returns.
            if (restoreGen.load() != genAtEntry || getSource() != src)
            {
                DBG ("doStemJob: bail (site 2), source changed during separation");
                {
                    const juce::SpinLock::ScopedLockType sl (stemLock);
                    if (stemSet.get() == set)
                        stemSet = nullptr;
                }
                setStatus ("separation discarded (source changed during separation)");
                separating = false; stemProgress = 0.0f; ++uiDirty;
                return;
            }

            if (cacheOk)
            {
                const juce::SpinLock::ScopedLockType sl (infoLock);
                stemCacheKey = key;
            }
            else
            {
                keyDir.deleteRecursively();
                const juce::SpinLock::ScopedLockType sl (infoLock);
                stemCacheKey.clear();
            }
        }
    }

    // ---- proof: write the 6 stems as WAVs so they can be A/B'd vs run_onnx.py ----
    juce::String log;
    log << "GentSampler stem separation\n";
    log << "source: " << getFileName() << "\n";
    log << "took: " << juce::String (secs, 1) << " s   (" << stemEngine.selectedProvider()
        << (maxQ ? ", max quality" : ", standard") << ")\n";
    const auto outDir = gentDir.getChildFile ("stems_out");
    outDir.createDirectory();
    juce::WavAudioFormat wav;
    for (int i = 0; i < 6; ++i)
    {
        auto& buf = set->buffers[(size_t) i];
        if (buf.getNumSamples() == 0) { log << "  " << StemSet::name (i) << ": (empty)\n"; continue; }
        const auto f = outDir.getChildFile (juce::String (StemSet::name (i)) + ".wav");
        f.deleteFile();
        if (auto* os = f.createOutputStream().release())
        {
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (os, set->sampleRate, (unsigned int) buf.getNumChannels(), 24, {}, 0));
            if (w != nullptr)
            {
                w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
                w.reset();
                log << "  " << StemSet::name (i) << ".wav  (" << buf.getNumSamples() << " smp)\n";
            }
            else { delete os; log << "  " << StemSet::name (i) << ": writer failed\n"; }
        }
    }
    log << "stems written to: " << outDir.getFullPathName() << "\n";
    logFile.replaceWithText (log);

    const juce::String gpuErr = stemEngine.gpuErrorMessage();
    if (gpuErr.isNotEmpty())
    {
        // GPU was requested but couldn't be used (hang / EP failure / unavailable);
        // the engine finished on CPU. Disable GPU so the next run doesn't retry it,
        // and record the exact reason to the log for diagnosis.
        stemUseGpu = false;
        stemEngineMode = -1;     // force a clean CPU re-init next time
        setStatus ("done (" + juce::String (secs, 1) + " s) - GPU failed, used CPU");
        logFile.appendText ("\nGPU DIAGNOSTIC: " + gpuErr + "\n");
    }
    else
    {
        setStatus ("done (" + juce::String (secs, 1) + " s, " + stemEngine.selectedProvider() + ")");
    }
    stemProgress = 1.0f;
    separating = false;
    ++uiDirty;

    // stems exist now -> render them through the stretch (2b-1)
    wantStemRender = true;
    notify();
}

// ----------------------------------------------------------------------------
//  P2 wiring (PHASE3_SPEC.md PART 2 "Wiring + gate deliverable"): read-only
//  classification of the CURRENTLY assigned slices into a text report. Runs
//  entirely on the worker thread (called only from run()'s wantClassify
//  dispatch, exactly like doAnalysisJob/doStemJob). Never reslices, never
//  writes cues/masks/grain, never pushes undo — a lying report is worse than
//  no report, so every number here is read straight off the same cached
//  state the rest of the engine uses, under the same locks.
// ----------------------------------------------------------------------------
void GentSamplerAudioProcessor::doClassifyJob()
{
    const auto gentDir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                            .getChildFile ("GentSampler");
    gentDir.createDirectory();

    const juce::String stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    const juce::String rawName = getFileName();
    const juce::String safeName = rawName.isNotEmpty()
        ? juce::File::createLegalFileName (rawName)
        : juce::String ("no_source");
    const auto reportFile = gentDir.getChildFile ("ClassifyReport_" + safeName + "_" + stamp + ".txt");

    auto writeAndReveal = [reportFile] (const juce::String& text)
    {
        reportFile.replaceWithText (text);
        juce::MessageManager::callAsync ([reportFile]
        {
            reportFile.revealToUser();
        });
    };

    // ---- snapshot every input under its own lock (worker thread) ----------
    auto src = getSource();
    if (src == nullptr)
    {
        writeAndReveal ("GentSampler classify report\n\nNo source loaded — nothing to classify.\n");
        return;
    }

    std::vector<gent::FrameFeatures> frames;
    double frameRate = 0.0;
    getFeatureFrames (frames, frameRate);

    std::vector<std::pair<int, float>> onsets;
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        onsets = transientOnsets;
    }

    StemSet::Ptr stems = getStems();      // refcounted keep-alive for the duration of this job
    const bool stemsAvailable = (stems != nullptr);
    const juce::String stemStatusStr = getStemStatus();
    const int stemQualityIdx = getStemQuality();
    static const char* const kQualityName[3] = { "FAST", "HQ", "MAX" };
    const juce::String qualityStr = (stemQualityIdx >= 0 && stemQualityIdx < 3)
        ? kQualityName[stemQualityIdx] : "?";

    const double bpm = getEffectiveSourceBpm();
    const double sr  = src->sampleRate;
    const int sourceLen = src->buffer.getNumSamples();

    // assigned pads (cue >= 0), ascending by cue position
    struct AssignedPad { int pad; int cue; };
    std::vector<AssignedPad> assigned;
    for (int pad = 0; pad < 16; ++pad)
    {
        const int c = cues[(size_t) pad].load();
        if (c >= 0)
            assigned.push_back ({ pad, c });
    }
    std::sort (assigned.begin(), assigned.end(),
               [] (const AssignedPad& a, const AssignedPad& b)
               { return a.cue != b.cue ? a.cue < b.cue : a.pad < b.pad; });   // pad tiebreaker = deterministic

    // ---- guard: nothing to classify (silent-source landmine — never crash) ----
    if (sourceLen < 2 || assigned.empty() || frames.empty() || frameRate <= 0.0)
    {
        juce::String text;
        text << "GentSampler classify report\n";
        text << "source: " << rawName << "\n\n";
        if (sourceLen < 2)
            text << "Source is empty — nothing to classify.\n";
        else if (assigned.empty())
            text << "No pads are assigned (no cues set) — nothing to classify.\n";
        else
            text << "No cached analysis features yet — run an auto-slice first, "
                    "then try Classify again.\n";
        writeAndReveal (text);
        return;
    }

    // sample position -> frame index (frames were stored at hop => frameRate = sr/hop)
    auto sampleToFrame = [frameRate, sr] (int samplePos) -> int
    {
        return (int) std::llround ((double) samplePos * frameRate / sr);
    };

    const auto& thresholds = stemsAvailable ? gent::kThreshStems : gent::kThreshNoStems;

    struct Row
    {
        int pad; int cue; gent::SliceAggregates agg; gent::ClassifyResult result;
        float onsetStrength; float stemShare[6];
        bool ambiguous;   // stems present but mix ambiguous -> classified with kThreshNoStems
    };
    std::vector<Row> rows;
    rows.reserve (assigned.size());

    const int frameCount = (int) frames.size();

    for (size_t i = 0; i < assigned.size(); ++i)
    {
        const int cue = assigned[i].cue;
        const int sliceEndSample = (i + 1 < assigned.size()) ? assigned[i + 1].cue : sourceLen;

        const int startFrame = juce::jlimit (0, frameCount, sampleToFrame (cue));
        const int endFrame   = juce::jlimit (0, frameCount, sampleToFrame (sliceEndSample));

        const gent::SliceAggregates agg = gent::aggregateSliceFeatures (frames, frameRate, startFrame, endFrame);

        // nearest onset to this cue (0 if none cached)
        float onsetStrength = 0.0f;
        if (! onsets.empty())
        {
            int bestIdx = 0;
            int bestDist = std::abs (onsets[0].first - cue);
            for (int k = 1; k < (int) onsets.size(); ++k)
            {
                const int d = std::abs (onsets[(size_t) k].first - cue);
                if (d < bestDist) { bestDist = d; bestIdx = k; }
            }
            onsetStrength = onsets[(size_t) bestIdx].second;
        }

        // per-stem energy share (source-domain sample-range sum of squares)
        float stemShare[6] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
        if (stemsAvailable)
        {
            const int rangeStart = juce::jlimit (0, sourceLen, cue);
            const int rangeEnd   = juce::jlimit (rangeStart, sourceLen, sliceEndSample);
            float energy[6] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
            float total = 0.0f;
            for (int s = 0; s < 6; ++s)
            {
                const auto& buf = stems->buffers[(size_t) s];
                const int bufLen = buf.getNumSamples();
                const int st = juce::jmin (rangeStart, bufLen);
                const int en = juce::jmin (rangeEnd, bufLen);
                float e = 0.0f;
                for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                {
                    const float* d = buf.getReadPointer (ch);
                    for (int n = st; n < en; ++n)
                        e += d[n] * d[n];
                }
                energy[s] = e;
                total += e;
            }
            if (total > 0.0f)
                for (int s = 0; s < 6; ++s)
                    stemShare[s] = energy[s] / total;
        }

        gent::SliceFeatures feat {};
        feat.bandRatio[0] = agg.bandRatio[0];
        feat.bandRatio[1] = agg.bandRatio[1];
        feat.bandRatio[2] = agg.bandRatio[2];
        feat.bandRatio[3] = agg.bandRatio[3];
        feat.centroidHz   = agg.centroidHz;
        feat.zcr          = agg.zcr;
        feat.decaySec     = agg.decaySec;
        feat.durationSec  = agg.durationSec;
        feat.chromaFlatness = agg.chromaFlatness;
        feat.onsetStrength  = onsetStrength;
        feat.hasStems       = stemsAvailable;
        for (int s = 0; s < 6; ++s)
            feat.stemShare[s] = stemShare[s];

        const gent::ClassifyResult result = gent::classifySlice (feat, thresholds);

        // Report-honesty (R1 finding): classifySlice internally falls through to
        // kThreshNoStems for a stems-present slice whose mix is ambiguous
        // (drums < drumsDominant AND tonal < tonalDominant) — so its effective
        // preset differs from the kThreshStems header. Mirror that exact branch
        // condition here so the row can be marked, and Joe isn't tuning it
        // against the wrong printed thresholds.
        const float tonalMix = stemShare[1] + stemShare[2] + stemShare[3] + stemShare[4];
        const bool ambiguous = stemsAvailable
                            && stemShare[0] < gent::kThreshStems.drumsDominant
                            && tonalMix     < gent::kThreshStems.tonalDominant;

        Row row;
        row.pad = assigned[i].pad;
        row.cue = cue;
        row.agg = agg;
        row.result = result;
        row.onsetStrength = onsetStrength;
        for (int s = 0; s < 6; ++s) row.stemShare[s] = stemShare[s];
        row.ambiguous = ambiguous;
        rows.push_back (row);
    }

    // ---- build the fixed-width report ----------------------------------
    static const char* const kClassName[6] = { "KICK", "SNARE", "HAT", "PERC", "TONAL", "OTHER" };

    auto formatTime = [sr] (int samplePos) -> juce::String
    {
        const double totalSec = sr > 0.0 ? (double) samplePos / sr : 0.0;
        const int mins = (int) (totalSec / 60.0);
        const double secs = totalSec - mins * 60.0;
        return juce::String (mins) + ":" + juce::String (secs, 3).paddedLeft ('0', 6);
    };

    juce::String text;
    text << "GentSampler classify report\n";
    text << "source:  " << rawName << "\n";
    text << "bpm:     " << (bpm > 1.0 ? juce::String (bpm, 1) : juce::String ("(unknown)")) << "\n";
    text << "stems:   " << (stemsAvailable ? "yes" : "no")
         << (stemsAvailable ? ("  (quality " + qualityStr + ", " + stemStatusStr + ")") : juce::String())
         << "\n";
    text << "preset:  " << (stemsAvailable ? "kThreshStems" : "kThreshNoStems") << "\n";
    text << "\nactive thresholds (" << (stemsAvailable ? "kThreshStems" : "kThreshNoStems") << "):\n";
    text << "  drumsDominant   = " << juce::String (thresholds.drumsDominant, 2) << "\n";
    text << "  tonalDominant   = " << juce::String (thresholds.tonalDominant, 2) << "\n";
    text << "  kickLowRatio    = " << juce::String (thresholds.kickLowRatio, 2) << "\n";
    text << "  kickCentroidMax = " << juce::String (thresholds.kickCentroidMax, 1) << "\n";
    text << "  kickDecayMax    = " << juce::String (thresholds.kickDecayMax, 2) << "\n";
    text << "  hatHighRatio    = " << juce::String (thresholds.hatHighRatio, 2) << "\n";
    text << "  hatZcrMin       = " << juce::String (thresholds.hatZcrMin, 2) << "\n";
    text << "  hatDurMax       = " << juce::String (thresholds.hatDurMax, 2) << "\n";
    text << "  snareMidRatio   = " << juce::String (thresholds.snareMidRatio, 2) << "\n";
    text << "  snareFlatMin    = " << juce::String (thresholds.snareFlatMin, 2) << "\n";
    text << "  tonalFlatMax    = " << juce::String (thresholds.tonalFlatMax, 2) << "\n";
    text << "  tonalDurMin     = " << juce::String (thresholds.tonalDurMin, 2) << "\n";
    text << "  minConfidence   = " << juce::String (thresholds.minConfidence, 2) << "\n";

    text << "\n";
    text << juce::String ("slice").paddedRight (' ', 6)
         << juce::String ("pad").paddedRight (' ', 5)
         << juce::String ("time").paddedRight (' ', 11)
         << juce::String ("class").paddedRight (' ', 7)
         << juce::String ("conf").paddedRight (' ', 6)
         << juce::String ("low").paddedRight (' ', 6)
         << juce::String ("mid1").paddedRight (' ', 6)
         << juce::String ("mid2").paddedRight (' ', 6)
         << juce::String ("high").paddedRight (' ', 6)
         << juce::String ("centHz").paddedRight (' ', 9)
         << juce::String ("zcr").paddedRight (' ', 6)
         << juce::String ("decay").paddedRight (' ', 7)
         << juce::String ("dur").paddedRight (' ', 7)
         << juce::String ("flat").paddedRight (' ', 6)
         << juce::String ("drums%/tonal%") << "\n";

    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto& r = rows[i];
        juce::String stemCol = "no stems";
        if (stemsAvailable)
        {
            const float drumsPct = r.stemShare[0] * 100.0f;
            const float tonalPct = (r.stemShare[1] + r.stemShare[2] + r.stemShare[3] + r.stemShare[4]) * 100.0f;
            stemCol = juce::String (drumsPct, 0) + "%/" + juce::String (tonalPct, 0) + "%";
        }

        text << juce::String ((int) i + 1).paddedRight (' ', 6)
             << juce::String (r.pad).paddedRight (' ', 5)
             << formatTime (r.cue).paddedRight (' ', 11)
             << juce::String (kClassName[r.result.cls]).paddedRight (' ', 7)
             << juce::String (r.result.confidence, 2).paddedRight (' ', 6)
             << juce::String (r.agg.bandRatio[0], 2).paddedRight (' ', 6)
             << juce::String (r.agg.bandRatio[1], 2).paddedRight (' ', 6)
             << juce::String (r.agg.bandRatio[2], 2).paddedRight (' ', 6)
             << juce::String (r.agg.bandRatio[3], 2).paddedRight (' ', 6)
             << juce::String (r.agg.centroidHz, 0).paddedRight (' ', 9)
             << juce::String (r.agg.zcr, 3).paddedRight (' ', 6)
             << juce::String (r.agg.decaySec, 2).paddedRight (' ', 7)
             << juce::String (r.agg.durationSec, 2).paddedRight (' ', 7)
             << juce::String (r.agg.chromaFlatness, 2).paddedRight (' ', 6)
             << stemCol << (r.ambiguous ? "  *" : "") << "\n";
    }

    // Footnote for the ambiguous-fallthrough marker (only when it fired).
    bool anyAmbiguous = false;
    for (const auto& r : rows) if (r.ambiguous) { anyAmbiguous = true; break; }
    if (anyAmbiguous)
        text << "\n* stem mix ambiguous (drums < " << juce::String (gent::kThreshStems.drumsDominant, 2)
             << " AND tonal < " << juce::String (gent::kThreshStems.tonalDominant, 2)
             << ") — this row was classified with the kThreshNoStems thresholds, "
                "NOT the kThreshStems preset printed above.\n";

    writeAndReveal (text);
}

// ----------------------------------------------------------------------------
//  SECTIONS Part 2 (SECTIONS_SPEC.md PART 2 + AMENDMENT P2-A): NOVELTY
//  (spectral-change) section boundary detection, dev-only pre-gate wiring.
//  Runs entirely on the worker thread (run()'s wantSectionReport/
//  wantSectionApply dispatch), mirroring doClassifyJob's report-file idiom.
// ----------------------------------------------------------------------------
namespace
{
juce::String gentSectionsSensitivityName (int sensitivity)
{
    return sensitivity <= 0 ? "few" : (sensitivity == 1 ? "medium" : "many");
}
}

void GentSamplerAudioProcessor::doSectionReportJob()
{
    const auto gentDir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                            .getChildFile ("GentSampler");
    gentDir.createDirectory();
    const auto reportFile = gentDir.getChildFile ("GentSampler_sections_report.txt");

    auto writeAndReveal = [reportFile] (const juce::String& text)
    {
        reportFile.replaceWithText (text);
        juce::MessageManager::callAsync ([reportFile]
        {
            reportFile.revealToUser();
        });
    };

    const int sensitivity = sectionSensitivityForReport.load();
    const juce::String sensName = gentSectionsSensitivityName (sensitivity);

    std::vector<gent::FrameFeatures> frames;
    double frameRate = 0.0;
    getFeatureFrames (frames, frameRate);

    if (frames.empty() || frameRate <= 0.0)
    {
        writeAndReveal ("GentSampler sections (novelty) report\n\n"
                         "no analysis features — load/analyze a source first\n");
        return;
    }

    const double spBeat = samplesPerBeat();
    if (spBeat <= 0.0)
    {
        writeAndReveal ("GentSampler sections (novelty) report\n\n"
                         "no reliable BPM\n");
        return;
    }
    const double samplesPerBar = spBeat * 4.0;

    auto src = getSource();
    if (src == nullptr)
    {
        writeAndReveal ("GentSampler sections (novelty) report\n\n"
                         "no source loaded — nothing to detect.\n");
        return;
    }
    const double sr = src->sampleRate;
    const int sourceLen = src->buffer.getNumSamples();

    // ---- run the pure chain once (this is the ground truth for boundaries) ----
    const auto boundaries = gent::noveltyBoundaries (frames, frameRate, samplesPerBar, sr, sensitivity);

    // ---- recompute stages a/b + mean/std locally to report per-boundary
    //      score/threshold and per-part {bandDist, chromaDist} at the peak ----
    const auto curve = gent::noveltyCurve (frames);
    const double framesPerBar = frameRate * (samplesPerBar / sr);
    const int w = std::max (3, (int) std::round (gent::kNoveltyThresh.smoothBars * framesPerBar));
    const int minGap = std::max (1, (int) std::round (gent::kNoveltyThresh.minSectionBars * framesPerBar));
    const float k = sensitivity <= 0 ? gent::kNoveltyThresh.kFew
                  : sensitivity == 1 ? gent::kNoveltyThresh.kMedium
                                     : gent::kNoveltyThresh.kMany;
    const auto smoothed = gent::smoothCurve (curve, w);

    double mean = 0.0;
    for (float v : smoothed) mean += (double) v;
    if (! smoothed.empty()) mean /= (double) smoothed.size();
    double var = 0.0;
    for (float v : smoothed) var += ((double) v - mean) * ((double) v - mean);
    if (! smoothed.empty()) var /= (double) smoothed.size();
    const double stddev = std::sqrt (var);
    const double threshold = mean + (double) k * stddev;

    // sample position -> nearest frame index (for looking up smoothed/curve
    // values at a reported boundary; boundary 0 has no associated peak).
    auto sampleToFrame = [frameRate, sr] (int samplePos) -> int
    {
        return (int) std::llround ((double) samplePos * frameRate / sr);
    };

    auto formatTime = [sr] (int samplePos) -> juce::String
    {
        const double totalSec = sr > 0.0 ? (double) samplePos / sr : 0.0;
        const int mins = (int) (totalSec / 60.0);
        const double secs = totalSec - mins * 60.0;
        return juce::String (mins) + ":" + juce::String (secs, 3).paddedLeft ('0', 6);
    };

    juce::String text;
    text << "GentSampler sections (novelty) report\n";
    text << "source:       " << getFileName() << "\n";
    text << "bpm:          " << juce::String (getEffectiveSourceBpm(), 1) << "\n";
    text << "sensitivity:  " << sensName << "\n";
    text << "smooth w:     " << w << " frames\n";
    text << "k:            " << juce::String (k, 2) << "\n";
    text << "framesPerBar: " << juce::String (framesPerBar, 2) << "\n";
    text << "\n";

    const int frameCount = (int) frames.size();
    const int totalBoundaries = (int) boundaries.size();
    const int rowCap = 64;
    const int rowCount = std::min (totalBoundaries, rowCap);

    text << juce::String ("idx").paddedRight (' ', 5)
         << juce::String ("sample").paddedRight (' ', 10)
         << juce::String ("time").paddedRight (' ', 11)
         << juce::String ("bar").paddedRight (' ', 8)
         << juce::String ("score").paddedRight (' ', 9)
         << juce::String ("thresh").paddedRight (' ', 9)
         << juce::String ("bandD").paddedRight (' ', 9)
         << juce::String ("chromaD").paddedRight (' ', 9)
         << "led" << "\n";

    for (int i = 0; i < rowCount; ++i)
    {
        const int pos = boundaries[(size_t) i];
        const double barNum = samplesPerBar > 0.0 ? (double) pos / samplesPerBar + 1.0 : 1.0;

        juce::String scoreStr = "-", threshStr = "-", bandStr = "-", chromaStr = "-", ledStr = "-";
        if (pos != 0 || i != 0)   // boundary 0 (the mandatory first section start) has no peak
        {
            const int frameIdx = juce::jlimit (0, frameCount - 1, sampleToFrame (pos));
            const float score = (frameIdx >= 0 && frameIdx < (int) smoothed.size()) ? smoothed[(size_t) frameIdx] : 0.0f;
            const float bandD = (frameIdx >= 0 && frameIdx < (int) curve.size()) ? curve[(size_t) frameIdx].bandDist : 0.0f;
            const float chromaD = (frameIdx >= 0 && frameIdx < (int) curve.size()) ? curve[(size_t) frameIdx].chromaDist : 0.0f;
            scoreStr = juce::String (score, 5);
            threshStr = juce::String (threshold, 5);
            bandStr = juce::String (bandD, 4);
            chromaStr = juce::String (chromaD, 4);
            const float loBig = std::max (bandD, chromaD);
            const float loSmall = std::min (bandD, chromaD);
            if (loSmall <= 0.0f || loBig > loSmall * 1.5f)
                ledStr = (bandD >= chromaD) ? "band-led" : "chroma-led";
            else
                ledStr = "both";
        }

        text << juce::String (i).paddedRight (' ', 5)
             << juce::String (pos).paddedRight (' ', 10)
             << formatTime (pos).paddedRight (' ', 11)
             << juce::String (barNum, 1).paddedRight (' ', 8)
             << scoreStr.paddedRight (' ', 9)
             << threshStr.paddedRight (' ', 9)
             << bandStr.paddedRight (' ', 9)
             << chromaStr.paddedRight (' ', 9)
             << ledStr << "\n";
    }

    text << "\nsection count: " << totalBoundaries << "\n";
    if (totalBoundaries > 16)
        text << "OVERFLOW: " << totalBoundaries << " sections, first 16 laid on APPLY\n";
    if (totalBoundaries > rowCap)
        text << "(capped at " << rowCap << " listed rows; " << (totalBoundaries - rowCap)
             << " more boundaries not shown)\n";

    writeAndReveal (text);
}

void GentSamplerAudioProcessor::doSectionApplyJob()
{
    // DATA_INTEGRITY_SPEC.md Change 1: same restore-authority guard as
    // doAnalysisJob — a restore landing mid-job must not have this job's
    // slice-apply clobber the restored cues.
    const int genAtEntry = restoreGen.load();

    const int sensitivity = sectionSensitivityForApply.load();

    std::vector<gent::FrameFeatures> frames;
    double frameRate = 0.0;
    getFeatureFrames (frames, frameRate);
    if (frames.empty() || frameRate <= 0.0)
        return;

    const double spBeat = samplesPerBeat();
    if (spBeat <= 0.0)
        return;
    const double samplesPerBar = spBeat * 4.0;

    auto src = getSource();
    if (src == nullptr)
        return;
    const double sr = src->sampleRate;
    const int len = src->buffer.getNumSamples();

    auto boundaries = gent::noveltyBoundaries (frames, frameRate, samplesPerBar, sr, sensitivity);

    // Drop any boundary >= len before assigning (cues < len enforced).
    boundaries.erase (std::remove_if (boundaries.begin(), boundaries.end(),
                                       [len] (int b) { return b >= len; }),
                       boundaries.end());

    if ((int) boundaries.size() > 16)
        DBG ("SECTIONS (novelty): " << boundaries.size() << " sections at sensitivity "
             << gentSectionsSensitivityName (sensitivity) << ", first 16 laid");

    std::vector<int> s (16, -1);
    for (int i = 0; i < 16 && i < (int) boundaries.size(); ++i)
        s[(size_t) i] = boundaries[(size_t) i];

    if (restoreGen.load() == genAtEntry)
        applySlices (s, len);
    else
        DBG ("doSectionApplyJob: slice apply suppressed (state restore landed mid-job)");
}

// ----------------------------------------------------------------------------
//  Stretch one buffer exactly like doRenderJob stretches the source, so a
//  rendered stem lines up sample-for-sample with the master rendered buffer.
// ----------------------------------------------------------------------------
void GentSamplerAudioProcessor::stretchForRender (const juce::AudioBuffer<float>& in, double srcSR,
                                                  double speed, double pitch, juce::AudioBuffer<float>& out)
{
    const int ch    = in.getNumChannels();
    const int inLen = in.getNumSamples();

    if (std::abs (speed - 1.0) < 0.0005 && std::abs (pitch) < 0.01)
    {
        out.makeCopyOf (in);
        return;
    }

    signalsmith::stretch::SignalsmithStretch<float> st;
    st.presetDefault (ch, (float) srcSR);
    st.setTransposeSemitones ((float) pitch);

    const int outLen = (int) std::ceil ((double) inLen / speed);
    const int lat = st.outputLatency() + (int) std::round ((double) st.inputLatency() / speed);

    juce::AudioBuffer<float> tmp (ch, outLen + lat + 1024);
    tmp.clear();

    std::vector<const float*> ip ((size_t) ch);
    std::vector<float*>       op ((size_t) ch);
    std::vector<float>        zeros (8192, 0.0f);
    std::vector<const float*> zp ((size_t) ch, zeros.data());

    int inPos = 0, outPos = 0;
    const int total = outLen + lat;
    while (outPos < total)
    {
        if (threadShouldExit())
            return;
        const int oc = juce::jmin (1024, total - outPos);
        const int targetIn = (int) std::round ((double) (outPos + oc) * speed);
        const int icnt = juce::jlimit (0, inLen - inPos, targetIn - inPos);
        for (int c = 0; c < ch; ++c)
            op[(size_t) c] = tmp.getWritePointer (c) + outPos;
        if (icnt > 0)
        {
            for (int c = 0; c < ch; ++c)
                ip[(size_t) c] = in.getReadPointer (c) + inPos;
            st.process (ip, icnt, op, oc);
            inPos += icnt;
        }
        else
        {
            const int z = juce::jlimit (16, (int) zeros.size(), (int) std::round ((double) oc * speed));
            st.process (zp, z, op, oc);
        }
        outPos += oc;
    }

    out.setSize (ch, outLen);
    for (int c = 0; c < ch; ++c)
        out.copyFrom (c, 0, tmp, c, lat, outLen);
}

// ----------------------------------------------------------------------------
//  2b-1: render the 6 stems through the same stretch as the master, parallel to
//  `rendered`. No playback change yet. Writes a proof log: each rendered stem
//  must match the master rendered length, and (at speed 1 / pitch 0) the stems
//  should sum back to the source.
// ----------------------------------------------------------------------------
void GentSamplerAudioProcessor::doStemRenderJob()
{
    auto src = getSource();
    StemSet::Ptr ss = getStems();
    if (src == nullptr || ss == nullptr)
        return;

    const double speed = currentTargetSpeed();
    const double pitch = (double) pMasterPitch->load();
    const double srcSR = src->sampleRate;
    const int    srcLen = src->buffer.getNumSamples();

    RenderedStems::Ptr out = new RenderedStems();
    out->speed = speed;
    out->generation = ++stemRenderGeneration;
    out->stemSetGeneration = ss->generation;

    for (int i = 0; i < 6; ++i)
    {
        if (threadShouldExit())
            return;

        juce::AudioBuffer<float>& stem = ss->buffers[(size_t) i];
        if (stem.getNumSamples() == 0) { out->buffers[(size_t) i].setSize (2, 0); continue; }

        // bring the stem into the source sample-rate domain if needed
        juce::AudioBuffer<float> atSrc;
        if (std::abs (ss->sampleRate - srcSR) < 1.0)
        {
            atSrc.makeCopyOf (stem);
        }
        else
        {
            const int ch = stem.getNumChannels();
            atSrc.setSize (ch, srcLen);
            atSrc.clear();
            const double ratio = ss->sampleRate / srcSR;   // speedRatio for Lagrange
            for (int c = 0; c < ch; ++c)
            {
                juce::LagrangeInterpolator interp;
                interp.reset();
                interp.process (ratio, stem.getReadPointer (c), atSrc.getWritePointer (c), srcLen, stem.getNumSamples(), 0);
            }
        }

        stretchForRender (atSrc, srcSR, speed, pitch, out->buffers[(size_t) i]);
    }

    {
        const juce::SpinLock::ScopedLockType sl (stemRendLock);
        renderedStems = out;
    }
    ++uiDirty;
    // wake the worker so any speed-toggled pads cut their per-pad stems now that
    // the stretched stems exist (doPadRenderJobs picks them up on the next scan).
    notify();

    // ---- proof log ----
    int masterLen = 0;
    { const juce::SpinLock::ScopedLockType sl (rendLock);
      if (rendered != nullptr) masterLen = rendered->buffer.getNumSamples(); }

    juce::String log;
    log << "GentSampler stem render (2b-1)\n";
    log << "speed: " << juce::String (speed, 4) << "   pitch: " << juce::String (pitch, 2) << "\n";
    log << "master rendered length: " << masterLen << "\n";
    bool allMatch = true;
    for (int i = 0; i < 6; ++i)
    {
        const int len = out->buffers[(size_t) i].getNumSamples();
        if (masterLen > 0 && std::abs (len - masterLen) > 2) allMatch = false;
        log << "  " << StemSet::name (i) << ": " << len << " smp"
            << (masterLen > 0 && std::abs (len - masterLen) <= 2 ? "  (aligned)\n" : "\n");
    }
    log << "alignment: " << (allMatch ? "OK (stems match master length)\n" : "MISMATCH -> tell Claude\n");

    // at speed 1 / pitch 0, stems should sum back to source
    if (std::abs (speed - 1.0) < 0.0005 && std::abs (pitch) < 0.01)
    {
        const auto& s = src->buffer;
        const int ch = juce::jmin (2, s.getNumChannels());
        const int N  = juce::jmin (srcLen, out->buffers[0].getNumSamples());
        float worst = 0.0f;
        for (int c = 0; c < ch; ++c)
        {
            const float* sp = s.getReadPointer (c);
            for (int n = 0; n < N; ++n)
            {
                float sum = 0.0f;
                for (int i = 0; i < 6; ++i)
                    if (out->buffers[(size_t) i].getNumSamples() > n)
                        sum += out->buffers[(size_t) i].getReadPointer (c)[n];
                worst = juce::jmax (worst, std::abs (sum - sp[n]));
            }
        }
        log << "sum-of-stems vs source: max diff " << juce::String (worst, 5)
            << (worst < 0.02f ? "  (reconstructs)\n" : "  (investigate)\n");
    }
    else
    {
        log << "sum-of-stems check skipped (only meaningful at speed 1 / pitch 0)\n";
    }

    juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("GentSampler").getChildFile ("stem_render_log.txt")
        .replaceWithText (log);
}

void GentSamplerAudioProcessor::doRenderJob()
{
    auto src = getSource();

    const double speed = currentTargetSpeed();
    const double pitch = (double) pMasterPitch->load();

    lastRenderSpeed = speed;
    lastRenderPitch = pitch;

    if (src == nullptr)
        return;

    RenderedSample::Ptr out = new RenderedSample();
    out->speed = speed;
    out->sourceSampleRate = src->sampleRate;
    out->generation = ++renderGeneration;

    const auto& in = src->buffer;
    const int ch    = in.getNumChannels();
    const int inLen = in.getNumSamples();

    if (std::abs (speed - 1.0) < 0.0005 && std::abs (pitch) < 0.01)
    {
        out->buffer.makeCopyOf (in);
    }
    else
    {
        signalsmith::stretch::SignalsmithStretch<float> st;
        st.presetDefault (ch, (float) src->sampleRate);
        st.setTransposeSemitones ((float) pitch);

        const int outLen = (int) std::ceil ((double) inLen / speed);
        const int lat = st.outputLatency() + (int) std::round ((double) st.inputLatency() / speed);

        juce::AudioBuffer<float> tmp (ch, outLen + lat + 1024);
        tmp.clear();

        std::vector<const float*> ip ((size_t) ch);
        std::vector<float*>       op ((size_t) ch);
        std::vector<float>        zeros (8192, 0.0f);
        std::vector<const float*> zp ((size_t) ch, zeros.data());

        int inPos = 0, outPos = 0;
        const int total = outLen + lat;

        while (outPos < total)
        {
            if (threadShouldExit())
                return;

            const int oc = juce::jmin (1024, total - outPos);
            const int targetIn = (int) std::round ((double) (outPos + oc) * speed);
            const int icnt = juce::jlimit (0, inLen - inPos, targetIn - inPos);

            for (int c = 0; c < ch; ++c)
                op[(size_t) c] = tmp.getWritePointer (c) + outPos;

            if (icnt > 0)
            {
                for (int c = 0; c < ch; ++c)
                    ip[(size_t) c] = in.getReadPointer (c) + inPos;
                st.process (ip, icnt, op, oc);
                inPos += icnt;
            }
            else
            {
                const int z = juce::jlimit (16, (int) zeros.size(), (int) std::round ((double) oc * speed));
                st.process (zp, z, op, oc);
            }

            outPos += oc;
        }

        out->buffer.setSize (ch, outLen);
        for (int c = 0; c < ch; ++c)
            out->buffer.copyFrom (c, 0, tmp, c, lat, outLen);
    }

    {
        const juce::SpinLock::ScopedLockType sl (rendLock);
        rendered = out;
    }
    ++uiDirty;

    // keep stretched stems in sync with the master render (tempo/pitch changes)
    if (hasStems())
    {
        wantStemRender = true;
        notify();
    }
}

// ============================================================================
//  Voices
// ============================================================================

void GentSamplerAudioProcessor::startVoice (int pad, float vel, int extraSemis, int note, bool kbMode)
{
    if (pad < 0 || pad >= 16)
        return;

    auto r = active;
    if (r == nullptr || r->buffer.getNumSamples() < 2)
        return;

    const int mode = (int) pMode[(size_t) pad]->load();   // 0 gate, 1 one-shot, 2 latch

    // LATCH: a press while the pad is already sounding switches it OFF
    {
        bool padSounding = false;
        for (auto& w : voices)
            if (w.active && w.pad == pad && w.state != 2) { padSounding = true; break; }
        if (gent::latchPressTurnsOff (mode, kbMode, padSounding))
        {
            releaseVoices (pad, -1, false, false);
            return;
        }
    }

    if (! kbMode)
        releaseVoices (pad, -1, true, false);

    // CHOKE GROUPS: a trigger silences any sounding voice on OTHER pads that share
    // this pad's non-zero choke group (classic open/closed hi-hat behaviour). Same
    // quick 4 ms fade as releaseVoices to avoid clicks.
    {
        const int myChoke = (int) pChoke[(size_t) pad]->load();
        if (myChoke > 0)
        {
            const float quickDec = 1.0f / juce::jmax (1.0f, 0.004f * (float) getSampleRate());
            for (auto& w : voices)
                if (gent::chokeSilences (myChoke, pad, w.active, w.state, w.pad,
                                         w.pad >= 0 ? (int) pChoke[(size_t) w.pad]->load() : -1))
                {
                    w.state  = 2;
                    w.relDec = quickDec;
                }
        }
    }

    Voice* v = nullptr;
    for (auto& w : voices)
        if (! w.active) { v = &w; break; }
    if (v == nullptr)
        v = &voices[0];

    const double fs   = getSampleRate();
    const float att   = pAtt[(size_t) pad]->load();
    const float rel   = pRel[(size_t) pad]->load();
    const float semis = pPitch[(size_t) pad]->load() + (float) extraSemis;
    const float crush = pCrush[(size_t) pad]->load();
    const float spd   = pSpeed[(size_t) pad]->load();

    // granular (srcKind 2) reads from the master render domain (honoring the pad's
    // stem mask + bleed at grain-read time) — it overrides the per-pad speed render
    // (pads with a SPEED != 1.00x otherwise play a pre-stretch)
    const bool grainOn = pGrainOn[(size_t) pad]->load() > 0.5f;
    PadRender::Ptr pr = activePads[(size_t) pad];
    const bool usePad = ! grainOn
                        && std::abs (spd - 1.0f) >= 0.015f
                        && pr != nullptr
                        && pr->masterGen == r->generation
                        && std::abs (pr->speed - spd) < 0.02f
                        && pr->buffer.getNumSamples() > 2;

    v->srcKind   = grainOn ? 2 : (usePad ? 1 : 0);
    v->rate      = ((usePad ? pr->sourceSampleRate : r->sourceSampleRate) / fs)
                   * std::pow (2.0, (double) semis / 12.0);
    v->pos       = usePad ? 0.0
                          : juce::jlimit (0.0, (double) r->buffer.getNumSamples() - 2.0,
                                          (double) cues[(size_t) pad].load() / r->speed);

    // where this voice stops on its own: explicit region end, else next cue
    // (slice mode), else end of sample — all mapped into the rendered domain.
    // OPEN slice (collapsed window) = play from the start to the sample end and
    // let GATE release / ONE-SHOT-to-end decide the cut; ignore all boundaries.
    {
        const int cueSrc = cues[(size_t) pad].load();
        const int eEnd = cueEnds[(size_t) pad].load();
        const bool openSlice = (eEnd == kOpenSlice);
        double endPos = (double) r->buffer.getNumSamples() - 1.0;
        if (openSlice)
        {
            // endPos stays the sample end; no cue/stop-at-cue boundary applies
        }
        else if (eEnd > cueSrc)
        {
            endPos = juce::jmin (endPos, (double) eEnd / r->speed);
        }
        else if (pSlice[(size_t) pad]->load() > 0.5f)
        {
            int nextSrc = std::numeric_limits<int>::max();
            for (int i = 0; i < 16; ++i)
            {
                const int c = cues[(size_t) i].load();
                if (c >= 0 && c > cueSrc && c < nextSrc)
                    nextSrc = c;
            }
            if (nextSrc != std::numeric_limits<int>::max())
                endPos = juce::jmin (endPos, (double) nextSrc / r->speed);
        }
        if (usePad)
            endPos = (double) pr->buffer.getNumSamples() - 1.0;
        v->endPos = juce::jmax (endPos, v->pos + 1.0);
    }

    // loop / reverse: the playable region is [loopStart .. endPos]. Reverse plays
    // it back-to-front (starts at the end); loop wraps instead of stopping.
    v->loopStart = v->pos;
    v->loop      = pLoop[(size_t) pad]->load()    > 0.5f;
    v->reverse   = pReverse[(size_t) pad]->load() > 0.5f;
    if (v->reverse)
        v->pos = v->endPos;

    v->attInc    = 1.0f / juce::jmax (1.0f, att * 0.001f * (float) fs);
    v->padRelDec = 1.0f / juce::jmax (1.0f, rel * 0.001f * (float) fs);
    v->relDec    = v->padRelDec;
    v->env       = 0.0f;
    v->state     = 0;
    // velocity-respect toggle: when off, MIDI velocity is ignored (full level). Pad
    // clicks already pass vel = 1.0f, so they are unaffected either way.
    const float lvlVel = velToLevel.load() ? vel : 1.0f;
    v->level     = juce::jmax (0.05f, lvlVel) * pLevel[(size_t) pad]->load();
    {
        // equal-power pan, captured at trigger (like level): -1 = hard L, +1 = hard R.
        // Unity-center law (×√2) so a centered pad is 0 dB — unchanged vs pre-pan
        // builds — and the curve only attenuates/boosts once you move off centre.
        const float pan = juce::jlimit (-1.0f, 1.0f, pPan[(size_t) pad]->load());
        const float th  = (pan * 0.5f + 0.5f) * juce::MathConstants<float>::halfPi;
        v->panL = juce::MathConstants<float>::sqrt2 * std::cos (th);
        v->panR = juce::MathConstants<float>::sqrt2 * std::sin (th);
    }
    v->gate      = gent::voiceGateFlag (mode, kbMode);    // gate mode releases on key-up
    v->crush     = crush;
    v->crushQ    = std::pow (2.0f, (16.0f - crush * 8.0f) - 1.0f);   // hoisted out of the per-sample loop
    v->holdN     = 1 + (int) (crush * 5.0f);
    v->holdCount = 0;
    v->heldL = v->heldR = 0.0f;
    v->fic1L = v->fic2L = v->fic1R = v->fic2R = 0.0f;   // clear filter state to avoid clicks on reuse
    v->sgInit    = false;                                // snap stem gains on this voice's first stem-mixed block
    // granular: fresh grain pool, pitch-independent base rate, per-voice RNG seed
    for (auto& gr : v->grains) gr.active = false;
    v->grainSpawnAcc = 0.0;
    v->grainAge      = 0;
    v->grainBaseRate = r->sourceSampleRate / fs;
    v->grainRng      = 0x9e3779b9u ^ (unsigned) (pad * 2654435761u) ^ (unsigned) (note + 1);
    v->pad       = pad;
    v->note      = note;
    v->gen       = usePad ? pr->generation : r->generation;
    v->active    = true;

    lastTriggerPad = pad;
    ++lastTriggerCount;

    logCap (pad, true, vel);
}

void GentSamplerAudioProcessor::releaseVoices (int pad, int note, bool quick, bool onlyGate)
{
    const float quickDec = 1.0f / juce::jmax (1.0f, 0.004f * (float) getSampleRate());
    bool releasedAny = false;
    for (auto& v : voices)
    {
        if (! gent::releaseApplies (v.active, v.pad, v.note, v.gate, v.state, pad, note, quick, onlyGate))
            continue;
        v.state = 2;
        v.relDec = quick ? quickDec : v.padRelDec;
        releasedAny = true;
    }
    if (releasedAny && ! quick)
        logCap (pad, false, 0.0f);
}

void GentSamplerAudioProcessor::handleNoteOn (int note, float vel)
{
    if (pKb->load() > 0.5f)
    {
        startVoice (selectedPad.load(), vel, note - 60, note, true);
        return;
    }
    if (note >= 36 && note < 52)
    {
        const int pad = note - 36;
        selectedPad = pad;                   // panel follows the pad
        if (cues[(size_t) pad].load() < 0)   // unassigned: drop a cue at the playhead
        {
            // WAVE1_SPEC.md F1 (audit #2 rt-safety): this used to call
            // assignPadCue(pad) directly here, on the audio thread -- pushUndo()
            // heap-allocates (std::vector history push/erase) with no lock,
            // unsynchronized against the message thread's own undo()/redo()/
            // pushUndo() callers. The audio-thread side is now ONLY an atomic
            // position write + triggerAsyncUpdate() -- no allocation, no
            // history access. handleAsyncUpdate() (message thread) does the
            // real pushUndo()/setCue()/cueEnds assignment. Position source is
            // exactly what assignPadCue() itself reads -- both already atomics,
            // read here without calling any non-atomic-reading function.
            const int pos = (previewingA.load() && previewPlayPos.load() >= 0)
                                 ? previewPlayPos.load()
                                 : assignCursor.load();
            pendingAssignPos[(size_t) pad] = pos;
            triggerAsyncUpdate();
        }
        else
            startVoice (pad, vel, 0, note, false);
    }
}

void GentSamplerAudioProcessor::handleNoteOff (int note)
{
    if (pKb->load() > 0.5f)
        releaseVoices (selectedPad.load(), note, false, false);
    else if (note >= 36 && note < 52)
        releaseVoices (note - 36, -1, false, true);
}

// WAVE1_SPEC.md F1 (audit #2 rt-safety): juce::AsyncUpdater dispatches this on
// the message thread. Sweeps all 16 slots (not just one) so near-simultaneous
// MIDI taps on different pads within the same message-loop tick are all
// serviced, one CueSnap (pushUndo()) per pad actually assigned. Re-checks
// cues[pad] < 0 after the exchange -- a drag-drop assignPadCue() on the
// message thread may have raced the audio-thread tick and already assigned
// this pad; if so, skip it (drafter Q2.2 ruling: race resolves to skip).
// WAVE4 F3 (PREPACKAGE_AUDIT_2 #3, planner OQ-A path (a)): Wave-1's own
// framing of this deferral as "VISUAL-only" (WAVE1_SPEC.md:104-106) was
// verified FALSE against baseline 29969f7 assignPadCue -- baseline started
// an audible voice on the same tap that assigned the cue. Restored here to
// match assignPadCue's post-setCue sequence verbatim (minus selectedPad,
// which handleNoteOn already sets on the audio thread -- see below).
void GentSamplerAudioProcessor::handleAsyncUpdate()
{
    for (int pad = 0; pad < 16; ++pad)
    {
        const int pos = pendingAssignPos[(size_t) pad].exchange (-1);
        if (pos < 0)
            continue;
        if (cues[(size_t) pad].load() >= 0)   // already assigned since the tick -- skip
            continue;
        pushUndo();
        setCue (pad, pos, /*snap*/ false);
        cueEnds[(size_t) pad] = kOpenSlice;   // 9f2ab28 point-cue semantics: bare POINT cue, open end
        // Baseline parity (29969f7 assignPadCue, post-setCue tail): audition the freshly
        // assigned pad on the same tap, same as a drag-drop assign. NOT
        // selectedPad -- handleNoteOn already wrote that on the audio thread
        // before triggerAsyncUpdate(); writing it again here would be a
        // duplicate, not parity. If several pads were assigned in the same
        // sweep, the last one wins the single auditionPad slot -- accepted
        // coalescing, same spirit as the multi-pad sweep itself.
        lastTriggerPad = pad;
        ++lastTriggerCount;
        ++uiDirty;
        if (! previewingA.load())
            auditionPad = pad;
    }

    // PREPACKAGE_AUDIT.md #11: drain the graveyard ring stashed by
    // processBlock's swap sites. Nulling a slot here is where the actual
    // ReferenceCountedObject free happens -- on the message thread, never on
    // the audio thread. Coalescing is fine: one callback drains everything
    // that has piled up since the last one.
    for (std::uint32_t r = graveR.load (std::memory_order_relaxed);
         r != graveW.load (std::memory_order_acquire);
         ++r)
    {
        graveyard[(size_t) (r & 63u)] = nullptr;   // wrap-safe unsigned index
        graveR.store (r + 1u, std::memory_order_release);
    }
}

// ============================================================================
//  MIDI performance capture
// ============================================================================

void GentSamplerAudioProcessor::logCap (int pad, bool on, float vel)
{
    if (! capturing.load())
        return;
    const double sec = (double) capSamples.load() / juce::jmax (1.0, getSampleRate());
    const juce::SpinLock::ScopedTryLockType tl (capLock);
    if (tl.isLocked() && capEvents.size() < capEvents.capacity())
        capEvents.push_back ({ sec, pad, on, vel });
}

void GentSamplerAudioProcessor::startMidiCapture()
{
    {
        const juce::SpinLock::ScopedLockType sl (capLock);
        capEvents.clear();
    }
    capSamples = 0;
    capturing = true;
}

void GentSamplerAudioProcessor::stopMidiCapture()
{
    capturing = false;
}

int GentSamplerAudioProcessor::capturedEventCount() const
{
    const juce::SpinLock::ScopedLockType sl (capLock);
    return (int) capEvents.size();
}

bool GentSamplerAudioProcessor::exportCapturedMidi (const juce::File& midFile)
{
    std::vector<CapEvent> ev;
    {
        const juce::SpinLock::ScopedLockType sl (capLock);
        ev = capEvents;
    }
    if (ev.empty())
        return false;

    double bpm = hostBpm.load();
    if (bpm <= 1.0) bpm = getEffectiveSourceBpm();
    if (bpm <= 1.0) bpm = 120.0;

    const int tpq = 960;
    const double ticksPerSec = (bpm / 60.0) * tpq;

    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) (60000000.0 / bpm)), 0.0);

    // track held pads so one-shots still get a sensible note length
    std::array<double, 16> onTime {};
    std::array<bool, 16> isOn {};
    for (auto& b : isOn) b = false;

    for (const auto& e : ev)
    {
        const int note = 36 + juce::jlimit (0, 15, e.pad);
        const double t = e.sec * ticksPerSec;
        if (e.on)
        {
            if (isOn[(size_t) e.pad])
                seq.addEvent (juce::MidiMessage::noteOff (1, note), t - 1.0);
            seq.addEvent (juce::MidiMessage::noteOn (1, note, juce::jlimit (0.05f, 1.0f, e.vel)), t);
            isOn[(size_t) e.pad] = true;
            onTime[(size_t) e.pad] = t;
        }
        else if (isOn[(size_t) e.pad])
        {
            seq.addEvent (juce::MidiMessage::noteOff (1, note), juce::jmax (t, onTime[(size_t) e.pad] + 1.0));
            isOn[(size_t) e.pad] = false;
        }
    }
    // close any hanging notes a quarter-note after their hit
    for (int p = 0; p < 16; ++p)
        if (isOn[(size_t) p])
            seq.addEvent (juce::MidiMessage::noteOff (1, 36 + p), onTime[(size_t) p] + tpq);

    seq.updateMatchedPairs();

    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (tpq);
    mf.addTrack (seq);

    midFile.deleteFile();
    juce::FileOutputStream os (midFile);
    if (! os.openedOk())
        return false;
    return mf.writeTo (os);
}

// ============================================================================
//  audio-to-MIDI transcription (Basic Pitch) — worker-thread job + SMF writer
// ============================================================================

void GentSamplerAudioProcessor::requestTranscription (int pad)
{
    transcribePad    = juce::jlimit (0, 15, pad);
    transcribeReady  = false;
    transcribeFailed = false;
    wantTranscription = true;
    notify();
}

juce::File GentSamplerAudioProcessor::getTranscriptionFile() const
{
    const juce::SpinLock::ScopedLockType sl (infoLock);
    return transcribeMidi;
}

juce::String GentSamplerAudioProcessor::getTranscribeStatus() const
{
    const juce::SpinLock::ScopedLockType sl (infoLock);
    return transcribeStatus;
}

// Snap transcribed notes (micro-timed by design) to GentSampler's own beat grid —
// the differentiator. Reuses the grid division (sliceGridDiv: Bar/Beat/1/8/1/16) that
// drives the on-screen grid; default ON, but only applied when a real tempo is known.
bool GentSamplerAudioProcessor::writeTranscribedMidi (const std::vector<BasicPitchTranscriber::Note>& notes,
                                                      const juce::File& midFile)
{
    if (notes.empty())
        return false;

    double bpm = hostBpm.load();
    if (bpm <= 1.0) bpm = getEffectiveSourceBpm();
    const bool haveTempo = (bpm > 1.0);
    if (! haveTempo) bpm = 120.0;

    const int    tpq         = 960;
    const double ticksPerSec = (bpm / 60.0) * tpq;
    const double secPerBeat  = 60.0 / bpm;

    const bool quantize = transcribeQuantize.load() && haveTempo;
    double stepBeats = 0.0;
    if (quantize)
        switch (sliceGridDiv.load())   // 0 Bar, 1 Beat, 2 1/8, 3 1/16
        {
            case 0:  stepBeats = 4.0;  break;
            case 1:  stepBeats = 1.0;  break;
            case 2:  stepBeats = 0.5;  break;
            default: stepBeats = 0.25; break;
        }

    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) (60000000.0 / bpm)), 0.0);

    for (const auto& n : notes)
    {
        double s = n.startSec, e = n.endSec;
        if (quantize && stepBeats > 0.0)
        {
            double sb = std::round ((s / secPerBeat) / stepBeats) * stepBeats;
            double eb = std::round ((e / secPerBeat) / stepBeats) * stepBeats;
            if (eb <= sb) eb = sb + stepBeats;          // keep at least one grid step
            s = sb * secPerBeat;
            e = eb * secPerBeat;
        }
        const int pitch = juce::jlimit (0, 127, n.pitch);
        const int vel   = juce::jlimit (1, 127, (int) std::lround (127.0 * (double) n.amplitude));
        const double tOn  = s * ticksPerSec;
        const double tOff = juce::jmax (e * ticksPerSec, tOn + 1.0);
        seq.addEvent (juce::MidiMessage::noteOn  (1, pitch, (juce::uint8) vel), tOn);
        seq.addEvent (juce::MidiMessage::noteOff (1, pitch), tOff);
    }

    seq.updateMatchedPairs();

    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (tpq);
    mf.addTrack (seq);

    midFile.deleteFile();
    juce::FileOutputStream os (midFile);
    if (! os.openedOk())
        return false;
    return mf.writeTo (os);
}

void GentSamplerAudioProcessor::doTranscriptionJob()
{
    const int pad = juce::jlimit (0, 15, transcribePad.load());
    transcribing     = true;
    transcribeReady  = false;
    transcribeFailed = false;
    { const juce::SpinLock::ScopedLockType sl (infoLock); transcribeStatus = "Transcribing pad " + juce::String (pad + 1) + "..."; }
    ++uiDirty;

    auto fail = [this] (const juce::String& msg)
    {
        { const juce::SpinLock::ScopedLockType sl (infoLock); transcribeStatus = msg; }
        transcribeFailed = true;
        transcribing     = false;
        ++uiDirty;
    };

    const auto modelsDir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                               .getChildFile ("GentSampler").getChildFile ("models");

    // first-use download of the tiny Basic Pitch model (independent of the stem weights)
    {
        juce::String err;
        auto prog = [this] (float f, const juce::String& lbl)
        {
            const juce::SpinLock::ScopedLockType sl (infoLock);
            transcribeStatus = "Downloading " + lbl + " " + juce::String ((int) (f * 100.0f)) + "%";
        };
        auto stat = [this] (const juce::String& s)
        { const juce::SpinLock::ScopedLockType sl (infoLock); transcribeStatus = s; };
        auto canc = [this] { return (bool) threadShouldExit(); };
        if (! GentModels::ensureBasicPitchPresent (modelsDir, prog, stat, canc, err))
        { fail ("Transcription model unavailable: " + err); return; }
    }

    juce::String err;
    if (! transcriber.ensureLoaded (GentModels::basicPitchFile (modelsDir), err))
    { fail (err); return; }

    juce::AudioBuffer<float> slice;
    double sr = 44100.0;
    if (! renderPadSlice (pad, slice, sr) || slice.getNumSamples() < 2)
    { fail ("Pad " + juce::String (pad + 1) + " has no slice to transcribe"); return; }

    const auto notes = transcriber.transcribe (slice, sr);
    if (notes.empty())
    { fail ("No pitched notes found (percussive or silent slice?)"); return; }

    const auto out = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("GentSampler_Transcribe_pad" + juce::String (pad + 1) + ".mid");
    if (! writeTranscribedMidi (notes, out))
    { fail ("Could not write the transcribed MIDI file"); return; }

    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        transcribeMidi   = out;
        transcribeStatus = juce::String ((int) notes.size()) + " notes"
                           + (transcribeQuantize.load() && getEffectiveSourceBpm() > 1.0 ? " (quantized)" : "");
    }
    transcribeReady = true;
    transcribing    = false;
    ++uiDirty;
}

// ============================================================================
//  Export: pad slices, kits, flip log
// ============================================================================

bool GentSamplerAudioProcessor::renderPadSlice (int pad, juce::AudioBuffer<float>& out, double& outSampleRate)
{
    RenderedSample::Ptr r;
    {
        const juce::SpinLock::ScopedLockType sl (rendLock);
        r = rendered;
    }
    if (r == nullptr || r->buffer.getNumSamples() < 2 || pad < 0 || pad >= 16)
        return false;

    const auto& rb = r->buffer;
    const int len = rb.getNumSamples();

    // slice = this cue → next-greater cue (or end of sample), in rendered domain
    const int cueSrc = cues[(size_t) pad].load();
    if (cueSrc < 0)
        return false;                         // unassigned pad: nothing to export
    const int eEnd = cueEnds[(size_t) pad].load();
    int endSrc;
    if (eEnd == kOpenSlice)
    {
        endSrc = std::numeric_limits<int>::max();   // open/gated: export to the sample end
    }
    else if (eEnd > cueSrc)
    {
        endSrc = eEnd;
    }
    else
    {
        int nextSrc = std::numeric_limits<int>::max();
        for (int i = 0; i < 16; ++i)
        {
            const int c = cues[(size_t) i].load();
            if (c >= 0 && c > cueSrc && c < nextSrc)
                nextSrc = c;
        }
        endSrc = nextSrc;
    }

    const int start = juce::jlimit (0, len - 2, (int) ((double) cueSrc / r->speed));
    const int end   = endSrc == std::numeric_limits<int>::max()
                        ? len
                        : juce::jlimit (start + 1, len, (int) ((double) endSrc / r->speed));

    const float semis = pPitch[(size_t) pad]->load();
    const float level = pLevel[(size_t) pad]->load();
    const float crush = pCrush[(size_t) pad]->load();
    const float spd   = pSpeed[(size_t) pad]->load();
    const double rate = std::pow (2.0, (double) semis / 12.0);

    // apply per-pad SPEED by stretching the region into a working buffer first
    juce::AudioBuffer<float> stretched;
    const bool useStretch = std::abs (spd - 1.0f) >= 0.015f;
    if (useStretch)
        offlineStretchSlice (rb, start, end - start, r->sourceSampleRate, (double) spd, stretched);
    const int holdN = 1 + (int) (crush * 5.0f);
    const float bits = 16.0f - crush * 8.0f;
    const float qLevels = std::pow (2.0f, bits - 1.0f);

    const int regionLen = useStretch ? stretched.getNumSamples() : (end - start);
    const int outLen = juce::jmax (1, (int) ((double) regionLen / rate));
    out.setSize (2, outLen);

    const auto& srcBuf = useStretch ? stretched : rb;
    const float* inL = srcBuf.getReadPointer (0);
    const float* inR = srcBuf.getNumChannels() > 1 ? srcBuf.getReadPointer (1) : inL;
    const int srcLen = useStretch ? stretched.getNumSamples() : len;

    double pos = useStretch ? 0.0 : (double) start;
    float heldL = 0.0f, heldR = 0.0f;
    int holdCount = 0;

    const double srcStop = useStretch ? (double) (srcLen - 1)
                                      : juce::jmin ((double) (len - 1), (double) end);
    for (int i = 0; i < outLen; ++i)
    {
        if (pos >= srcStop)
        {
            out.setSize (2, juce::jmax (1, i), true);
            break;
        }
        const int   i0 = (int) pos;
        const float fr = (float) (pos - (double) i0);
        float sl = inL[i0] + fr * (inL[i0 + 1] - inL[i0]);
        float sr = inR[i0] + fr * (inR[i0 + 1] - inR[i0]);

        if (crush > 0.001f)
        {
            if (holdCount <= 0)
            {
                heldL = std::round (sl * qLevels) / qLevels;
                heldR = std::round (sr * qLevels) / qLevels;
                holdCount = holdN;
            }
            sl = heldL; sr = heldR;
            --holdCount;
        }

        out.setSample (0, i, sl * level);
        out.setSample (1, i, sr * level);
        pos += rate;
    }

    outSampleRate = r->sourceSampleRate;
    return out.getNumSamples() > 1;
}

bool GentSamplerAudioProcessor::exportPad (int pad, const juce::File& wavFile)
{
    juce::AudioBuffer<float> buf;
    double sr = 44100.0;
    if (! renderPadSlice (pad, buf, sr))
        return false;

    wavFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os (wavFile.createOutputStream());
    if (os == nullptr)
        return false;

    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
    if (writer == nullptr)
        return false;
    os.release();   // writer owns the stream now

    const bool ok = writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    return ok;
}

juce::String GentSamplerAudioProcessor::buildFlipLog()
{
    juce::String log;
    juce::String path, key;
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        path = filePath;
        key = detectedKey;
    }
    auto src = getSource();
    const double srcSr = src != nullptr ? src->sampleRate : 44100.0;

    log << "GENTSAMPLER FLIP LOG\n";
    log << "====================\n";
    log << "Exported:      " << juce::Time::getCurrentTime().toString (true, true) << "\n";
    log << "Source file:   " << (path.isNotEmpty() ? path : juce::String ("(none)")) << "\n";
    log << "Detected BPM:  " << juce::String (detectedBpm.load(), 1) << "\n";
    log << "Detected key:  " << (key.isNotEmpty() ? key : juce::String ("--")) << "\n";
    log << "\nPads (positions in the ORIGINAL source recording):\n";

    for (int i = 0; i < 16; ++i)
    {
        if (cues[(size_t) i].load() < 0)
        {
            log << "  Pad " << juce::String (i + 1).paddedLeft (' ', 2) << "  (unassigned)\n";
            continue;
        }
        const double sec = (double) cues[(size_t) i].load() / srcSr;
        const double esec = (double) getEffectiveCueEnd (i) / srcSr;
        const int mins = (int) (sec / 60.0);
        const double rem = sec - mins * 60.0;
        log << "  Pad " << juce::String (i + 1).paddedLeft (' ', 2)
            << "  @ " << mins << ":" << juce::String (rem, 3).paddedLeft ('0', 6)
            << "   len " << juce::String (juce::jmax (0.0, esec - sec), 2) << "s"
            << "   pitch " << juce::String ((int) pPitch[(size_t) i]->load()) << " st"
            << "   speed " << juce::String (pSpeed[(size_t) i]->load(), 2) << "x"
            << "   crush " << juce::String ((int) (pCrush[(size_t) i]->load() * 100)) << "%\n";
    }

    log << "\nNOTE: If the source recording is someone else's copyrighted work,\n";
    log << "release requires clearance of BOTH the master and the composition.\n";
    log << "This log documents exactly what you used to make that conversation easier.\n";
    return log;
}

bool GentSamplerAudioProcessor::exportKit (const juce::File& directory)
{
    if (! directory.isDirectory())
        if (! directory.createDirectory())
            return false;

    bool any = false;
    for (int i = 0; i < 16; ++i)
    {
        const auto f = directory.getChildFile ("GentSampler_Pad" + juce::String (i + 1).paddedLeft ('0', 2) + ".wav");
        if (exportPad (i, f))
            any = true;
    }

    // MIDI map: each pad's note on a successive beat, for quick auditioning
    {
        double bpm = hostBpm.load();
        if (bpm <= 1.0) bpm = getEffectiveSourceBpm();
        if (bpm <= 1.0) bpm = 120.0;
        const int tpq = 960;
        juce::MidiMessageSequence seq;
        seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) (60000000.0 / bpm)), 0.0);
        for (int i = 0; i < 16; ++i)
        {
            seq.addEvent (juce::MidiMessage::noteOn (1, 36 + i, 0.9f), (double) (i * tpq));
            seq.addEvent (juce::MidiMessage::noteOff (1, 36 + i), (double) (i * tpq + tpq / 2));
        }
        seq.updateMatchedPairs();
        juce::MidiFile mf;
        mf.setTicksPerQuarterNote (tpq);
        mf.addTrack (seq);
        const auto f = directory.getChildFile ("GentSampler_Kit.mid");
        f.deleteFile();
        juce::FileOutputStream os (f);
        if (os.openedOk())
            mf.writeTo (os);
    }

    directory.getChildFile ("FlipLog.txt").replaceWithText (buildFlipLog());
    return any;
}

// ============================================================================
//  Kits (presets)
// ============================================================================

// KIT_SPEC.md PART B: pure snapshot-to-ValueTree, no I/O — shared by saveKit()
// (v1, synchronous XML write) and doKitSaveJob() (v2, worker-threaded ZIP
// write) so the two XML bodies never drift. Safe to call from either thread:
// every read below already goes through the same atomics/locks the rest of
// the processor uses from arbitrary threads (infoLock, .load()).
juce::ValueTree GentSamplerAudioProcessor::buildKitStateXml()
{
    auto state = apvts.copyState();
    juce::ValueTree extra ("EXTRA");
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        extra.setProperty ("path", filePath, nullptr);
        extra.setProperty ("key", detectedKey, nullptr);
        // KIT_SPEC.md PART C: disk stem-cache key (three-spot pattern) — lets a
        // reopened project/kit restore separated stems without re-separating.
        extra.setProperty ("stemKey", stemCacheKey, nullptr);
    }
    extra.setProperty ("bpm", detectedBpm.load(), nullptr);
    extra.setProperty ("ubpm", bpmOverride.load(), nullptr);
    extra.setProperty ("edW", editorW.load(), nullptr);
    extra.setProperty ("edH", editorH.load(), nullptr);
    extra.setProperty ("stemQ", stemQuality.load(), nullptr);
    extra.setProperty ("slG", sliceGridDiv.load(), nullptr);
    extra.setProperty ("slS", sliceSensitivity.load(), nullptr);
    extra.setProperty ("slP", sliceSnap.load(), nullptr);
    // SECTIONS_SPEC.md PART 3: SLICE split-chip mode model (three-spot pattern).
    extra.setProperty ("slMode", sliceModeSel.load(), nullptr);
    extra.setProperty ("slBars", sectionBars.load(), nullptr);
    extra.setProperty ("slSens", sectionSens.load(), nullptr);
    extra.setProperty ("slEven", gridEvenSel.load(), nullptr);
    // KIT_SPEC.md PART A: mode 5's persisted sub-option (three-spot pattern).
    extra.setProperty ("slKSens", kitSens.load(), nullptr);
    extra.setProperty ("snap", snapEnabled.load(), nullptr);
    extra.setProperty ("vel2l", velToLevel.load(), nullptr);
    extra.setProperty ("heroView", heroView.load(), nullptr);   // D2: hero view sticky request
    for (int k = 0; k < 6; ++k)   // stem mute/solo are plain atomics -> persist them by hand
    {
        extra.setProperty ("smute" + juce::String (k), stemMuted[(size_t) k].load(), nullptr);
        extra.setProperty ("ssolo" + juce::String (k), stemSoloed[(size_t) k].load(), nullptr);
    }
    for (int i = 0; i < 16; ++i)
    {
        extra.setProperty ("cue" + juce::String (i), cues[(size_t) i].load(), nullptr);
        extra.setProperty ("end" + juce::String (i), cueEnds[(size_t) i].load(), nullptr);
        extra.setProperty ("src" + juce::String (i), (int) padStemMask[(size_t) i].load(), nullptr);
    }
    state.appendChild (extra, nullptr);
    return state;
}

bool GentSamplerAudioProcessor::saveKit (const juce::File& kitFile)
{
    auto state = buildKitStateXml();
    if (auto xml = state.createXml())
        return xml->writeTo (kitFile);
    return false;
}

// KIT_SPEC.md PART B: worker-threaded .gentkit v2 save, fire-and-forget shape
// (requestSectionReport()/requestClassifyReport() precedent). kitSaveDest is
// stashed under infoLock BEFORE wantKitSave is set so doKitSaveJob() always
// sees a fully-written destination the first time it observes the flag.
void GentSamplerAudioProcessor::requestKitSave (const juce::File& dest)
{
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        kitSaveDest = dest;
    }
    wantKitSave = true;
    notify();
}

// KIT_SPEC.md PART C: FLAC-encode helper, refactored out of doKitSaveJob()'s
// former encodeFlac lambda so the stem-cache write (doStemJob) can share it —
// behavior-identical to the old lambda (24-bit, same writer logic), just with
// the destination file supplied by the caller instead of built from a
// tmpDir/stamp/tag triple internally.
//
// DATA_INTEGRITY_SPEC.md ADDENDUM T: the encode is now chunked (~1 second of
// samples per writeFromAudioSampleBuffer call) with a threadShouldExit() poll
// between chunks, instead of one whole-buffer write. This is called only from
// the worker thread (doKitSaveJob/doStemJob), and GentSamplerAudioProcessor
// IS a juce::Thread, so threadShouldExit() is callable directly. On an
// exit-request the partial file is left on disk and {} is returned exactly
// like any other encode failure — every existing caller already deletes its
// temps / partial cache dir on a {} return, so no new cleanup path is needed.
juce::File GentSamplerAudioProcessor::encodeFlacTo (const juce::File& dest, const juce::AudioBuffer<float>& buf,
                                                     double sr, juce::String& errOut)
{
    dest.deleteFile();

    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::FileOutputStream> os (dest.createOutputStream());
    if (os == nullptr) { errOut = "could not open " + dest.getFullPathName(); return {}; }

    std::unique_ptr<juce::AudioFormatWriter> writer (
        flac.createWriterFor (os.get(), sr, (unsigned int) juce::jmax (1, buf.getNumChannels()), 24, {}, 0));
    if (writer == nullptr) { errOut = "FLAC writer unavailable for " + dest.getFullPathName(); return {}; }
    os.release();   // writer owns the stream now

    const int total     = buf.getNumSamples();
    const int chunkLen  = juce::jlimit (1, juce::jmax (1, total), (int) sr);   // ~1 s per chunk, sane-clamped
    for (int offset = 0; offset < total; offset += chunkLen)
    {
        if (threadShouldExit())
        { errOut = "FLAC encode aborted (teardown) for " + dest.getFullPathName(); return {}; }

        const int n = juce::jmin (chunkLen, total - offset);
        if (! writer->writeFromAudioSampleBuffer (buf, offset, n))
        { errOut = "FLAC encode failed for " + dest.getFullPathName(); return {}; }
    }
    writer.reset();
    return dest;
}

// KIT_SPEC.md PART C: content-hash cache key for the disk stem cache. Chunked
// juce::SHA256 over the source buffer's raw float bytes: channel 0, then
// channel 1 if present, concatenated into one MemoryBlock and hashed once
// (SHA256 has no incremental/streaming API in this JUCE version — the
// simplest correct form is to build the block once rather than juggle two
// separate hashes). "_q" + stemQuality is appended so a quality-dial change
// invalidates the cached stems. Content-based (no file paths) so the key
// survives the source file being moved or renamed.
juce::String GentSamplerAudioProcessor::computeStemKey (const SourceSample::Ptr& src) const
{
    if (src == nullptr || src->buffer.getNumSamples() <= 0)
        return {};

    const auto& buf = src->buffer;
    const size_t bytesPerChan = (size_t) buf.getNumSamples() * sizeof (float);
    const int numChans = juce::jmin (2, buf.getNumChannels());

    // review fix: NO ensureSize() before append() — ensureSize SETS the block's
    // size (leaving those bytes uninitialized) and append() adds AFTER them, so
    // the hash would cover garbage + data => a DIFFERENT key every run => the
    // cache writes under one key and restores under another (permanent silent
    // miss). append() grows the block itself; start empty.
    juce::MemoryBlock mb;
    for (int ch = 0; ch < numChans; ++ch)
        mb.append (buf.getReadPointer (ch), bytesPerChan);

    juce::SHA256 hash (mb);
    return hash.toHexString() + "_q" + juce::String (stemQuality.load());
}

// KIT_SPEC.md PART B: .gentkit v2 = a ZIP containing kit.xml + source.flac +
// stem0..5.flac (iff stems exist at save time). Runs entirely on the worker
// thread: source/stems are taken via their refcounted Ptr getters (no lock
// held while FLAC-encoding, same discipline as doClassifyJob/doStemJob), FLAC
// encoding of minutes of audio is worker-only work. FLAC is 24-bit (float
// buffers quantize down — FLAC has no float mode; this is the pragmatic
// lossless-in-practice choice, same bit depth exportPad()/exportKit() already
// use for WAV). Completion is revealed via MessageManager::callAsync
// (doClassifyJob's writeAndReveal idiom); temp files are always cleaned up,
// even on a failure return.
void GentSamplerAudioProcessor::doKitSaveJob()
{
    juce::File dest;
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        dest = kitSaveDest;
    }

    auto src = getSource();
    if (src == nullptr)
    {
        DBG ("doKitSaveJob: no source loaded, nothing to save");
        return;
    }

    // ---- snapshot state + stems (refcounted Ptr, lock-free afterward) -----
    auto state = buildKitStateXml();
    StemSet::Ptr stems = getStems();

    // ---- temp files (deleted in every exit path) ---------------------------
    const auto tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::String stamp = juce::String::toHexString (juce::Random::getSystemRandom().nextInt64());
    juce::Array<juce::File> tempFiles;
    auto cleanup = [&tempFiles]
    {
        for (auto& f : tempFiles)
            f.deleteFile();
    };

    auto tempFlac = [&tempFiles, &tmpDir, &stamp] (const juce::String& tag) -> juce::File
    {
        const auto f = tmpDir.getChildFile ("GentSamplerKit_" + stamp + "_" + tag + ".flac");
        f.deleteFile();
        tempFiles.add (f);
        return f;
    };

    juce::String err;
    const auto sourceFlac = encodeFlacTo (tempFlac ("source"), src->buffer, src->sampleRate, err);
    if (sourceFlac == juce::File())
    {
        DBG ("doKitSaveJob: " << err);
        cleanup();
        return;
    }

    std::array<juce::File, 6> stemFlacs;
    const bool haveStems = (stems != nullptr);
    if (haveStems)
    {
        for (int i = 0; i < 6; ++i)
        {
            // ADDENDUM T: bail between stems on a teardown request, same
            // cleanup path as any other encode failure (temps deleted, dest
            // untouched — the atomic .tmp swap below never runs).
            if (threadShouldExit())
            {
                DBG ("doKitSaveJob: aborted (teardown) before stem " << i);
                cleanup();
                return;
            }
            auto& buf = stems->buffers[(size_t) i];
            if (buf.getNumSamples() == 0)
                continue;   // write only non-empty stem buffers
            juce::String stemErr;
            stemFlacs[(size_t) i] = encodeFlacTo (tempFlac ("stem" + juce::String (i)), buf, stems->sampleRate, stemErr);
            if (stemFlacs[(size_t) i] == juce::File())
            {
                DBG ("doKitSaveJob: " << stemErr);
                cleanup();
                return;
            }
        }
    }

    // kit.xml -> its own temp file (Builder::addFile needs an on-disk file)
    const auto xmlFile = tmpDir.getChildFile ("GentSamplerKit_" + stamp + "_kit.xml");
    tempFiles.add (xmlFile);
    {
        juce::ValueTree stateV2 = state;   // kitVer="2" on the root, everything else identical
        stateV2.setProperty ("kitVer", 2, nullptr);
        auto xml = stateV2.createXml();
        if (xml == nullptr || ! xml->writeTo (xmlFile))
        {
            DBG ("doKitSaveJob: could not write kit.xml");
            cleanup();
            return;
        }
    }

    // ---- zip it all up, write to a sibling temp file, then swap in --------
    juce::ZipFile::Builder zip;
    zip.addFile (xmlFile, 0, "kit.xml");         // XML compresses well but is tiny either way
    zip.addFile (sourceFlac, 0, "source.flac");  // FLAC is already compressed -> store, don't re-deflate
    if (haveStems)
        for (int i = 0; i < 6; ++i)
            if (stemFlacs[(size_t) i] != juce::File())
                zip.addFile (stemFlacs[(size_t) i], 0, "stem" + juce::String (i) + ".flac");

    const auto tmpDest = dest.getSiblingFile (dest.getFileName() + ".tmp");
    tmpDest.deleteFile();
    bool ok = false;
    {
        std::unique_ptr<juce::FileOutputStream> os (tmpDest.createOutputStream());
        if (os != nullptr)
            ok = zip.writeToStream (*os, nullptr);
    }

    cleanup();

    if (! ok)
    {
        DBG ("doKitSaveJob: ZIP write failed");
        tmpDest.deleteFile();
        return;
    }

    dest.deleteFile();
    if (! tmpDest.moveFileTo (dest))
    {
        DBG ("doKitSaveJob: could not move temp ZIP to destination");
        tmpDest.deleteFile();
        return;
    }

    juce::MessageManager::callAsync ([dest] { dest.revealToUser(); });
}

bool GentSamplerAudioProcessor::loadKit (const juce::File& kitFile)
{
    // KIT_SPEC.md PART B: sniff the file's first two bytes. "PK" -> v2 (ZIP)
    // path; anything else -> the EXISTING v1 path, byte-identical (old kits
    // keep loading).
    {
        juce::FileInputStream sniff (kitFile);
        char magic[2] = { 0, 0 };
        if (sniff.openedOk() && sniff.read (magic, 2) == 2 && magic[0] == 'P' && magic[1] == 'K')
            return loadKitV2 (kitFile);
    }

    auto xml = juce::XmlDocument::parse (kitFile);
    if (xml == nullptr)
        return false;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid())
        return false;
    applyStateTree (state);
    return true;
}

// KIT_SPEC.md PART B: .gentkit v2 load, synchronous on the message thread
// (matches today's loadKit() behavior — the async-restore item is a separate
// BACKLOG concern, not fixed here). Order: adoptSourceBuffer() FIRST (so the
// source exists before anything downstream references it), then stems if
// present, THEN applyStateTree() with "path" removed from its EXTRA child so
// applyStateTree's own loadFile(path) branch is skipped (v2 supplies audio
// directly, never touches/requires the stored path) — every other restore
// (cues, ends, masks, bpm, mode model, ...) goes through the normal
// applyStateTree code, unchanged.
bool GentSamplerAudioProcessor::loadKitV2 (const juce::File& kitFile)
{
    juce::ZipFile zip (kitFile);

    const int xmlIdx = zip.getIndexOfFileName ("kit.xml");
    if (xmlIdx < 0)
        return false;
    std::unique_ptr<juce::InputStream> xmlStream (zip.createStreamForEntry (xmlIdx));
    if (xmlStream == nullptr)
        return false;
    auto xml = juce::XmlDocument::parse (xmlStream->readEntireStreamAsString());
    if (xml == nullptr)
        return false;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid())
        return false;

    if (! loadKitV2Audio (kitFile, false))
        return false;

    // applyStateTree's "path" branch must be skipped (v2 never requires the
    // stored path to load) — everything else in EXTRA restores normally.
    auto extra = state.getChildWithName ("EXTRA");
    if (extra.isValid())
        extra.removeProperty ("path", nullptr);

    applyStateTree (state);
    return true;
}

// KIT_SPEC.md PART B (review fix): the AUDIO half of a v2 kit load — source
// + stems adopted, NO state applied. Two callers: loadKitV2() above (which
// applies the kit's own state afterwards), and loadFile()'s ZIP-sniff route
// (a HOST-PROJECT restore whose applyStateTree stored the .gentkit as the
// source path — there the PROJECT's state must stay in charge, so only the
// audio may be adopted).
bool GentSamplerAudioProcessor::loadKitV2Audio (const juce::File& kitFile, bool runAnalysis, bool keepCues)
{
    juce::ZipFile zip (kitFile);

    const int srcIdx = zip.getIndexOfFileName ("source.flac");
    if (srcIdx < 0)
        return false;

    juce::FlacAudioFormat flac;
    {
        std::unique_ptr<juce::AudioFormatReader> reader (
            flac.createReaderFor (zip.createStreamForEntry (srcIdx), true));
        if (reader == nullptr)
            return false;
        const auto maxLen = (juce::int64) (reader->sampleRate * 600.0);   // cap at 10 minutes (loadFile precedent)
        const int len = (int) juce::jmin (reader->lengthInSamples, maxLen);
        juce::AudioBuffer<float> buf ((int) reader->numChannels, len);
        reader->read (&buf, 0, len, 0, true, true);
        if (! adoptSourceBuffer (std::move (buf), reader->sampleRate, kitFile.getFullPathName(), runAnalysis, keepCues))
            return false;
    }

    // stems, iff present (mirrors doStemJob's stemSet write + doStemRenderJob
    // invalidation: set stemSet under stemLock, then request a stem re-render).
    bool anyStem = false;
    auto* set = new StemSet();
    for (int i = 0; i < 6; ++i)
    {
        const juce::String name = "stem" + juce::String (i) + ".flac";
        const int idx = zip.getIndexOfFileName (name);
        if (idx < 0)
            continue;
        std::unique_ptr<juce::AudioFormatReader> reader (
            flac.createReaderFor (zip.createStreamForEntry (idx), true));
        if (reader == nullptr)
            continue;
        const auto maxLen = (juce::int64) (reader->sampleRate * 600.0);   // cap at 10 minutes (source.flac precedent above)
        const int len = (int) juce::jmin (reader->lengthInSamples, maxLen);
        set->buffers[(size_t) i].setSize ((int) reader->numChannels, len);
        reader->read (&set->buffers[(size_t) i], 0, len, 0, true, true);
        set->sampleRate = reader->sampleRate;
        anyStem = true;
    }
    adoptStemSet (set, anyStem);

    return true;
}

// DATA_INTEGRITY_SPEC.md Change 2: worker-thread decode for the async restore
// path. applyStateTree() has already validated the stored path (not our own
// temp export, exists) and written the restored cues/masks/params; this job's
// ONLY responsibility is to decode the source (and, for a v2 kit, its stems)
// WITHOUT touching those already-restored cues (keepCues=true throughout) —
// and to never adopt a blank/invalid source (the ghost's other door: reader
// null or decoded length <= 0 both bail out with no adoptSourceBuffer call).
void GentSamplerAudioProcessor::doRestoreLoadJob()
{
    juce::File f;
    int genAtStash = 0;
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        f = restoreLoadPath;
        genAtStash = restoreLoadGenAtStash;
    }
    if (f == juce::File())
        return;

    // PK-sniff, loadFile()'s exact idiom: a host-project's stored path may be
    // a .gentkit (v2 kit's own audio, project state stays in charge — same
    // split as loadFile()'s ZIP-sniff route, just off the message thread now).
    {
        juce::FileInputStream sniff (f);
        char magic[2] = { 0, 0 };
        if (sniff.openedOk() && sniff.read (magic, 2) == 2 && magic[0] == 'P' && magic[1] == 'K')
        {
            if (! loadKitV2Audio (f, /*runAnalysis*/ false, /*keepCues*/ true))
                DBG ("doRestoreLoadJob: loadKitV2Audio failed for " << f.getFullPathName());
            return;
        }
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr)
    {
        DBG ("doRestoreLoadJob: no reader for " << f.getFullPathName());
        return;
    }

    const auto maxLen = (juce::int64) (reader->sampleRate * 600.0);   // cap at 10 minutes (loadFile precedent)
    const int len = (int) juce::jmin (reader->lengthInSamples, maxLen);
    if (len <= 0)
    {
        DBG ("doRestoreLoadJob: decoded length <= 0 for " << f.getFullPathName() << " -- not adopting");
        return;
    }

    // ADDENDUM T: read in ~1-second chunks with a threadShouldExit() poll
    // between reads, so a teardown request mid-decode returns promptly
    // instead of blocking on a single whole-file read. On abort we return
    // WITHOUT adopting -- a partially-filled buffer must never reach
    // adoptSourceBuffer().
    juce::AudioBuffer<float> buf ((int) reader->numChannels, len);
    const int chunkLen = juce::jlimit (1, juce::jmax (1, len), (int) reader->sampleRate);
    for (int offset = 0; offset < len; offset += chunkLen)
    {
        if (threadShouldExit())
        {
            DBG ("doRestoreLoadJob: aborted (teardown) mid-decode for " << f.getFullPathName());
            return;
        }
        const int n = juce::jmin (chunkLen, len - offset);
        reader->read (&buf, offset, n, offset, true, true);
    }

    // WAVE1_SPEC.md F2 (audit #5): the decode above is long; re-check right
    // before adopting that no newer, unrelated restore landed while we were
    // decoding (doAnalysisJob's gen-guard pattern). A stale decode is
    // discarded here rather than adopted under a different restore's cues.
    if (restoreGen.load() != genAtStash)
    {
        DBG ("doRestoreLoadJob: restoreGen changed mid-decode for " << f.getFullPathName() << " -- discarding stale decode");
        return;
    }

    adoptSourceBuffer (std::move (buf), reader->sampleRate, f.getFullPathName(),
                        /*runAnalysis*/ false, /*keepCues*/ true);
    wantRender = true;
    notify();
}

// KIT_SPEC.md PART C: shared stem-adoption tail, factored out of
// loadKitV2Audio()'s stem block so doStemCacheLoadJob() doesn't duplicate the
// stemLock/generation/wantStemRender logic. Takes ownership of `set`: deletes
// it if `anyStem` is false (nothing was actually decoded), else bumps
// stemGeneration, publishes it under stemLock, and requests a stem re-render —
// exactly doStemJob's own stemSet write + doStemRenderJob invalidation.
void GentSamplerAudioProcessor::adoptStemSet (StemSet* set, bool anyStem)
{
    if (anyStem)
    {
        set->generation = ++stemGeneration;
        {
            const juce::SpinLock::ScopedLockType sl (stemLock);
            stemSet = set;
        }
        wantStemRender = true;
        notify();
    }
    else
    {
        delete set;
    }
}

// KIT_SPEC.md PART C: worker-thread stem-cache restore. Reads stemCacheKey
// (set by applyStateTree()'s "stemKey" restore) and decodes
// Documents\GentSampler\stemcache\<key>\stemN.flac into a StemSet exactly
// like loadKitV2Audio's stem block, then shares adoptStemSet() for the
// generation/stemLock/wantStemRender tail (no duplicated logic). A missing
// cache dir (user deleted it, or the key never got a successful write) is a
// silent degrade to today's inert-mask behavior — one DBG line, no error UI.
void GentSamplerAudioProcessor::doStemCacheLoadJob()
{
    // PREPACKAGE_AUDIT.md #9 (WAVE2_SPEC.md, part 3 of 3): snapshot the key
    // this job started with, plus restoreGen (doAnalysisJob parity), so a
    // direct load or a fresh restore that lands WHILE this job is decoding
    // can be detected before the result is published.
    juce::String key;
    const int genAtEntry = restoreGen.load();
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        key = stemCacheKey;
    }
    if (key.isEmpty())
        return;

    const auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                        .getChildFile ("GentSampler").getChildFile ("stemcache").getChildFile (key);
    if (! dir.isDirectory())
    {
        DBG ("doStemCacheLoadJob: stem cache miss for " << key);
        return;
    }

    juce::FlacAudioFormat flac;
    bool anyStem = false;
    auto* set = new StemSet();
    for (int i = 0; i < 6; ++i)
    {
        const auto f = dir.getChildFile ("stem" + juce::String (i) + ".flac");
        if (! f.existsAsFile())
            continue;
        std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (f.createInputStream().release(), true));
        if (reader == nullptr)
            continue;
        const auto maxLen = (juce::int64) (reader->sampleRate * 600.0);   // cap at 10 minutes (loadFile precedent)
        const int len = (int) juce::jmin (reader->lengthInSamples, maxLen);
        set->buffers[(size_t) i].setSize ((int) reader->numChannels, len);
        reader->read (&set->buffers[(size_t) i], 0, len, 0, true, true);
        set->sampleRate = reader->sampleRate;
        anyStem = true;
    }

    if (! anyStem)
        DBG ("doStemCacheLoadJob: stem cache dir present but no stemN.flac decoded for " << key);

    // Gen guard (uniformity with doAnalysisJob/doSectionApplyJob): a fresh
    // restore landed mid-decode — that restore's own applyStateTree() has
    // already cleared/re-derived stemCacheKey/wantStemCacheLoad for itself,
    // so this stale job must not publish over it.
    if (restoreGen.load() != genAtEntry)
    {
        DBG ("doStemCacheLoadJob: bail, restoreGen changed mid-job for " << key);
        delete set;
        return;
    }

    // Key re-check (the LOAD-BEARING guard): immediately before publishing,
    // re-read stemCacheKey under infoLock. If a direct load or a new restore
    // cleared it (or swapped in a different key) while this job was decoding,
    // this result belongs to a superseded request — discard it rather than
    // attach it to whatever is now current.
    {
        juce::String keyNow;
        {
            const juce::SpinLock::ScopedLockType sl (infoLock);
            keyNow = stemCacheKey;
        }
        if (keyNow.isEmpty() || keyNow != key)
        {
            DBG ("doStemCacheLoadJob: bail, stemCacheKey changed/cleared mid-job (was "
                 << key << ", now " << keyNow << ")");
            delete set;
            return;
        }
    }

    adoptStemSet (set, anyStem);
}

void GentSamplerAudioProcessor::applyStateTree (const juce::ValueTree& state)
{
    apvts.replaceState (state);
    // DATA_INTEGRITY_SPEC.md Change 1: bump restore authority FIRST, before any
    // other work, and cancel every pending derive-then-apply intent — restore
    // is authoritative: whatever slicing was queued belongs to the pre-restore
    // world.
    ++restoreGen;
    analysisThenSlice = false;
    kitPending = false;
    analysisKeepCues = true;

    // WAVE1_SPEC.md F2 (audit #5): clear ANY previously-stashed pending load at
    // the top of every restore's path handling — including the empty/refused/
    // missing-path branches below — so a restore with no source path can never
    // inherit a predecessor restore's still-pending doRestoreLoadJob() request.
    // The valid-path branch below re-stashes (path + gen) as needed.
    // PREPACKAGE_AUDIT.md #9 (WAVE2_SPEC.md, part 1 of 3): same reasoning
    // applies to a pending STEM-CACHE load — a prior restore's stemCacheKey/
    // wantStemCacheLoad must not survive into a new restore (which may have
    // no cache key of its own, or a different one); this restore's own
    // key/flag are re-derived from THIS state further below if applicable.
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        restoreLoadPath = juce::File();
        stemCacheKey.clear();
    }
    wantRestoreLoad = false;
    wantStemCacheLoad = false;

    auto extra = state.getChildWithName ("EXTRA");
    if (! extra.isValid())
        return;

    // DATA_INTEGRITY_SPEC.md Change 2: async, validated restore decode. The
    // message thread does ZERO decoding now — it only validates the stored
    // path and hands it to the worker (doRestoreLoadJob) via restoreLoadPath/
    // wantRestoreLoad.
    const juce::String path = extra.getProperty ("path", "");
    if (path.isNotEmpty())
    {
        if (gent::pathIsWithin (path.toStdString(),
                                 juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getFullPathName().toStdString()))
        {
            DBG ("applyStateTree: refusing our own temp export " << path);
        }
        else if (! juce::File (path).existsAsFile())
        {
            // today's behavior: missing file -> skip, rest of restore proceeds
        }
        else
        {
            // Save-before-decode correctness: write the display path now, under
            // infoLock, so a save that happens before the worker's decode lands
            // still persists the right path (getStateInformation reads filePath
            // under infoLock).
            const juce::File f (path);
            {
                const juce::SpinLock::ScopedLockType sl (infoLock);
                fileName = f.getFileName();
                filePath = path;
                restoreLoadPath = f;
                restoreLoadGenAtStash = restoreGen.load();
            }
            wantRestoreLoad = true;
        }
    }

    for (int i = 0; i < 16; ++i)
    {
        cues[(size_t) i] = (int) extra.getProperty ("cue" + juce::String (i), cues[(size_t) i].load());
        cueEnds[(size_t) i] = (int) extra.getProperty ("end" + juce::String (i), -1);
        padStemMask[(size_t) i] = gent::sanitizeStemMask ((int) extra.getProperty ("src" + juce::String (i), 0));
    }

    detectedBpm = (double) extra.getProperty ("bpm", 0.0);
    bpmOverride = (double) extra.getProperty ("ubpm", 0.0);
    editorW = (int) extra.getProperty ("edW", 900);
    editorH = (int) extra.getProperty ("edH", 640);

    // PREPACKAGE_AUDIT.md #10: these nine fallbacks used to be the LIVE
    // in-memory atomic (e.g. "stemQ", (int) stemQuality.load()) — for a key
    // absent from an older kit/project XML, that silently carried over
    // whatever a PREVIOUS applyStateTree() call on this same processor
    // instance left behind, instead of landing on the field's declared
    // default. Named constants below match PluginProcessor.h's initializers
    // EXACTLY (sliceGridDiv{2}, sliceSensitivity{1}, sliceSnap{1},
    // sliceModeSel{0}, sectionBars{4}, sectionSens{1}, gridEvenSel{3},
    // kitSens{1}, stemQuality{1}) so a missing key always lands on the
    // documented default, regardless of prior session state.
    constexpr int kDefaultStemQuality      = 1;   // FAST/HQ/MAX — HQ default
    constexpr int kDefaultSliceGridDiv     = 2;   // 0 Bar,1 Beat,2 1/8,3 1/16
    constexpr int kDefaultSliceSensitivity = 1;   // 0 Low,1 Med,2 High
    constexpr int kDefaultSliceSnap        = 1;   // 0 Loose,1 Med,2 Tight
    constexpr int kDefaultSliceModeSel     = 0;   // 0 SECTIONS-BARS (default)
    constexpr int kDefaultSectionBars      = 4;   // {1,2,4,8}
    constexpr int kDefaultSectionSens      = 1;   // 0 few,1 medium,2 many
    constexpr int kDefaultGridEvenSel      = 3;   // 0 16-equal,1 beat,2 2-beats,3 every bar
    constexpr int kDefaultKitSens          = 1;   // 0 few,1 medium,2 many

    setStemQuality ((int) extra.getProperty ("stemQ", kDefaultStemQuality));   // clamped 0..2
    setSliceGridDiv     ((int) extra.getProperty ("slG", kDefaultSliceGridDiv));
    setSliceSensitivity ((int) extra.getProperty ("slS", kDefaultSliceSensitivity));
    setSliceSnap        ((int) extra.getProperty ("slP", kDefaultSliceSnap));
    // SECTIONS_SPEC.md PART 3: SLICE split-chip mode model — defaults preserved
    // for old projects/kits without these keys (getProperty fallback).
    setSliceModeSel ((int) extra.getProperty ("slMode", kDefaultSliceModeSel));
    setSectionBars  ((int) extra.getProperty ("slBars", kDefaultSectionBars));
    setSectionSens  ((int) extra.getProperty ("slSens", kDefaultSectionSens));
    setGridEvenSel  ((int) extra.getProperty ("slEven", kDefaultGridEvenSel));
    setKitSens      ((int) extra.getProperty ("slKSens", kDefaultKitSens));
    snapEnabled = (bool) extra.getProperty ("snap", snapEnabled.load());
    velToLevel  = (bool) extra.getProperty ("vel2l", true);
    // D2: restore hero view sticky request through the sanitizer (garbage-stored-int
    // defense, docs/STEM_VIEW_MODEL.md SS4); old kits/projects without the key default
    // to 0 (COMPOSITE) via getProperty's fallback, same pattern as the smute/ssolo
    // "default audible for older presets" restore just below.
    heroView = gent::sanitizeHeroView ((int) extra.getProperty ("heroView", 0));
    for (int k = 0; k < 6; ++k)   // restore stem mute/solo (default audible for older presets)
    {
        stemMuted[(size_t) k]  = (bool) extra.getProperty ("smute" + juce::String (k), false);
        stemSoloed[(size_t) k] = (bool) extra.getProperty ("ssolo" + juce::String (k), false);
    }
    juce::String restoredStemKey;
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        detectedKey = extra.getProperty ("key", "").toString();
        // KIT_SPEC.md PART C: read the persisted stem-cache key back; stash it
        // under infoLock now (stemCacheKey) so doStemCacheLoadJob() can read it
        // once it runs on the worker. Keep a local copy too for the emptiness
        // check just below (avoids re-locking).
        restoredStemKey = extra.getProperty ("stemKey", "").toString();
        stemCacheKey = restoredStemKey;
    }

    // KIT_SPEC.md PART C restore: a host-project reopen (or an old .gentkit
    // that never carried stems) may have a cache key but no stems loaded yet —
    // defer the FLAC decode to the worker (kitPending/wantAnalysis precedent).
    // DATA_INTEGRITY_SPEC.md Change 2: a v2 kit's own embedded stems are
    // adopted synchronously by loadKitV2()/loadKitV2Audio() BEFORE this
    // applyStateTree call (see loadKitV2's definition comment), so hasStems()
    // here still correctly reflects that load and must NOT be clobbered by the
    // cache. A host-project path restore (the wantRestoreLoad branch above) is
    // async and has NOT adopted anything yet at this point — hasStems() is
    // false in that case, same effective ordering as before this change.
    if (restoredStemKey.isNotEmpty() && ! hasStems())
        wantStemCacheLoad = true;

    wantRender = true;
    notify();
    ++uiDirty;
}

// ============================================================================
//  processBlock
// ============================================================================

void GentSamplerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (pos->getBpm().hasValue())
                hostBpm.store (*pos->getBpm());

    {
        const double target = currentTargetSpeed();
        if (std::abs (target - lastRenderSpeed.load()) > 0.002
            || std::abs ((double) pMasterPitch->load() - lastRenderPitch.load()) > 0.01)
            requestRender();
    }

    // PREPACKAGE_AUDIT.md #11: each swap below stashes the OUTGOING Ptr into
    // the graveyard ring (inside the SAME try-locked scope as the swap
    // itself, per WAVE2_SPEC.md Q5.2) before reassigning, so the old
    // AudioBuffer's free happens later on the message thread
    // (handleAsyncUpdate's drain loop), never synchronously here. If the
    // ring is full, the swap is SKIPPED ENTIRELY this block (keep playing
    // the old render — the worker's own Ptr still references the new one,
    // so the swap simply retries next block; clean backpressure, never an
    // inline free, never a block, never a drop).
    bool stashedThisBlock = false;
    {
        const juce::SpinLock::ScopedTryLockType tl (rendLock);
        if (tl.isLocked())
        {
            const std::uint32_t w = graveW.load (std::memory_order_relaxed);
            if (w - graveR.load (std::memory_order_acquire) < (std::uint32_t) graveyard.size())
            {
                graveyard[(size_t) (w & 63u)] = active;   // 64-slot ring, wrap-safe
                graveW.store (w + 1u, std::memory_order_release);
                active = rendered;
                stashedThisBlock = true;
            }
        }
    }
    {
        const juce::SpinLock::ScopedTryLockType tl (stemRendLock);
        if (tl.isLocked())
        {
            const std::uint32_t w = graveW.load (std::memory_order_relaxed);
            if (w - graveR.load (std::memory_order_acquire) < (std::uint32_t) graveyard.size())
            {
                graveyard[(size_t) (w & 63u)] = activeStems;   // 64-slot ring, wrap-safe
                graveW.store (w + 1u, std::memory_order_release);
                activeStems = renderedStems;
                stashedThisBlock = true;
            }
        }
    }
    {
        const juce::SpinLock::ScopedTryLockType tl (padLock);
        if (tl.isLocked())
        {
            for (int i = 0; i < 16; ++i)
            {
                const std::uint32_t w = graveW.load (std::memory_order_relaxed);
                if (w - graveR.load (std::memory_order_acquire) < (std::uint32_t) graveyard.size())
                {
                    graveyard[(size_t) (w & 63u)] = activePads[(size_t) i];   // 64-slot ring, wrap-safe
                    graveW.store (w + 1u, std::memory_order_release);
                    activePads[(size_t) i] = padRenders[(size_t) i];
                    stashedThisBlock = true;
                }
                // ring full -> skip THIS pad's swap this block only; the other
                // 15 pads still get a chance (independent per-pad backpressure).
            }
        }
    }
    if (stashedThisBlock)
        triggerAsyncUpdate();

    {
        int s1, n1, s2, n2;
        uiFifo.prepareToRead (uiFifo.getNumReady(), s1, n1, s2, n2);
        for (int i = 0; i < n1; ++i)
        {
            const auto e = uiEvents[(size_t) (s1 + i)];
            e.on ? startVoice (e.pad, 1.0f, 0, -1, false)
                 : releaseVoices (e.pad, -1, false, true);
        }
        for (int i = 0; i < n2; ++i)
        {
            const auto e = uiEvents[(size_t) (s2 + i)];
            e.on ? startVoice (e.pad, 1.0f, 0, -1, false)
                 : releaseVoices (e.pad, -1, false, true);
        }
        uiFifo.finishedRead (n1 + n2);
    }

    {
        const int ap = auditionPad.exchange (-1);
        if (ap >= 0)
            startVoice (ap, 1.0f, 0, -1, false);
    }

    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn())
            handleNoteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())
            handleNoteOff (m.getNoteNumber());
    }
    midi.clear();

    const int n = buffer.getNumSamples();

    if (capturing.load())
        capSamples += (juce::int64) n;

    auto r = active;
    if (r == nullptr || r->buffer.getNumSamples() < 2)
    {
        previewingA = false;
        previewPlayPos = -1;
        return;
    }

    const auto& rb = r->buffer;
    const float* inL = rb.getReadPointer (0);
    const float* inR = rb.getNumChannels() > 1 ? rb.getReadPointer (1) : inL;
    const int len = rb.getNumSamples();

    // main-bus pointers (fallback destination)
    auto mainBus = getBusBuffer (buffer, false, 0);
    float* mainL = mainBus.getWritePointer (0);
    float* mainR = mainBus.getNumChannels() > 1 ? mainBus.getWritePointer (1) : mainL;

    // ---- resolve global stem mute/solo into per-stem audibility (audio thread) ----
    bool globalAudible[6];
    bool anySolo = false, anyMute = false;
    for (int k = 0; k < 6; ++k)
    {
        if (stemSoloed[(size_t) k].load()) anySolo = true;
        if (stemMuted[(size_t) k].load())  anyMute = true;
    }
    for (int k = 0; k < 6; ++k)
        globalAudible[k] = anySolo ? stemSoloed[(size_t) k].load()
                                   : ! stemMuted[(size_t) k].load();
    // divert to the stem path when global filtering OR a pad's own source is active
    RenderedStems::Ptr asPtr = activeStems;
    const bool stemFilterActive = (anySolo || anyMute) && (asPtr != nullptr);
    // per-sample step that ramps a stem gain 0<->1 in ~6 ms: instant but click-free
    const float stemGainStep = (float) (1.0 / juce::jmax (1.0, 0.006 * getSampleRate()));

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        // resolve this voice's source buffer (master render or per-pad render)
        const float* vinL = inL;
        const float* vinR = inR;
        int vlen = len;
        const PadRender* padPtr = nullptr;   // this voice's per-pad render (srcKind 1)
        if (v.srcKind == 1)
        {
            auto& pr = activePads[(size_t) juce::jlimit (0, 15, v.pad)];
            if (pr == nullptr || pr->generation != v.gen || pr->buffer.getNumSamples() < 2)
            {
                v.active = false;
                continue;
            }
            vinL = pr->buffer.getReadPointer (0);
            vinR = pr->buffer.getNumChannels() > 1 ? pr->buffer.getReadPointer (1) : vinL;
            vlen = pr->buffer.getNumSamples();
            padPtr = pr.get();
        }
        else if (v.gen != r->generation)
        {
            v.active = false;
            continue;
        }

        // route to this pad's aux bus when the host has enabled it
        float* outL = mainL;
        float* outR = mainR;
        const int busIdx = v.pad + 1;
        if (busIdx >= 1 && busIdx < getBusCount (false))
        {
            if (auto* bus = getBus (false, busIdx))
            {
                if (bus->isEnabled())
                {
                    auto bb = getBusBuffer (buffer, false, busIdx);
                    if (bb.getNumChannels() > 0)
                    {
                        outL = bb.getWritePointer (0);
                        outR = bb.getNumChannels() > 1 ? bb.getWritePointer (1) : outL;
                    }
                }
            }
        }

        // playable region bounds; reverse walks hiEdge -> loEdge, loop wraps
        const double loEdge = juce::jmax (0.0, v.loopStart);
        const double hiEdge = juce::jmin (v.endPos, (double) (vlen - 1));
        const double stepMag = v.rate;
        double pos = juce::jlimit (loEdge, hiEdge, v.pos);

        // per-voice stem gains: combine global mute/solo with this pad's source mask.
        // vStemGain[] is the live TARGET (re-read every block -> instant response);
        // v.sg[] ramps toward it per sample so a mute/solo/source flip is click-free.
        const float* stemL[6] = {};
        const float* stemR[6] = {};
        float vStemGain[6];
        const std::uint8_t pmask = padStemMask[(size_t) juce::jlimit (0, 15, v.pad)].load();
        const bool padFiltered = (pmask != 0);
        const float bleedParam = pBleed[(size_t) juce::jlimit (0, 15, v.pad)]->load();
        for (int k = 0; k < 6; ++k)
            vStemGain[k] = gent::stemGainFor (pmask, k, bleedParam, globalAudible[k]);
        const bool wantStems = (stemFilterActive || padFiltered);
        // keep mixing stems while a gain is still ramping back to "full", so an
        // un-mute fades in cleanly before we drop back to the cheap master path.
        bool ramping = false;
        if (v.sgInit)
            for (int k = 0; k < 6; ++k)
                if (std::abs (v.sg[k] - vStemGain[k]) > 1.0e-4f) { ramping = true; break; }

        // stem source: this pad's own stretched stems (srcKind 1 = speed-toggled pad),
        // else the master render's global stems (srcKind 0 = normal, srcKind 2 = granular
        // — granular reads the master domain, so it shares the global stems).
        const bool haveStemSrc = (v.srcKind == 1)
            ? (padPtr != nullptr && padPtr->hasStems && padPtr->stems[0].getNumSamples() == vlen)
            : (asPtr != nullptr && asPtr->buffers[0].getNumSamples() == vlen);

        bool useStems = false;
        if (haveStemSrc && (wantStems || ramping))
        {
            useStems = true;
            for (int k = 0; k < 6; ++k)
            {
                const auto& b = (v.srcKind == 1) ? padPtr->stems[(size_t) k]
                                                 : asPtr->buffers[(size_t) k];
                if (b.getNumSamples() == vlen)
                {
                    stemL[k] = b.getReadPointer (0);
                    stemR[k] = b.getNumChannels() > 1 ? b.getReadPointer (1) : stemL[k];
                }
            }
            if (! v.sgInit)   // voice that starts already filtered: snap, don't fade in
            {
                for (int k = 0; k < 6; ++k) v.sg[k] = vStemGain[k];
                v.sgInit = true;
            }
        }
        else
        {
            // not mixing stems this block -> keep sg[] coherent so the next flip
            // ramps from the correct (full) state, not a stale one.
            for (int k = 0; k < 6; ++k) v.sg[k] = vStemGain[k];
            v.sgInit = true;
        }

        // per-voice filter (TPT state-variable); params read per block -> live sweeps
        const int  ftype  = (int) pFType[(size_t) v.pad]->load();   // 0 Off,1 LP,2 HP,3 BP
        const bool filtOn = (ftype != 0);
        float fK = 0.0f, fA1 = 0.0f, fA2 = 0.0f, fA3 = 0.0f;
        if (filtOn)
        {
            const float fsf  = (float) getSampleRate();
            const float cut  = juce::jlimit (20.0f, 0.49f * fsf, pCutoff[(size_t) v.pad]->load());
            const float reso = juce::jlimit (0.0f, 1.0f, pReso[(size_t) v.pad]->load());
            const float gT   = std::tan (juce::MathConstants<float>::pi * cut / fsf);
            const float Q    = juce::jmap (reso, 0.0f, 1.0f, 0.5f, 8.0f);   // 0.5 .. 8
            fK  = 1.0f / Q;
            fA1 = 1.0f / (1.0f + gT * (gT + fK));
            fA2 = gT * fA1;
            fA3 = gT * fA2;
        }

        // ---- per-pad granular (srcKind 2) block setup ----
        const bool granular = (v.srcKind == 2);
        bool   granFrozen = false;
        double spawnInterval = 1.0e18;
        float  gNorm = 1.0f, gPitchRate = 1.0f, gSpray = 0.0f;
        int    gLen = 1, loI = 0, hiI = juce::jmax (0, vlen - 2);
        double frozenCenter = loEdge;
        if (granular)
        {
            const int    pi  = juce::jlimit (0, 15, v.pad);
            // GRAIN switched off mid-note: a voice can't morph srcKind back to normal
            // playback cleanly, so fade it out — toggling GRAIN off audibly reverts the
            // pad (and releases any stuck freeze drone) instead of leaving it ringing.
            if (pGrainOn[(size_t) pi]->load() <= 0.5f && v.state != 2)
                v.state = 2;
            const double fs2 = getSampleRate();
            gLen = juce::jmax (4, (int) (pGrainSize[(size_t) pi]->load() * 0.001 * fs2));
            const float overlap = juce::jmap (juce::jlimit (0.0f, 1.0f, pGrainDens[(size_t) pi]->load()), 1.0f, 8.0f);
            spawnInterval = juce::jmax (1.0, (double) gLen / (double) overlap);
            gNorm  = 1.0f / std::sqrt (juce::jmax (1.0f, overlap));
            gPitchRate = (float) v.grainBaseRate * std::pow (2.0f, pGrainPitch[(size_t) pi]->load() / 12.0f);
            gSpray = juce::jlimit (0.0f, 1.0f, pGrainSpray[(size_t) pi]->load());
            granFrozen = pGrainFreeze[(size_t) pi]->load() > 0.5f;
            loI = juce::jlimit (0, juce::jmax (0, vlen - 2), (int) loEdge);
            hiI = juce::jlimit (loI, juce::jmax (loI, vlen - 2), (int) hiEdge);
            frozenCenter = loEdge + juce::jlimit (0.0f, 1.0f, pGrainPos[(size_t) pi]->load()) * (hiEdge - loEdge);
        }
        const int maxFrozenSamples = (int) (20.0 * getSampleRate());

        for (int i = 0; i < n; ++i)
        {
            if (! granFrozen)   // frozen granular holds the playhead -> no boundary/end
            {
                if (! v.reverse)
                {
                    if (pos >= hiEdge)
                    {
                        if (v.loop) pos = loEdge;            // wrap to region start
                        else { v.active = false; break; }
                    }
                }
                else if (pos <= loEdge)
                {
                    if (v.loop) pos = hiEdge;                // wrap to region end
                    else { v.active = false; break; }
                }
            }

            const int   i0 = juce::jlimit (0, juce::jmax (0, vlen - 2), (int) pos);
            const float fr = (float) (pos - (double) i0);
            float sl, sr;
            if (granular)
            {
                // spawn on the density schedule (1 grain/sample max; skip if pool full)
                v.grainSpawnAcc += 1.0;
                if (v.grainSpawnAcc >= spawnInterval)
                {
                    v.grainSpawnAcc -= spawnInterval;
                    v.grainRng ^= v.grainRng << 13; v.grainRng ^= v.grainRng >> 17; v.grainRng ^= v.grainRng << 5;
                    const float  r01 = (float) (v.grainRng & 0xffffffu) / 16777216.0f;
                    const double center = granFrozen ? frozenCenter : pos;
                    const double jit = (double) (r01 * 2.0f - 1.0f) * gSpray * (hiEdge - loEdge) * 0.5;
                    const double start = juce::jlimit ((double) loI, (double) hiI, center + jit);
                    for (auto& gr : v.grains)
                        if (! gr.active) { gr.active = true; gr.readPos = start; gr.rate = gPitchRate;
                                           gr.age = 0; gr.length = gLen; break; }
                }
                // grains read this pad's selected stem mix (+ bleed) when stems are
                // active, else the master render — so granular honors PAD SOURCE and
                // BLEED just like normal playback. Ramp the per-stem gains once per
                // sample (shared by every grain this sample) so a source flip stays
                // click-free; the non-granular stem branch below is never reached here.
                if (useStems)
                    for (int k = 0; k < 6; ++k)
                    {
                        const float t  = vStemGain[k];
                        const float gk = v.sg[k];
                        v.sg[k] = (gk < t) ? juce::jmin (t, gk + stemGainStep)
                                           : juce::jmax (t, gk - stemGainStep);
                    }
                // sum active grains (Hann-windowed, linear-interp reads within the slice)
                sl = sr = 0.0f;
                for (auto& gr : v.grains)
                {
                    if (! gr.active) continue;
                    const float w  = hannWin ((float) gr.age / (float) gr.length);
                    const int   gi = juce::jlimit (loI, hiI, (int) gr.readPos);
                    const int   g1 = juce::jmin (vlen - 1, gi + 1);   // buffer-safe upper interp index
                    const float gf = (float) (gr.readPos - (double) gi);
                    if (useStems)
                    {
                        float aL = 0.0f, aR = 0.0f, bL = 0.0f, bR = 0.0f;
                        for (int k = 0; k < 6; ++k)
                        {
                            const float gk = v.sg[k];
                            if (gk <= 0.0f || stemL[k] == nullptr) continue;
                            aL += gk * stemL[k][gi]; aR += gk * stemR[k][gi];
                            bL += gk * stemL[k][g1]; bR += gk * stemR[k][g1];
                        }
                        sl += w * (aL + gf * (bL - aL));
                        sr += w * (aR + gf * (bR - aR));
                    }
                    else
                    {
                        sl += w * (vinL[gi] + gf * (vinL[g1] - vinL[gi]));
                        sr += w * (vinR[gi] + gf * (vinR[g1] - vinR[gi]));
                    }
                    gr.readPos += gr.rate;
                    if (++gr.age >= gr.length) gr.active = false;
                }
                sl *= gNorm; sr *= gNorm;
            }
            else if (useStems)
            {
                sl = sr = 0.0f;
                for (int k = 0; k < 6; ++k)
                {
                    // glide the per-stem gain toward its target (click-free toggle)
                    const float t  = vStemGain[k];
                    float       gk = v.sg[k];
                    if (gk < t) gk = juce::jmin (t, gk + stemGainStep);
                    else        gk = juce::jmax (t, gk - stemGainStep);
                    v.sg[k] = gk;
                    if (gk <= 0.0f || stemL[k] == nullptr) continue;
                    sl += gk * (stemL[k][i0] + fr * (stemL[k][i0 + 1] - stemL[k][i0]));
                    sr += gk * (stemR[k][i0] + fr * (stemR[k][i0 + 1] - stemR[k][i0]));
                }
            }
            else
            {
                sl = vinL[i0] + fr * (vinL[i0 + 1] - vinL[i0]);
                sr = vinR[i0] + fr * (vinR[i0 + 1] - vinR[i0]);
            }

            // SP-1200-style crush: sample-rate hold + bit quantize
            if (v.crush > 0.001f)
            {
                if (v.holdCount <= 0)
                {
                    const float q = v.crushQ;   // precomputed at trigger (see startVoice)
                    v.heldL = std::round (sl * q) / q;
                    v.heldR = std::round (sr * q) / q;
                    v.holdCount = v.holdN;
                }
                sl = v.heldL; sr = v.heldR;
                --v.holdCount;
            }

            // per-pad multimode filter (Zavalishin TPT SVF), post-crush / pre-gain
            if (filtOn)
            {
                auto svf = [&] (float x, float& ic1, float& ic2)
                {
                    const float v3 = x - ic2;
                    const float v1 = fA1 * ic1 + fA2 * v3;
                    const float v2 = ic2 + fA2 * ic1 + fA3 * v3;
                    ic1 = 2.0f * v1 - ic1;
                    ic2 = 2.0f * v2 - ic2;
                    if (ftype == 1) return v2;                  // low-pass
                    if (ftype == 2) return x - fK * v1 - v2;    // high-pass
                    return v1;                                  // band-pass
                };
                sl = svf (sl, v.fic1L, v.fic2L);
                sr = svf (sr, v.fic1R, v.fic2R);
            }

            if (v.state == 0)
            {
                v.env += v.attInc;
                if (v.env >= 1.0f) { v.env = 1.0f; v.state = 1; }
            }
            else if (v.state == 2)
            {
                v.env -= v.relDec;
                if (v.env <= 0.0f) { v.active = false; break; }
            }

            const float g = v.env * v.level;
            outL[i] += sl * g * v.panL;
            outR[i] += sr * g * v.panR;
            if (! granFrozen)
                pos += v.reverse ? -stepMag : stepMag;
            else if (! v.gate && v.state != 2 && ++v.grainAge > maxFrozenSamples)
                v.state = 2;                  // frozen non-gate voice has no key-up to free it — cap it (~20s)
        }
        v.pos = pos;
    }

    // ---- preview transport: free playback of the rendered sample ----
    // Honors the stem-lane mute/solo pills live (click-free) so auditioning via
    // Preview reflects a toggle immediately; falls back to the master mix when no
    // stem is filtered or stems aren't ready.
    {
        const int cmd = previewCmd.exchange (-1);
        if (cmd == -2)
            prevActive = false;
        else if (cmd >= 0)
        {
            prevActive = true;
            prevPos = juce::jlimit (0.0, (double) (len - 2), (double) cmd / r->speed);
            for (int k = 0; k < 6; ++k)            // start at the current target: no fade-in
                prevStemSg[k] = globalAudible[k] ? 1.0f : 0.0f;
        }

        if (prevActive)
        {
            bool prevRamping = false;
            for (int k = 0; k < 6; ++k)
                if (prevStemSg[k] != (globalAudible[k] ? 1.0f : 0.0f)) { prevRamping = true; break; }
            const bool prevUseStems = asPtr != nullptr
                                      && asPtr->buffers[0].getNumSamples() == len
                                      && (stemFilterActive || prevRamping);

            const float* psL[6] = {};
            const float* psR[6] = {};
            if (prevUseStems)
                for (int k = 0; k < 6; ++k)
                {
                    auto& b = asPtr->buffers[(size_t) k];
                    if (b.getNumSamples() == len)
                    {
                        psL[k] = b.getReadPointer (0);
                        psR[k] = b.getNumChannels() > 1 ? b.getReadPointer (1) : psL[k];
                    }
                }
            else
                for (int k = 0; k < 6; ++k)         // keep sg coherent for the next toggle
                    prevStemSg[k] = globalAudible[k] ? 1.0f : 0.0f;

            const double rate = r->sourceSampleRate / getSampleRate();
            double pos = prevPos;
            for (int i = 0; i < n; ++i)
            {
                if (pos >= (double) (len - 1)) { prevActive = false; break; }
                const int   i0 = (int) pos;
                const float fr = (float) (pos - (double) i0);
                float sl, sr;
                if (prevUseStems)
                {
                    sl = sr = 0.0f;
                    for (int k = 0; k < 6; ++k)
                    {
                        const float t  = globalAudible[k] ? 1.0f : 0.0f;
                        float       gk = prevStemSg[k];
                        if (gk < t) gk = juce::jmin (t, gk + stemGainStep);
                        else        gk = juce::jmax (t, gk - stemGainStep);
                        prevStemSg[k] = gk;
                        if (gk <= 0.0f || psL[k] == nullptr) continue;
                        sl += gk * (psL[k][i0] + fr * (psL[k][i0 + 1] - psL[k][i0]));
                        sr += gk * (psR[k][i0] + fr * (psR[k][i0 + 1] - psR[k][i0]));
                    }
                }
                else
                {
                    sl = inL[i0] + fr * (inL[i0 + 1] - inL[i0]);
                    sr = inR[i0] + fr * (inR[i0 + 1] - inR[i0]);
                }
                mainL[i] += 0.9f * sl;
                mainR[i] += 0.9f * sr;
                pos += rate;
            }
            prevPos = pos;
        }
        previewingA = prevActive;
        previewPlayPos = prevActive ? (int) (prevPos * r->speed) : -1;
    }

    // publish playheads (SOURCE-domain) for the waveform view
    for (int i = 0; i < 16; ++i)
        padPlayPos[(size_t) i].store (-1, std::memory_order_relaxed);
    for (const auto& v : voices)
    {
        if (! v.active || v.pad < 0 || v.pad >= 16)
            continue;
        int srcPos = -1;
        if (v.srcKind == 0)
        {
            srcPos = (int) (v.pos * r->speed);
        }
        else
        {
            const auto& pr = activePads[(size_t) v.pad];
            if (pr != nullptr)
                srcPos = pr->cueSrc + (int) (v.pos * (double) pr->speed * r->speed);
        }
        if (srcPos >= 0)
            padPlayPos[(size_t) v.pad].store (srcPos, std::memory_order_relaxed);
    }
}

// ============================================================================
//  State save / restore
// ============================================================================

void GentSamplerAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    juce::ValueTree extra ("EXTRA");
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        extra.setProperty ("path", filePath, nullptr);
        extra.setProperty ("key", detectedKey, nullptr);
        // KIT_SPEC.md PART C: disk stem-cache key (three-spot pattern) — this
        // is the host-project save path (getStateInformation builds its own
        // EXTRA tree rather than sharing buildKitStateXml), the exact gap Part
        // C fixes: without this, a project reopen restores masks but no audio.
        extra.setProperty ("stemKey", stemCacheKey, nullptr);
    }
    extra.setProperty ("bpm", detectedBpm.load(), nullptr);
    extra.setProperty ("ubpm", bpmOverride.load(), nullptr);
    extra.setProperty ("edW", editorW.load(), nullptr);
    extra.setProperty ("edH", editorH.load(), nullptr);
    extra.setProperty ("stemQ", stemQuality.load(), nullptr);
    extra.setProperty ("slG", sliceGridDiv.load(), nullptr);
    extra.setProperty ("slS", sliceSensitivity.load(), nullptr);
    extra.setProperty ("slP", sliceSnap.load(), nullptr);
    // SECTIONS_SPEC.md PART 3: SLICE split-chip mode model (three-spot pattern).
    extra.setProperty ("slMode", sliceModeSel.load(), nullptr);
    extra.setProperty ("slBars", sectionBars.load(), nullptr);
    extra.setProperty ("slSens", sectionSens.load(), nullptr);
    extra.setProperty ("slEven", gridEvenSel.load(), nullptr);
    // KIT_SPEC.md PART A: mode 5's persisted sub-option (three-spot pattern).
    extra.setProperty ("slKSens", kitSens.load(), nullptr);
    extra.setProperty ("snap", snapEnabled.load(), nullptr);
    extra.setProperty ("vel2l", velToLevel.load(), nullptr);
    extra.setProperty ("heroView", heroView.load(), nullptr);   // D2: hero view sticky request
    for (int k = 0; k < 6; ++k)   // stem mute/solo are plain atomics -> persist them by hand
    {
        extra.setProperty ("smute" + juce::String (k), stemMuted[(size_t) k].load(), nullptr);
        extra.setProperty ("ssolo" + juce::String (k), stemSoloed[(size_t) k].load(), nullptr);
    }
    for (int i = 0; i < 16; ++i)
    {
        extra.setProperty ("cue" + juce::String (i), cues[(size_t) i].load(), nullptr);
        extra.setProperty ("end" + juce::String (i), cueEnds[(size_t) i].load(), nullptr);
        extra.setProperty ("src" + juce::String (i), (int) padStemMask[(size_t) i].load(), nullptr);
    }
    state.appendChild (extra, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void GentSamplerAudioProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr)
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid())
        return;

    applyStateTree (state);
}

// ============================================================================

juce::AudioProcessorEditor* GentSamplerAudioProcessor::createEditor()
{
    return new GentSamplerAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GentSamplerAudioProcessor();
}
