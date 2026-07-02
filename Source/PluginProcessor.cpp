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
    }

    capEvents.reserve (4096);
    startThread();
}

GentSamplerAudioProcessor::~GentSamplerAudioProcessor()
{
    stopThread (3000);
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
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr)
        return false;

    auto* s = new SourceSample();
    const auto maxLen = (juce::int64) (reader->sampleRate * 600.0);   // cap at 10 minutes
    const int len = (int) juce::jmin (reader->lengthInSamples, maxLen);
    s->buffer.setSize ((int) reader->numChannels, len);
    reader->read (&s->buffer, 0, len, 0, true, true);
    s->sampleRate = reader->sampleRate;

    {
        const juce::SpinLock::ScopedLockType sl (srcLock);
        source = s;
    }
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        fileName = f.getFileName();
        filePath = f.getFullPathName();
        transientOnsets.clear();   // always: stale onsets must never drive a new file's auto-slice
        if (runAnalysis) { detectedKey.clear(); transientSlices.clear(); }
    }

    for (int i = 0; i < 16; ++i)
    {
        cues[(size_t) i] = (int) ((juce::int64) len * i / 16);
        cueEnds[(size_t) i] = -1;
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
    if (ts.empty())
        return sourcePos;
    auto src = getSource();
    const double sr = src != nullptr ? src->sampleRate : 44100.0;
    const int maxDist = (int) (sr * 0.05);                 // snap within 50 ms
    int best = sourcePos, bestD = maxDist + 1;
    for (int t : ts)
    {
        const int d = std::abs (t - sourcePos);
        if (d < bestD) { bestD = d; best = t; }
    }
    return bestD <= maxDist ? best : sourcePos;
}

GentSamplerAudioProcessor::CueSnap GentSamplerAudioProcessor::snapshot() const
{
    CueSnap s;
    for (int i = 0; i < 16; ++i)
    {
        s.cue[(size_t) i] = cues[(size_t) i].load();
        s.end[(size_t) i] = cueEnds[(size_t) i].load();
    }
    return s;
}

void GentSamplerAudioProcessor::applySnap (const CueSnap& s)
{
    for (int i = 0; i < 16; ++i)
    {
        cues[(size_t) i] = s.cue[(size_t) i];
        cueEnds[(size_t) i] = s.end[(size_t) i];
    }
    ++uiDirty;
}

void GentSamplerAudioProcessor::pushUndo()
{
    // drop any redo branch, record current state as the baseline to return to
    if (undoPos < (int) history.size() - 1)
        history.erase (history.begin() + undoPos + 1, history.end());
    if (history.empty())
        history.push_back (snapshot());           // seed with pre-edit state
    history.push_back (snapshot());
    if (history.size() > 64)
        history.erase (history.begin());
    undoPos = (int) history.size() - 1;
}

void GentSamplerAudioProcessor::undo()
{
    if (undoPos <= 0) return;
    // capture the live state into the top slot so redo can return to it
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
    cues[(size_t) pad] = juce::jmax (0, samplePos);
    const int e = cueEnds[(size_t) pad].load();
    if (e >= 0 && e <= samplePos + 32)                    // start pushed past end: end goes auto
        cueEnds[(size_t) pad] = -1;
}

void GentSamplerAudioProcessor::setCueEnd (int pad, int samplePos)
{
    if (pad < 0 || pad >= 16)
        return;
    if (samplePos < 0)
        cueEnds[(size_t) pad] = -1;                                   // auto (right-click reset)
    else if (samplePos <= cues[(size_t) pad].load() + 32)
        cueEnds[(size_t) pad] = kOpenSlice;                           // collapsed -> OPEN / gated slice
    else
        cueEnds[(size_t) pad] = samplePos;                           // real window end
}

int GentSamplerAudioProcessor::getEffectiveCueEnd (int pad) const
{
    auto src = getSource();
    const int len = src != nullptr ? src->buffer.getNumSamples() : 0;
    if (pad < 0 || pad >= 16 || len < 2)
        return juce::jmax (0, len - 1);

    const int cueSrc = cues[(size_t) pad].load();
    if (cueSrc < 0)
        return -1;                            // unassigned
    const int e = cueEnds[(size_t) pad].load();
    if (e == kOpenSlice)
        return len - 1;                       // open/gated: region runs to the sample end
    if (e > cueSrc)
        return juce::jmin (e, len - 1);

    if (pSlice[(size_t) pad]->load() > 0.5f)
    {
        int nextSrc = std::numeric_limits<int>::max();
        for (int i = 0; i < 16; ++i)
        {
            const int cc = cues[(size_t) i].load();
            if (cc >= 0 && cc > cueSrc && cc < nextSrc)
                nextSrc = cc;
        }
        if (nextSrc != std::numeric_limits<int>::max())
            return juce::jmin (nextSrc, len - 1);
    }
    return len - 1;
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
    int best = pos;
    long long bestDist = std::numeric_limits<long long>::max();
    auto consider = [&] (int cand)
    {
        if (cand < 0) return;
        long long d = (long long) cand - (long long) pos;
        if (d < 0) d = -d;
        if (d < bestDist) { bestDist = d; best = cand; }
    };
    if (step > 0.0) consider (nearestGridLine (pos));           // beat grid (always quantizes)
    for (int i = 0; i < 16; ++i) consider (cues[(size_t) i].load());   // placed slices
    if (step <= 0.0)
    {
        consider (nearestTransient (pos));                      // no grid -> transients
        auto src = getSource();
        const double sr = src != nullptr ? src->sampleRate : 44100.0;
        if (bestDist > (long long) (0.05 * sr)) return pos;     // nothing close: stay free
    }
    return best;
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

    analyzing = true;
    ++uiDirty;

    const auto res = gent::Analyzer::analyze (src->buffer, src->sampleRate);

    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        detectedKey = res.key;
        transientSlices = res.slices;
        transientOnsets = res.onsets;
    }
    detectedBpm = res.bpm;

    if (analysisThenSlice.exchange (false))
    {
        // a music-aware auto-slice was requested before onsets existed: do it now
        auto s = computeBlendedSlices();
        applySlices (! s.empty() ? s : res.slices, src->buffer.getNumSamples());
    }
    else if (! analysisKeepCues.load())
        applySlices (res.slices, src->buffer.getNumSamples());

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
        stems = stemEngine.separate (src->buffer, src->sampleRate, mode, sepOverlap, progressFn, err);
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
    {
        const juce::SpinLock::ScopedLockType sl (stemLock);
        stemSet = set;
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
                interp.process (ratio, stem.getReadPointer (c), atSrc.getWritePointer (c), srcLen);
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
    if (! kbMode && mode == 2)
    {
        for (auto& w : voices)
        {
            if (w.active && w.pad == pad && w.state != 2)
            {
                releaseVoices (pad, -1, false, false);
                return;
            }
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
                if (w.active && w.state != 2 && w.pad >= 0 && w.pad != pad
                    && (int) pChoke[(size_t) w.pad]->load() == myChoke)
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
    v->gate      = kbMode || mode == 0;                   // gate mode releases on key-up
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
        if (! v.active || v.pad != pad)            continue;
        if (note >= 0 && v.note != note)           continue;
        if (onlyGate && ! v.gate)                  continue;
        if (v.state == 2 && ! quick)               continue;
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
            assignPadCue (pad);
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

    writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    return true;
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

bool GentSamplerAudioProcessor::saveKit (const juce::File& kitFile)
{
    auto state = apvts.copyState();
    juce::ValueTree extra ("EXTRA");
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        extra.setProperty ("path", filePath, nullptr);
        extra.setProperty ("key", detectedKey, nullptr);
    }
    extra.setProperty ("bpm", detectedBpm.load(), nullptr);
    extra.setProperty ("ubpm", bpmOverride.load(), nullptr);
    extra.setProperty ("edW", editorW.load(), nullptr);
    extra.setProperty ("edH", editorH.load(), nullptr);
    extra.setProperty ("stemQ", stemQuality.load(), nullptr);
    extra.setProperty ("slG", sliceGridDiv.load(), nullptr);
    extra.setProperty ("slS", sliceSensitivity.load(), nullptr);
    extra.setProperty ("slP", sliceSnap.load(), nullptr);
    extra.setProperty ("snap", snapEnabled.load(), nullptr);
    extra.setProperty ("vel2l", velToLevel.load(), nullptr);
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
        return xml->writeTo (kitFile);
    return false;
}

bool GentSamplerAudioProcessor::loadKit (const juce::File& kitFile)
{
    auto xml = juce::XmlDocument::parse (kitFile);
    if (xml == nullptr)
        return false;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid())
        return false;
    applyStateTree (state);
    return true;
}

void GentSamplerAudioProcessor::applyStateTree (const juce::ValueTree& state)
{
    apvts.replaceState (state);

    auto extra = state.getChildWithName ("EXTRA");
    if (! extra.isValid())
        return;

    const juce::String path = extra.getProperty ("path", "");
    if (path.isNotEmpty())
    {
        const juce::File f (path);
        if (f.existsAsFile())
            loadFile (f, false);
    }

    for (int i = 0; i < 16; ++i)
    {
        cues[(size_t) i] = (int) extra.getProperty ("cue" + juce::String (i), cues[(size_t) i].load());
        cueEnds[(size_t) i] = (int) extra.getProperty ("end" + juce::String (i), -1);
        padStemMask[(size_t) i] = (std::uint8_t) (int) extra.getProperty ("src" + juce::String (i), 0);
    }

    detectedBpm = (double) extra.getProperty ("bpm", 0.0);
    bpmOverride = (double) extra.getProperty ("ubpm", 0.0);
    editorW = (int) extra.getProperty ("edW", 900);
    editorH = (int) extra.getProperty ("edH", 640);
    setStemQuality ((int) extra.getProperty ("stemQ", (int) stemQuality.load()));   // clamped 0..2
    setSliceGridDiv     ((int) extra.getProperty ("slG", (int) sliceGridDiv.load()));
    setSliceSensitivity ((int) extra.getProperty ("slS", (int) sliceSensitivity.load()));
    setSliceSnap        ((int) extra.getProperty ("slP", (int) sliceSnap.load()));
    snapEnabled = (bool) extra.getProperty ("snap", snapEnabled.load());
    velToLevel  = (bool) extra.getProperty ("vel2l", true);
    for (int k = 0; k < 6; ++k)   // restore stem mute/solo (default audible for older presets)
    {
        stemMuted[(size_t) k]  = (bool) extra.getProperty ("smute" + juce::String (k), false);
        stemSoloed[(size_t) k] = (bool) extra.getProperty ("ssolo" + juce::String (k), false);
    }
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        detectedKey = extra.getProperty ("key", "").toString();
    }

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

    {
        const juce::SpinLock::ScopedTryLockType tl (rendLock);
        if (tl.isLocked())
            active = rendered;
    }
    {
        const juce::SpinLock::ScopedTryLockType tl (stemRendLock);
        if (tl.isLocked())
            activeStems = renderedStems;
    }
    {
        const juce::SpinLock::ScopedTryLockType tl (padLock);
        if (tl.isLocked())
            for (int i = 0; i < 16; ++i)
                activePads[(size_t) i] = padRenders[(size_t) i];
    }

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
        // Bleed: on a FILTERED pad, the UNSELECTED stems come back in at low gain
        // (bleed*0.5) instead of hard 0 — organic background. bleed=0 => bit-identical
        // to pure isolation; FULL pads (mask 0) are unaffected. Global mute/solo still wins.
        const float bleedGain = padFiltered
            ? juce::jlimit (0.0f, 1.0f, pBleed[(size_t) juce::jlimit (0, 15, v.pad)]->load()) * 0.5f
            : 0.0f;
        for (int k = 0; k < 6; ++k)
        {
            float gk;
            if (! padFiltered)               gk = 1.0f;          // FULL: every stem full
            else if (((pmask >> k) & 1) != 0) gk = 1.0f;          // selected stem
            else                              gk = bleedGain;     // unselected stem bled in
            vStemGain[k] = globalAudible[k] ? gk : 0.0f;
        }
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
    }
    extra.setProperty ("bpm", detectedBpm.load(), nullptr);
    extra.setProperty ("ubpm", bpmOverride.load(), nullptr);
    extra.setProperty ("edW", editorW.load(), nullptr);
    extra.setProperty ("edH", editorH.load(), nullptr);
    extra.setProperty ("stemQ", stemQuality.load(), nullptr);
    extra.setProperty ("slG", sliceGridDiv.load(), nullptr);
    extra.setProperty ("slS", sliceSensitivity.load(), nullptr);
    extra.setProperty ("slP", sliceSnap.load(), nullptr);
    extra.setProperty ("snap", snapEnabled.load(), nullptr);
    extra.setProperty ("vel2l", velToLevel.load(), nullptr);
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
