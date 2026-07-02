#include "PluginEditor.h"

static const char* kRootNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

GentSamplerAudioProcessorEditor::GentSamplerAudioProcessorEditor (GentSamplerAudioProcessor& proc)
    : juce::AudioProcessorEditor (proc), p (proc), wave (proc), sliceDetail (proc), pads (proc)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (wave);
    addAndMakeVisible (sliceDetail);
    addAndMakeVisible (pads);
    addAndMakeVisible (loadBtn);
    addAndMakeVisible (sliceBtn);
    addAndMakeVisible (sliceMode);
    addAndMakeVisible (saveKitBtn);
    addAndMakeVisible (loadKitBtn);
    addAndMakeVisible (exportKitBtn);
    addAndMakeVisible (recBtn);
    addAndMakeVisible (tempoMode);
    addAndMakeVisible (playLbl);
    addAndMakeVisible (ratioLbl);
    addAndMakeVisible (halfBtn);
    addAndMakeVisible (dblBtn);
    addAndMakeVisible (followBtn);
    addAndMakeVisible (previewBtn);
    addAndMakeVisible (undoBtn);
    addAndMakeVisible (redoBtn);
    addAndMakeVisible (snapBtn);
    addAndMakeVisible (clearBtn);
    addAndMakeVisible (kbBtn);
    addAndMakeVisible (fileLbl);
    addAndMakeVisible (bpmLbl);
    addAndMakeVisible (keyPick);
    addAndMakeVisible (titleLbl);
    addAndMakeVisible (padTitle);
    addAndMakeVisible (padRead);
    addAndMakeVisible (sliceStop);
    addAndMakeVisible (playMode);
    addAndMakeVisible (masterPitch);

    playMode.addItem ("GATE  (sounds while held)", 1);
    playMode.addItem ("ONE-SHOT  (plays the slice)", 2);
    playMode.addItem ("LATCH  (press on / press off)", 3);

    addAndMakeVisible (chokeBox);
    // mockup chip label; ids match the Choice param order. CharPointer_UTF8 — a plain
    // char* literal with multi-byte UTF-8 renders as mojibake in ComboBox items.
    chokeBox.addItem (juce::String (juce::CharPointer_UTF8 ("CHOKE \xc2\xb7 OFF")), 1);
    for (int g = 1; g <= 8; ++g) chokeBox.addItem ("CHOKE " + juce::String (g), g + 1);
    chokeBox.setTooltip ("Pads sharing a choke group cut each other off (e.g. open vs closed hi-hat).");
    addAndMakeVisible (chokeLbl);
    chokeLbl.setText ("CHOKE", juce::dontSendNotification);
    chokeLbl.setFont (juce::Font (10.0f, juce::Font::bold));
    chokeLbl.setColour (juce::Label::textColourId, juce::Colour (0xff9c9a91));

    addAndMakeVisible (loopBtn);
    loopBtn.setTooltip ("Loop the pad's region instead of playing it once (hold it in Gate/Latch mode).");
    addAndMakeVisible (revBtn);
    revBtn.setTooltip ("Play the pad's slice backwards.");

    // SELECTED PAD detail: big number + cue line + meta line
    padTitle.setFont (juce::Font (38.0f, juce::Font::bold));
    padTitle.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (padMetaLbl);
    padMetaLbl.setFont (juce::Font (9.5f));
    padMetaLbl.setColour (juce::Label::textColourId, juce::Colour (0xff6b6f74));
    addAndMakeVisible (padMeta2Lbl);   // .m2: second meta line (SLICE n - CUE t)

    // TRIGGER icon buttons drive the (hidden) playMode combo
    {
        static const char* tl[3] = { "GATE", "ONE-SHOT", "LATCH" };
        static const char* tw[3] = { "hold", "tap-fire", "tap on/off" };
        for (int i = 0; i < 3; ++i)
        {
            auto& b = trigSeg[(size_t) i];
            b.icon  = i;
            b.pos   = i;              // 0 left / 1 mid / 2 right segment of the shared well
            b.label = tl[i];
            b.word  = tw[i];
            addAndMakeVisible (b);
            b.onClick = [this, i] { playMode.setSelectedItemIndex (i, juce::sendNotification); };
        }
    }

    titleLbl.setText ("GentSampler", juce::dontSendNotification);
    titleLbl.setFont (juce::Font (24.0f, juce::Font::bold));
    titleLbl.setColour (juce::Label::textColourId, juce::Colour (0xfff1ebdd));

    // readouts that live inside the black display
    fileLbl.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.85f));
    fileLbl.setFont (juce::Font (13.0f));
    padRead.setColour (juce::Label::textColourId, juce::Colour (0xff180c04));
    padRead.setFont (juce::Font (14.0f, juce::Font::bold));
    padRead.setJustificationType (juce::Justification::centred);

    // SRC: what the record IS. Double-click to correct detection (0 resets).
    bpmLbl.setJustificationType (juce::Justification::centredRight);
    bpmLbl.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.9f));
    bpmLbl.setFont (juce::Font (14.0f, juce::Font::bold));
    bpmLbl.setEditable (false, true, false);
    bpmLbl.setTooltip ("Source tempo of the loaded recording. Double-click to correct it (0 = back to detected).");
    bpmLbl.onTextChange = [this]
    {
        const double v = bpmLbl.getText().retainCharacters ("0123456789.").getDoubleValue();
        p.setSourceBpmOverride (v);
    };
    halfBtn.setTooltip ("Detection called it double-time? Halve the source BPM.");
    dblBtn.setTooltip ("Detection called it half-time? Double the source BPM.");
    halfBtn.onClick = [this] { const double b = p.getEffectiveSourceBpm(); if (b > 1.0) p.setSourceBpmOverride (b * 0.5); };
    dblBtn.onClick  = [this] { const double b = p.getEffectiveSourceBpm(); if (b > 1.0) p.setSourceBpmOverride (b * 2.0); };

    // PLAY: what it's playing AT. Editable; typing a value switches to CUSTOM.
    playLbl.setJustificationType (juce::Justification::centredRight);
    playLbl.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.9f));
    playLbl.setFont (juce::Font (14.0f, juce::Font::bold));
    playLbl.setEditable (false, true, false);
    playLbl.setTooltip ("Playback tempo. OFF mirrors SRC, SYNC mirrors your session, or type a value to go CUSTOM.");
    playLbl.onTextChange = [this]
    {
        const double v = playLbl.getText().retainCharacters ("0123456789.").getDoubleValue();
        if (v >= 40.0 && v <= 220.0)
        {
            p.apvts.getParameterAsValue ("customBpm") = v;
            p.apvts.getParameterAsValue ("tempoMode") = 2;     // CUSTOM
        }
    };

    ratioLbl.setJustificationType (juce::Justification::centred);
    ratioLbl.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.45f));
    ratioLbl.setFont (juce::Font (11.0f));

    // the dropdown shows the ACTIVE key; picking a new one retunes the flip
    keyPick.setTextWhenNothingSelected ("KEY --");
    keyPick.onChange = [this]
    {
        const int id = keyPick.getSelectedId();
        if (id <= 0) return;
        const int from = rootIndexOf (p.getDetectedKey());
        if (from < 0) return;
        int d = (((id - 1) - from) % 12 + 12) % 12;
        if (d > 6) d -= 12;                          // shortest path: -5 .. +6
        p.apvts.getParameterAsValue ("masterPitch") = (double) d;
    };

    tempoMode.addItem ("OFF", 1);
    tempoMode.addItem ("SYNC", 2);
    tempoMode.addItem ("CUSTOM", 3);

    masterPitch.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    masterPitch.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 16);

    sliceMode.addItem ("16 EQUAL", 1);
    sliceMode.addItem ("EVERY BEAT", 2);
    sliceMode.addItem ("EVERY 2 BEATS", 3);
    sliceMode.addItem ("EVERY BAR", 4);
    sliceMode.setTextWhenNothingSelected ("GRID...");
    sliceMode.onChange = [this]
    {
        if (sliceMode.getSelectedId() > 0) p.pushUndo();
        switch (sliceMode.getSelectedId())
        {
            case 1: p.sliceGrid(); break;
            case 2: p.sliceBeats (1.0); break;
            case 3: p.sliceBeats (2.0); break;
            case 4: p.sliceBeats (4.0); break;
            default: break;
        }
        sliceMode.setSelectedId (0, juce::dontSendNotification);
    };

    auto initRotary = [this] (juce::Slider& s, juce::Label& l, const juce::String& name)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 16);
        addAndMakeVisible (s);
        l.setText (name, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        l.setFont (Theme::ui (8.0f, true).withExtraKerningFactor (0.18f));   // .kl caption (mockup 7px CSS x1.12)
        l.setColour (juce::Label::textColourId, Theme::t2);
        addAndMakeVisible (l);
    };
    initRotary (padPitch, ppL, "PITCH");
    initRotary (padSpeed, psL, "SPEED");
    initRotary (padLevel, plL, "LEVEL");
    initRotary (padAtt,   paL, "ATTACK");
    initRotary (padRel,   prL, "RELEASE");
    initRotary (padCrush, pcL, "CRUSH");
    initRotary (padPan,   ppanL, "PAN");
    initRotary (padCutoff, pcoL, "CUTOFF");
    initRotary (padReso,   preL, "RESO");
    padSpeed.setTextValueSuffix ("x");
    padCutoff.setTextValueSuffix (" Hz");

    // BLEED: 36px mini-knob at the right of the SOURCE row (mockup bleedSlot)
    addAndMakeVisible (padBleed);
    padBleed.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    padBleed.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 12);
    padBleed.setTooltip ("Bring the UN-selected stems back in at low gain - organic bleed/background. "
                         "0 = surgical isolation. No effect when PAD SOURCE is FULL.");
    addAndMakeVisible (pbL);
    pbL.setText ("BLEED", juce::dontSendNotification);
    pbL.setJustificationType (juce::Justification::centred);

    // ---- GRANULAR controls ----
    initRotary (padGrainSize,  gsL, "SIZE");
    initRotary (padGrainDens,  gdL, "DENS");
    initRotary (padGrainPos,   gpL, "POS");
    initRotary (padGrainSpray, gyL, "SPRAY");
    initRotary (padGrainPitch, gtL, "G.PITCH");
    padGrainSize.setTextValueSuffix (" ms");
    addAndMakeVisible (grainBtn);
    grainBtn.setTooltip ("Granular mode for this pad - turn the slice into an evolving texture. "
                         "Hold or latch for a sustained pad; combine with FREEZE for a drone.");
    addAndMakeVisible (freezeBtn);
    freezeBtn.setTooltip ("Freeze the grain position at POS (scrub it) for a stable drone, instead of "
                          "following the playhead.");

    addAndMakeVisible (ftypeBox);
    ftypeBox.addItem ("Off", 1);
    ftypeBox.addItem ("LP", 2);
    ftypeBox.addItem ("HP", 3);
    ftypeBox.addItem ("BP", 4);
    ftypeBox.setTooltip ("Per-pad filter: low-pass / high-pass / band-pass (Off = bypass).");
    addAndMakeVisible (ftypeLbl);
    ftypeLbl.setText ("TYPE", juce::dontSendNotification);
    ftypeLbl.setJustificationType (juce::Justification::centred);
    ftypeLbl.setFont (juce::Font (8.0f, juce::Font::bold));
    ftypeLbl.setColour (juce::Label::textColourId, juce::Colour (0xff6b6f74));

    // (pan/pitch display formats are applied in attachPad — the SliderAttachment
    //  overwrites the slider text functions on every rebind, so they live there)

    // drag a PAD cell out to the DAW/desktop as a rendered 24-bit WAV (current
    // per-pad pitch/level/crush/speed baked in via exportPad -> renderPadSlice)
    pads.makePadFile = [this] (int pad) -> juce::File
    {
        if (pad < 0 || p.getCue (pad) < 0) return {};
        const auto f = tempDir().getChildFile ("GentSampler_Pad" + juce::String (pad + 1).paddedLeft ('0', 2) + ".wav");
        return p.exportPad (pad, f) ? f : juce::File();
    };
    midiChip = std::make_unique<DragChip> ("MIDI", [this]() -> juce::File
    {
        const auto f = tempDir().getChildFile ("GentSampler_Performance.mid");
        return p.exportCapturedMidi (f) ? f : juce::File();
    });
    addAndMakeVisible (*midiChip);

    // TRANSCRIBE -> MIDI (Basic Pitch): per-pad audio-to-MIDI with a drag-out chip
    transcribeChip = std::make_unique<DragChip> ("MIDI", [this]() -> juce::File
    {
        return p.transcriptionReady() ? p.getTranscriptionFile() : juce::File();
    });
    addChildComponent (*transcribeChip);   // shown only once a transcription is ready

    addAndMakeVisible (transcribeBtn);
    transcribeBtn.setTooltip ("Transcribe the selected pad's slice to MIDI notes (Spotify Basic Pitch, "
                              "CPU). Best on tonal/melodic slices; percussive pads transcribe to noise.");
    transcribeBtn.onClick = [this] { p.requestTranscription (p.selectedPad.load()); ++p.uiDirty; };

    addAndMakeVisible (transcribeQuantBtn);
    transcribeQuantBtn.setTooltip ("Snap transcribed notes to GentSampler's beat grid (uses the GRID "
                                   "division). On by default when a tempo is known.");
    transcribeQuantBtn.setToggleState (p.getTranscribeQuantize(), juce::dontSendNotification);
    transcribeQuantBtn.onClick = [this] { p.setTranscribeQuantize (transcribeQuantBtn.getToggleState()); };

    addAndMakeVisible (transcribeLbl);
    transcribeLbl.setInterceptsMouseClicks (false, false);   // status only — never block the drag chip
    transcribeLbl.setFont (juce::Font (9.5f));
    transcribeLbl.setColour (juce::Label::textColourId, juce::Colour (0xff9c9a91));
    transcribeLbl.setJustificationType (juce::Justification::centredLeft);

    // MIDI velocity-respect toggle (global, default on) — sits by the MIDI buttons
    addAndMakeVisible (velBtn);
    velBtn.setTooltip ("When on, incoming MIDI note velocity scales the pad's playback level. "
                       "Off = every note plays at the pad's set level.");
    velBtn.setToggleState (p.getVelToLevel(), juce::dontSendNotification);
    velBtn.onClick = [this] { p.setVelToLevel (velBtn.getToggleState()); };

    // visible per-pad CLEAR (mirrors the existing right-click-a-pad path, undoable)
    addAndMakeVisible (clearPadBtn);
    clearPadBtn.setTooltip ("Clear the selected pad's slice + cue (undoable). Right-clicking a pad does the same.");
    clearPadBtn.onClick = [this]
    {
        const int pad = p.selectedPad.load();
        if (p.getCue (pad) >= 0)
        {
            p.pushUndo();
            p.clearCue (pad);
            p.clearedFlash = pad;
            p.clearedFlashTime = juce::Time::getMillisecondCounter();
            ++p.uiDirty;
        }
    };

    aMaster    = std::make_unique<SA> (p.apvts, "masterPitch", masterPitch);
    aTempoMode = std::make_unique<CA> (p.apvts, "tempoMode", tempoMode);
    aKb        = std::make_unique<BA> (p.apvts, "kbMode", kbBtn);
    attachPad (0);

    loadBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (
            "Load a sample", juce::File(), "*.wav;*.mp3;*.aif;*.aiff;*.flac;*.ogg");
        chooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f.existsAsFile())
                    p.loadFile (f);
            });
    };
    wave.onRequestLoad = loadBtn.onClick;          // clicking the empty map also opens the browser

    sliceBtn.onClick = [this] { p.pushUndo(); p.sliceTransients(); };

    saveKitBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Save kit", juce::File(), "*.gentkit");
        chooser->launchAsync (
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f == juce::File()) return;
                if (! f.hasFileExtension ("gentkit"))
                    f = f.withFileExtension ("gentkit");
                p.saveKit (f);
            });
    };

    loadKitBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Load kit", juce::File(), "*.gentkit");
        chooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f.existsAsFile())
                    p.loadKit (f);
            });
    };

    exportKitBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a folder for the kit export", juce::File());
        chooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                const auto dir = fc.getResult();
                if (dir != juce::File())
                    p.exportKit (dir);
            });
    };

    previewBtn.setTooltip ("Audition from the playhead. Click/scrub the waveform to move it. Hit an empty pad to drop its cue here.");
    previewBtn.onClick = [this]
    {
        if (p.isPreviewing())
            p.stopPreview();
        else
            p.startPreview (juce::jmax (0, p.getAssignCursor()));
    };

    snapBtn.setTooltip ("Snap cue/handle edits to the beat grid (shown on the waveform); "
                        "falls back to the nearest transient when there's no reliable tempo.");
    snapBtn.onClick = [this] { p.snapEnabled = snapBtn.getToggleState(); };

    clearBtn.setTooltip ("Clear / reset cues.");
    clearBtn.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "Clear selected pad (or right-click the pad)");
        m.addItem (2, "Reset all region ends to auto");
        m.addSeparator();
        m.addItem (3, "Clear ALL pads (blank slate - then tap pads to assign)");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (clearBtn),
            [this] (int result)
            {
                if (result <= 0) return;
                p.pushUndo();
                if (result == 1)
                    p.clearCue (p.selectedPad.load());
                else if (result == 2)
                    for (int i = 0; i < 16; ++i)
                        p.setCueEnd (i, -1);
                else if (result == 3)
                    for (int i = 0; i < 16; ++i)
                        p.clearCue (i);
                ++p.uiDirty;
            });
    };

    undoBtn.setTooltip ("Undo cue edit (Ctrl+Z)");
    redoBtn.setTooltip ("Redo cue edit (Ctrl+Y / Ctrl+Shift+Z)");
    undoBtn.onClick = [this] { p.undo(); };
    redoBtn.onClick = [this] { p.redo(); };

    // ---- stem separation controls ----
    addAndMakeVisible (sepStemsBtn);
    sepStemsBtn.setTooltip ("Split the loaded sample into 6 stems (drums, bass, vocals, "
                            "guitar, piano, other). Runs on CPU; takes a bit.");
    sepStemsBtn.onClick = [this] { p.requestStemSeparation(); };

    addAndMakeVisible (stemStatusLbl);
    stemStatusLbl.setJustificationType (juce::Justification::centredLeft);
    stemStatusLbl.setText ("No stems yet", juce::dontSendNotification);

    // (stem mute/solo lives on the WaveformView lane pills, not a button row)

    // PAD SOURCE tags (FULL + 6 stems) — per-pad stem source. Hues come from the
    // "stemHue" component property (set in the skin block); no colour ids here —
    // a buttonColourId would trip the LnF's active-tint override.
    {
        static const char* slbl[7] = { "FULL", "DRM", "BASS", "VOX", "GTR", "PNO", "OTH" };
        for (int i = 0; i < 7; ++i)
        {
            auto& b = srcTag[(size_t) i];
            b.setButtonText (slbl[i]);
            b.setClickingTogglesState (false);
            addAndMakeVisible (b);
            b.onClick = [this, i]
            {
                const int pad = p.selectedPad.load();
                if (i == 0) p.setPadFull (pad);
                else        p.setPadStemBit (pad, i - 1, ! p.isPadStemOn (pad, i - 1));
                ++p.uiDirty;
            };
        }
    }

    addKeyListener (this);
    setWantsKeyboardFocus (true);

    // F4: mouse-grabbing a handle on EITHER surface arms it as the arrow-nudge target
    // (SLICE_FEEL_TASK.md F4). Both surfaces call the same callback shape; the editor
    // is the single owner of the armed-handle decision (ground rule 1).
    wave.onHandleGrabbed = [this] (HandleDragEngine::Handle h)
    {
        armedHandle = h;
        armedHandlePad = p.selectedPad.load();
        sliceDetail.setArmedHandle (h);
    };
    sliceDetail.onHandleGrabbed = [this] (HandleDragEngine::Handle h)
    {
        armedHandle = h;
        armedHandlePad = p.selectedPad.load();
        sliceDetail.setArmedHandle (h);
    };
    sliceDetail.setArmedHandle (armedHandle);   // paint the initial CUE default

    followBtn.setToggleState (true, juce::dontSendNotification);
    followBtn.setTooltip ("Snap the waveform view to whichever pad was last triggered.");
    followBtn.onClick = [this] { wave.setFollow (followBtn.getToggleState()); };

    addAndMakeVisible (fullBtn);
    fullBtn.setTooltip ("Zoom the waveform out to the whole sample (same as double-clicking it).");
    fullBtn.onClick = [this] { wave.fullView(); };

    // GPU is hard-disabled (crashes the host); the separation QUALITY dial takes its slot.
    addAndMakeVisible (qualityBox);
    qualityBox.addItem ("FAST", 1);   // 6-stem,  overlap 0.25  (~35s)
    qualityBox.addItem ("HQ",   2);   // 6-stem,  overlap 0.5   (~55s, cleaner)
    qualityBox.addItem ("MAX",  3);   // ft+6s hybrid, overlap 0.5 (best; several minutes on CPU)
    qualityBox.setTooltip ("Separation quality.  FAST: 6-stem, fastest.  "
                           "HQ: 6-stem, more overlap = cleaner (~1.5x time).  "
                           "MAX: ft+6s hybrid (best drums/bass/vocals) - several MINUTES on CPU.");
    qualityBox.setSelectedId (juce::jlimit (0, 2, p.getStemQuality()) + 1, juce::dontSendNotification);
    qualityBox.onChange = [this] { p.setStemQuality (qualityBox.getSelectedId() - 1); };

    recBtn.onClick = [this]
    {
        if (p.isCapturing())
            p.stopMidiCapture();
        else
            p.startMidiCapture();
    };

    // ---- toolbar dropdown menus (replace the old left panel) ----
    addAndMakeVisible (sliceMenu);
    addAndMakeVisible (kitMenu);
    addAndMakeVisible (exportMenu);
    sliceMenu.setTooltip ("Slice the sample onto pads: auto by transients, or by a grid. Also clear.");
    kitMenu.setTooltip ("Kits save / recall a whole flip.");
    exportMenu.setTooltip ("Export the kit's samples, or the selected pad as a WAV.");

    sliceMenu.onClick = [this]
    {
        const int gd = p.getSliceGridDiv(), sn = p.getSliceSensitivity(), sp = p.getSliceSnap();
        juce::PopupMenu m;
        m.addItem (1, "Auto-slice: musical (transients + grid)");
        m.addItem (2, "Auto-slice: transients only");
        m.addSeparator();
        juce::PopupMenu grid;
        grid.addItem (10, "Bar",  true, gd == 0);
        grid.addItem (11, "Beat", true, gd == 1);
        grid.addItem (12, "1/8",  true, gd == 2);
        grid.addItem (13, "1/16", true, gd == 3);
        m.addSubMenu ("Grid", grid);
        juce::PopupMenu sens;
        sens.addItem (20, "Low",    true, sn == 0);
        sens.addItem (21, "Medium", true, sn == 1);
        sens.addItem (22, "High",   true, sn == 2);
        m.addSubMenu ("Sensitivity", sens);
        juce::PopupMenu snap;
        snap.addItem (30, "Loose",  true, sp == 0);
        snap.addItem (31, "Medium", true, sp == 1);
        snap.addItem (32, "Tight",  true, sp == 2);
        m.addSubMenu ("Snap to grid", snap);
        m.addSeparator();
        juce::PopupMenu even;
        even.addItem (40, "16 equal");
        even.addItem (41, "Every beat");
        even.addItem (42, "Every 2 beats");
        even.addItem (43, "Every bar");
        m.addSubMenu ("Even grid (ignore audio)", even);
        m.addSeparator();
        m.addItem (50, "Clear selected pad");
        m.addItem (51, "Reset all region ends to auto");
        m.addItem (52, "Clear ALL pads (blank slate)");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (sliceMenu),
            [this] (int r)
            {
                if (r <= 0) return;
                if (r == 1) { p.pushUndo(); p.autoSliceMusical(); return; }
                if (r == 2) { if (sliceBtn.onClick) sliceBtn.onClick(); return; }
                if (r >= 10 && r <= 13) { p.pushUndo(); p.setSliceGridDiv (r - 10);      p.autoSliceMusical(); return; }
                if (r >= 20 && r <= 22) { p.pushUndo(); p.setSliceSensitivity (r - 20);  p.autoSliceMusical(); return; }
                if (r >= 30 && r <= 32) { p.pushUndo(); p.setSliceSnap (r - 30);         p.autoSliceMusical(); return; }
                if (r >= 40 && r <= 43) { sliceMode.setSelectedId (r - 39, juce::sendNotification); return; }
                p.pushUndo();
                if (r == 50)      p.clearCue (p.selectedPad.load());
                else if (r == 51) for (int i = 0; i < 16; ++i) p.setCueEnd (i, -1);
                else if (r == 52) for (int i = 0; i < 16; ++i) p.clearCue (i);
                ++p.uiDirty;
            });
    };

    kitMenu.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "Save Kit: all 16 pads, cues, stems & settings");
        m.addItem (2, "Load Kit: recall a saved flip");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (kitMenu),
            [this] (int r)
            {
                if (r == 1 && saveKitBtn.onClick) saveKitBtn.onClick();
                else if (r == 2 && loadKitBtn.onClick) loadKitBtn.onClick();
            });
    };

    exportMenu.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "Export Kit (samples to a folder)...");
        m.addItem (2, "Export selected Pad as WAV...");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (exportMenu),
            [this] (int r)
            {
                if (r == 1) { if (exportKitBtn.onClick) exportKitBtn.onClick(); return; }
                if (r == 2)
                {
                    const int sel = p.selectedPad.load();
                    chooser = std::make_unique<juce::FileChooser> ("Export pad as WAV", juce::File(), "*.wav");
                    chooser->launchAsync (
                        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                        [this, sel] (const juce::FileChooser& fc)
                        {
                            auto f = fc.getResult();
                            if (f == juce::File()) return;
                            if (! f.hasFileExtension ("wav")) f = f.withFileExtension ("wav");
                            p.exportPad (sel, f);
                        });
                }
            });
    };

    // these actions now live inside the toolbar menus — keep them constructed
    // (their onClick logic is reused) but out of the visible layout
    sliceBtn.setVisible (false);
    sliceMode.setVisible (false);
    clearBtn.setVisible (false);
    loadKitBtn.setVisible (false);
    saveKitBtn.setVisible (false);
    exportKitBtn.setVisible (false);
    sliceStop.setButtonText ("STOP AT CUE");

    startTimerHz (15);

    // ================= Redesign Phase A — skin wiring (paint only) =================
    // Chip variants, stem hues and bipolar flags ride on component properties the
    // LookAndFeel reads; fonts/colours below override the legacy scattered styling.
    {
        // chip kinds
        for (auto* btn : { &loadBtn, &kitMenu, &exportMenu, &fullBtn, &clearPadBtn })
            btn->getProperties().set ("chip", "ghost");
        sepStemsBtn.getProperties().set ("chip", "primary");

        // stem-hued source chips: FULL + the six stems
        srcTag[0].getProperties().set ("stemHue", (juce::int64) Theme::fullStem.getARGB());
        for (int k = 0; k < 6; ++k)
            srcTag[(size_t) (k + 1)].getProperties().set ("stemHue", (juce::int64) Theme::stem (k).getARGB());

        // bipolar knobs: arc fills from 12 o'clock outward
        for (auto* s : { &masterPitch, &padPitch, &padPan, &padGrainPitch })
            s->getProperties().set ("bipolar", true);

        // hero overlay chips: OPAQUE faces so they read over waveform content (B6.1)
        for (auto* c : std::initializer_list<juce::Component*> {
                 &previewBtn, &snapBtn, &followBtn, &sliceMenu, &fullBtn, &qualityBox })
            c->getProperties().set ("chip", "overlay");
        stemStatusLbl.getProperties().set ("overlayPill", true);
        fileLbl.getProperties().set ("overlayPill", true);

        // (pan/pitch value formats live in attachPad — SliderAttachment overwrites the
        //  slider text functions on every rebind; CHOKE item texts are set at creation)

        // header readouts (.ro.v mono 12 / .fname mono 9); wordmark is painted
        titleLbl.setVisible (false);
        fileLbl.setFont (Theme::mono (10.0f));
        fileLbl.setColour (juce::Label::textColourId, Theme::t3);
        for (auto* ro : { &bpmLbl, &playLbl })
        {
            ro->setFont (Theme::mono (13.5f));
            ro->setColour (juce::Label::textColourId, Theme::t1);
            ro->setJustificationType (juce::Justification::centred);
        }
        ratioLbl.setFont (Theme::mono (11.0f));
        ratioLbl.setColour (juce::Label::textColourId, Theme::t3);
        padRead.setFont (Theme::mono (13.5f, true).withExtraKerningFactor (0.06f));
        padRead.setColour (juce::Label::textColourId, Theme::inkOnAccent);

        // pad header block (.padNum 24 mono / .m1 9.5 mono / .m2 7.5 tracked)
        padTitle.setFont (Theme::mono (27.0f, true));
        padTitle.setColour (juce::Label::textColourId, Theme::accent);
        padMetaLbl.setFont (Theme::mono (10.5f).withExtraKerningFactor (0.08f));
        padMetaLbl.setColour (juce::Label::textColourId, Theme::t2);
        padMeta2Lbl.setFont (Theme::ui (8.5f).withExtraKerningFactor (0.16f));
        padMeta2Lbl.setColour (juce::Label::textColourId, Theme::t3);

        // status / hint text
        stemStatusLbl.setFont (Theme::ui (11.0f));
        stemStatusLbl.setColour (juce::Label::textColourId, Theme::t2);
        transcribeLbl.setFont (Theme::ui (10.0f));
        transcribeLbl.setColour (juce::Label::textColourId, Theme::t3);
        pbL.setFont (Theme::slabelFont());
        pbL.setColour (juce::Label::textColourId, Theme::t3);
        chokeLbl.setFont (Theme::slabelFont());
        chokeLbl.setColour (juce::Label::textColourId, Theme::t3);
        ftypeLbl.setFont (Theme::ui (8.5f, true).withExtraKerningFactor (0.18f));
        ftypeLbl.setColour (juce::Label::textColourId, Theme::t2);
    }

    // ---- whole-UI proportional scaling ----
    // Park every child under `root` (a fixed design-size container) and scale it as one
    // unit via an AffineTransform in resized(), so resizing zooms uniformly (never reflows).
    root.onPaint   = [this] (juce::Graphics& g) { paintContent (g); };
    root.onResized = [this] { layoutContent(); };
    addAndMakeVisible (root);
    root.setInterceptsMouseClicks (false, true);   // clicks pass through to the scaled children
    {
        juce::Array<juce::Component*> kids;
        for (int i = 0; i < getNumChildComponents(); ++i)
            if (auto* c = getChildComponent (i); c != &root)
                kids.add (c);
        for (auto* c : kids)
            root.addChildComponent (c);            // re-parent in order (keeps z + each child's visibility)
    }

    // aspect-locked resize around the mockup's own 1040x700. Default open = 100%
    // (the approved judge-at-this-size footprint); min 880x592 is the CANDIDATE
    // floor pending Joe's pick at the B5 gate; max 1560x1050 (1.5x) for detail work.
    setResizable (true, true);
    setResizeLimits (880, 592, 1560, 1050);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) kDesignW / (double) kDesignH);
    setSize (kDesignW, kDesignH);
}

GentSamplerAudioProcessorEditor::~GentSamplerAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

int GentSamplerAudioProcessorEditor::rootIndexOf (const juce::String& keyText)
{
    const auto root = keyText.upToFirstOccurrenceOf (" ", false, false).trim();
    for (int i = 0; i < 12; ++i)
        if (root == kRootNames[i])
            return i;
    return -1;
}

juce::File GentSamplerAudioProcessorEditor::tempDir() const
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("GentSampler");
    d.createDirectory();
    return d;
}

void GentSamplerAudioProcessorEditor::paintContent (juce::Graphics& g)
{
    const int w = kDesignW, hgt = kDesignH;

    // FRAME canvas — mockup .plugin radial (130%/110% at 50% 0%). Built once as a
    // low-res square tile, drawn stretched: the non-uniform stretch makes the radial
    // elliptical and the cache means no gradient math on repaint.
    if (! frameImg.isValid())
    {
        constexpr int N = 256;
        frameImg = juce::Image (juce::Image::ARGB, N, N, true);
        juce::Graphics fg (frameImg);
        juce::ColourGradient base (Theme::canvasTop, N * 0.5f, 0.0f,
                                   Theme::canvasEdge, N * 0.5f, N * 1.15f, true);
        base.addColour (0.62, Theme::canvas);
        fg.setGradientFill (base);
        fg.fillAll();
    }
    g.drawImage (frameImg, juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) hgt));

    // header bar — raised strip (panel-hi -> panel, inset edge-hi top, dark bottom edge)
    {
        juce::ColourGradient hg (Theme::panelHi, 0.0f, 0.0f,
                                 Theme::panel, 0.0f, (float) headerRect.getHeight(), false);
        g.setGradientFill (hg);
        g.fillRect (headerRect);
        g.setColour (Theme::edgeHi());
        g.drawHorizontalLine (headerRect.getY(), 0.0f, (float) w);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawHorizontalLine (headerRect.getBottom() - 1, 0.0f, (float) w);
        // group separators (.divv)
        g.setColour (Theme::edgeHi());
        for (int dx : hdrDividers)
            g.drawVerticalLine (dx, (float) headerRect.getY() + 14.0f, (float) headerRect.getBottom() - 12.0f);
        // wordmark (.wm two-tone) + tag — painted, replaces the old titleLbl
        g.setFont (Theme::ui (17.0f, true).withExtraKerningFactor (0.02f));
        g.setColour (Theme::t1);
        g.drawText ("Gent", 14, headerRect.getY() + 8, 44, 18, juce::Justification::left);
        g.setColour (Theme::accent);
        g.drawText ("Sampler", 14 + (int) Theme::ui (17.0f, true).getStringWidthFloat ("Gent"),
                    headerRect.getY() + 8, 80, 18, juce::Justification::left);
        g.setFont (Theme::ui (8.0f).withExtraKerningFactor (0.22f));
        g.setColour (Theme::t3);
        g.drawText ("STEM FLIP WORKSTATION", 14, headerRect.getY() + 29, 140, 10, juce::Justification::left);
        // filled PAD badge (.padBadge: glow -> accent, box-shadow 0 0 14px .35 bloom).
        // Painted on the root canvas, so the feather has real room (no clipping).
        if (! padChipRect.isEmpty())
        {
            const auto pr = padChipRect.toFloat();
            Theme::featherGlow (g, pr, 6.5f, Theme::accent.withAlpha (0.35f), 12.0f, 7);
            juce::ColourGradient bg (Theme::glow, pr.getX(), pr.getY(), Theme::accent, pr.getX(), pr.getBottom(), false);
            g.setGradientFill (bg);
            g.fillRoundedRectangle (pr, 6.5f);
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawLine (pr.getX() + 6.0f, pr.getY() + 0.9f, pr.getRight() - 6.0f, pr.getY() + 0.9f, 1.1f);
        }
    }

    // toolbar strip (under the header) — quiet panel band
    if (! toolbarRect.isEmpty())
    {
        g.setColour (Theme::panel);
        g.fillRect (toolbarRect);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawHorizontalLine (toolbarRect.getBottom() - 1, 0.0f, (float) w);
    }

    // map bezel — recessed well ring around the waveform (mockup .hero)
    {
        auto b = displayRect.toFloat().expanded (4.0f);
        Theme::paintWell (g, b, 9.0f);
    }

    // C3: slice-detail strip bezel — same recessed-well treatment (mockup .detail)
    if (! detailRect.isEmpty())
    {
        auto b = detailRect.toFloat().expanded (4.0f);
        Theme::paintWell (g, b, 8.0f);
    }

    // bottom-split panels: pads (left) + inspector (right) — raised elevation
    if (! padsRect.isEmpty()) Theme::paintRaisedPanel (g, padsRect.toFloat(), 9.0f);
    if (! inspRect.isEmpty()) Theme::paintRaisedPanel (g, inspRect.toFloat(), 9.0f);

    // inspector section separators (.isep gradient lines)
    for (int dy : inspDividers)
        Theme::paintDivider (g, (float) inspRect.getX() + 14.0f, (float) inspRect.getRight() - 14.0f, (float) dy);

    // bottom groups (.grp: translucent well box + faint border)
    for (auto& gr : grpRects)
    {
        g.setColour (Theme::well.withAlpha (0.5f));
        g.fillRoundedRectangle (gr.toFloat(), 7.0f);
        g.setColour (juce::Colours::white.withAlpha (0.045f));
        g.drawRoundedRectangle (gr.toFloat(), 7.0f, 1.0f);
    }

    // .kdiv vertical knob-group dividers
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    for (auto& kd : kdivRects)
        g.fillRect (kd);

    // section eyebrows (.slabel: tracked uppercase, t3)
    g.setFont (Theme::slabelFont());
    for (auto& e : sectionLabels)
    {
        g.setColour (Theme::t3);
        g.drawText (e.second, e.first.x, e.first.y, 220, 12, juce::Justification::left);
    }
}

void GentSamplerAudioProcessorEditor::layoutContent()
{
    // ======================================================================
    //  Phase B/C geometry — the mockup (1040x700) is the spec; regions and px
    //  below transcribe its HTML/CSS. C3 gave the hero its mockup-true 160px
    //  (was 196 while Phase B parked the Slice Detail strip's 60px budget in
    //  it) and added the strip itself — the pad grid gets that height back.
    // ======================================================================
    sectionLabels.clear();
    inspDividers.clear();
    grpRects.clear();
    kdivRects.clear();
    hdrDividers.clear();
    toolbarRect = {};
    padsRect = {};                     // pads sit bare on the frame (no panel), per mockup

    auto full = juce::Rectangle<int> (0, 0, kDesignW, kDesignH);

    // ===== HEADER (.hdr: one 48px row) =====
    auto header = full.removeFromTop (48);
    headerRect = header;
    {
        const int eyeY = header.getY() + 5;
        const int chipY = header.getY() + 11, chipH = 26;
        const int roY = header.getY() + 16, roH = 24;      // readout value band under eyebrows
        auto eyebrowAt = [&] (int x, const char* t)
        { sectionLabels.push_back ({ { x, eyeY }, juce::String (t) }); };
        auto divvAt = [&] (int x) { hdrDividers.push_back (x); };

        // wordmark + tag painted in paintContent; titleLbl retired from layout
        titleLbl.setBounds (juce::Rectangle<int>());

        // LOAD / KIT / EXPORT ghost chips after the wordmark
        int x = 146;
        loadBtn.setBounds   (x, chipY, 50, chipH); x += 53;
        kitMenu.setBounds   (x, chipY, 38, chipH); x += 41;
        exportMenu.setBounds(x, chipY, 58, chipH); x += 62;

        // centred readouts: SRC BPM (/2 value x2) | KEY | PITCH | HOST | TEMPO
        divvAt (x); x += 6;
        eyebrowAt (x + 8, "SRC BPM");
        halfBtn.setBounds (x, roY + 2, 16, 20);
        bpmLbl.setBounds  (x + 16, roY, 48, roH);
        dblBtn.setBounds  (x + 64, roY + 2, 16, 20);
        x += 84; divvAt (x); x += 6;
        eyebrowAt (x + 2, "KEY");
        keyPick.setBounds (x, roY, 58, 24);
        x += 62; divvAt (x); x += 6;
        eyebrowAt (x, "PITCH");
        masterPitch.setBounds (x, header.getY() + 12, 36, 34);
        x += 40; divvAt (x); x += 6;
        eyebrowAt (x + 6, "HOST");
        playLbl.setBounds  (x, roY, 42, roH);
        ratioLbl.setBounds (x + 42, roY + 3, 30, 18);
        x += 74; divvAt (x); x += 6;
        eyebrowAt (x + 2, "TEMPO");
        tempoMode.setBounds (x, roY, 48, 24);

        // right cluster <- from the badge inward
        int rx = header.getRight() - 14;
        padChipRect = { rx - 70, header.getY() + 10, 70, 28 };
        padRead.setBounds (padChipRect);
        rx -= 76;
        if (midiChip) midiChip->setBounds (rx - 42, chipY, 42, chipH);
        rx -= 47;
        recBtn.setBounds (rx - 68, chipY, 68, chipH);  rx -= 73;
        kbBtn.setBounds  (rx - 76, chipY, 76, chipH);  rx -= 81;
        velBtn.setBounds (rx - 66, chipY, 66, chipH);  rx -= 71;
        redoBtn.setBounds(rx - 16, chipY + 3, 16, 20); rx -= 20;
        undoBtn.setBounds(rx - 16, chipY + 3, 16, 20);
    }

    full.removeFromTop (8);   // mockup .hero margin: 8px 12px 0

    // ===== HERO (waveform map + corner overlays, mockup .hero .ov) =====
    // C3: hero claims its mockup-true 160px now that the Slice Detail strip exists
    // to take the other 60 (was 196 while Phase B parked the strip's budget here).
    auto hero = full.removeFromTop (160);
    hero.removeFromLeft (12); hero.removeFromRight (12);
    displayRect = hero;
    wave.setBounds (displayRect);
    {
        auto in = displayRect.reduced (8);
        // top-left: SEPARATE STEMS (primary) + status + filename
        sepStemsBtn.setBounds   (in.getX(), in.getY(), 122, 24);
        stemStatusLbl.setBounds (in.getX() + 128, in.getY(), 190, 24);
        fileLbl.setBounds       (in.getX() + 322, in.getY() + 4, 250, 16);
        // top-right: HQ quality caret-chip
        qualityBox.setBounds (in.getRight() - 58, in.getY(), 58, 24);
        // bottom-left: PREVIEW / SNAP / FOLLOW / SLICE
        int bx = in.getX();
        const int by = in.getBottom() - 24;
        previewBtn.setBounds (bx, by, 68, 24); bx += 72;
        snapBtn.setBounds    (bx, by, 48, 24); bx += 52;
        followBtn.setBounds  (bx, by, 58, 24); bx += 62;
        sliceMenu.setBounds  (bx, by, 52, 24);
        // bottom-right: FULL VIEW ghost
        fullBtn.setBounds (in.getRight() - 78, by, 78, 24);

        // C2: tell the wave how tall the bottom chip band is — this is the mockup's
        // own 8px .ov inset + 24px chip height (in), expressed via the layout rects
        // (displayRect.getBottom() - by) rather than re-hardcoded, so it keeps
        // tracking correctly if those two literals ever change. It lifts the wave's
        // scrollbar/end-handles clear of the chip plates instead of rendering
        // underneath them.
        wave.setBottomChromeInset (displayRect.getBottom() - by);
    }

    full.removeFromTop (6);   // mockup .detail margin: 6px 12px 0

    // ===== SLICE DETAIL STRIP (mockup .detail: dmeta 118 | dwave flex | dread 150) =====
    auto strip = full.removeFromTop (60);
    strip.removeFromLeft (12); strip.removeFromRight (12);
    detailRect = strip;
    sliceDetail.setBounds (detailRect);

    full.removeFromTop (8);
    full.removeFromBottom (12);

    // ===== BODY: pad grid (400) | inspector panel =====
    auto body = full;
    body.removeFromLeft (12); body.removeFromRight (12);
    auto grid = body.removeFromLeft (400);
    pads.setBounds (grid);
    body.removeFromLeft (10);
    inspRect = body;

    // inspector content (panel padding 14 x / 9 y)
    auto q = inspRect.reduced (14, 9);
    auto sep = [&]
    {
        q.removeFromTop (6);
        inspDividers.push_back (q.getY());
        q.removeFromTop (7);
    };
    auto slabelAt = [&] (int x, int y, const char* t)
    { sectionLabels.push_back ({ { x, y }, juce::String (t) }); };

    // a knob stack (.kwrap): slider (rotary + kv textbox at bottom) with the caption
    // Label in the getSliderLayout gap between them — mockup order knob/label/value.
    auto kwrap = [&] (juce::Rectangle<int> cell, int knobPx, juce::Slider& s, juce::Label& l)
    {
        const int tbH = juce::jmin (14, (knobPx + 9 + 14) / 4 + 4);
        const int totH = knobPx + 9 + tbH;
        s.setBounds (cell.getCentreX() - knobPx / 2, cell.getY(), knobPx, totH);
        l.setBounds (cell.getX(), cell.getY() + knobPx, cell.getWidth(), 9);
        l.setInterceptsMouseClicks (false, false);
    };

    // ---- row 1: pad header (num + meta x2 + CLEAR) | TRIGGER segmented ----
    {
        auto r1 = q.removeFromTop (44);
        padTitle.setBounds (r1.getX(), r1.getY() + 4, 42, 30);
        padMetaLbl.setBounds  (r1.getX() + 48, r1.getY() + 5, 148, 14);
        padMeta2Lbl.setBounds (r1.getX() + 48, r1.getY() + 20, 148, 12);
        clearPadBtn.setBounds (r1.getX() + 200, r1.getY() + 8, 58, 24);
        slabelAt (r1.getRight() - 294, r1.getY() + 15, "TRIGGER");
        auto seg = juce::Rectangle<int> (r1.getRight() - 240, r1.getY() + 3, 240, 38);
        const int cw = seg.getWidth() / 3;
        trigSeg[0].setBounds (seg.removeFromLeft (cw));
        trigSeg[2].setBounds (seg.removeFromRight (cw));
        trigSeg[1].setBounds (seg);
        playMode.setBounds (juce::Rectangle<int>());
        playMode.setVisible (false);
    }
    sep();

    // ---- row 2: SOURCE stems + BLEED mini-knob ----
    {
        auto r2 = q.removeFromTop (56);
        slabelAt (r2.getX(), r2.getY() + 22, "SOURCE");
        auto bleedCell = r2.removeFromRight (56);
        kwrap (bleedCell, 30, padBleed, pbL);
        r2.removeFromRight (8);
        r2.removeFromLeft (50);
        const int stW = (r2.getWidth() - 6 * 4) / 7;
        int sx = r2.getX();
        for (int i = 0; i < 7; ++i)
        {
            srcTag[(size_t) i].setBounds (sx, r2.getY() + 14, stW, 26);
            sx += stW + 4;
        }
    }
    sep();

    // ---- row 3: PLAYBACK (LOOP/REVERSE column + 4 knobs) ----
    {
        auto r3 = q.removeFromTop (84);
        auto col = r3.removeFromLeft (84);
        slabelAt (col.getX(), col.getY() + 2, "PLAYBACK");
        loopBtn.setBounds (col.getX(), col.getY() + 16, 80, 26);
        revBtn.setBounds  (col.getX(), col.getY() + 47, 80, 26);
        r3.removeFromLeft (8);
        const int kw = 70;
        const int step = (r3.getWidth() - kw) / 3;
        kwrap ({ r3.getX(),            r3.getY() + 2, kw, 80 }, 48, padPitch, ppL);
        kwrap ({ r3.getX() + step,     r3.getY() + 2, kw, 80 }, 48, padSpeed, psL);
        kwrap ({ r3.getX() + step * 2, r3.getY() + 2, kw, 80 }, 48, padLevel, plL);
        kwrap ({ r3.getX() + step * 3, r3.getY() + 2, kw, 80 }, 48, padPan,  ppanL);
    }
    sep();

    // ---- row 4: SHAPE - FILTER (CHOKE/STOP column + 5 knobs + TYPE stack) ----
    {
        auto r4 = q.removeFromTop (84);
        auto col = r4.removeFromLeft (84);
        slabelAt (col.getX(), col.getY() + 2, "SHAPE - FILTER");
        chokeBox.setBounds  (col.getX(), col.getY() + 16, 84, 26);
        chokeLbl.setBounds (juce::Rectangle<int>());
        chokeLbl.setVisible (false);
        sliceStop.setBounds (col.getX(), col.getY() + 47, 80, 26);
        r4.removeFromLeft (8);
        auto types = r4.removeFromRight (60);
        slabelAt (types.getX() + 14, types.getY() + 12, "TYPE");
        ftypeLbl.setBounds (juce::Rectangle<int>());
        ftypeLbl.setVisible (false);
        ftypeBox.setBounds (types.getX(), types.getY() + 28, 56, 24);
        r4.removeFromRight (4);
        const int kw = 64;
        // ATTACK RELEASE CRUSH | divider | CUTOFF RESO
        const int step = (r4.getWidth() - kw - 10) / 4;
        kwrap ({ r4.getX(),            r4.getY() + 2, kw, 80 }, 48, padAtt,    paL);
        kwrap ({ r4.getX() + step,     r4.getY() + 2, kw, 80 }, 48, padRel,    prL);
        kwrap ({ r4.getX() + step * 2, r4.getY() + 2, kw, 80 }, 48, padCrush,  pcL);
        // .kdiv — thin vertical divider between the SHAPE and FILTER knob groups
        kdivRects.push_back ({ r4.getX() + step * 2 + kw + (step - kw) / 2, r4.getY() + 6, 1, 66 });
        kwrap ({ r4.getX() + step * 3 + 10, r4.getY() + 2, kw, 80 }, 48, padCutoff, pcoL);
        kwrap ({ r4.getX() + step * 4 + 10, r4.getY() + 2, kw, 80 }, 48, padReso,   preL);
    }
    sep();

    // ---- row 5: bottom groups — GRANULAR (expands) + MIDI ----
    {
        auto r5 = q;                                   // remaining ~90px
        const bool grainExpanded = grainBtn.getToggleState();
        freezeBtn.setVisible (grainExpanded);
        for (auto* s : { &padGrainSize, &padGrainDens, &padGrainPos, &padGrainSpray, &padGrainPitch })
            s->setVisible (grainExpanded);
        for (auto* l : { &gsL, &gdL, &gpL, &gyL, &gtL })
            l->setVisible (grainExpanded);

        if (! grainExpanded)
        {
            // single centred line: GRANULAR grp left, MIDI grp right (mockup)
            auto line = r5.withSizeKeepingCentre (r5.getWidth(), 36);
            auto ggrp = juce::Rectangle<int> (line.getX(), line.getY(), 150, 36);
            grpRects.push_back (ggrp);
            slabelAt (ggrp.getX() + 8, ggrp.getY() + 14, "GRANULAR");
            grainBtn.setBounds (ggrp.getX() + 78, ggrp.getY() + 5, 60, 26);
            auto mgrp = juce::Rectangle<int> (line.getRight() - 330, line.getY(), 330, 36);
            grpRects.push_back (mgrp);
            slabelAt (mgrp.getX() + 8, mgrp.getY() + 14, "MIDI");
            transcribeBtn.setBounds      (mgrp.getX() + 40,  mgrp.getY() + 5, 92, 26);
            transcribeQuantBtn.setBounds (mgrp.getX() + 136, mgrp.getY() + 5, 78, 26);
            if (transcribeChip) transcribeChip->setBounds (mgrp.getRight() - 48, mgrp.getY() + 5, 42, 26);
            transcribeLbl.setBounds (mgrp.getX() + 218, mgrp.getY() + 5, 58, 26);
        }
        else
        {
            // line 1: full-width granular strip (GRAIN + FREEZE + 5 mini-knobs)
            auto line1 = r5.removeFromTop (58);
            grpRects.push_back (line1);
            slabelAt (line1.getX() + 8, line1.getY() + 24, "GRANULAR");
            grainBtn.setBounds  (line1.getX() + 62,  line1.getY() + 15, 56, 26);
            freezeBtn.setBounds (line1.getX() + 122, line1.getY() + 15, 62, 26);
            const int kx0 = line1.getX() + 194;
            const int kstep = (line1.getRight() - 6 - kx0) / 5;
            kwrap ({ kx0,             line1.getY() + 2, kstep - 6, 54 }, 32, padGrainSize,  gsL);
            kwrap ({ kx0 + kstep,     line1.getY() + 2, kstep - 6, 54 }, 32, padGrainDens,  gdL);
            kwrap ({ kx0 + kstep * 2, line1.getY() + 2, kstep - 6, 54 }, 32, padGrainPos,   gpL);
            kwrap ({ kx0 + kstep * 3, line1.getY() + 2, kstep - 6, 54 }, 32, padGrainSpray, gyL);
            kwrap ({ kx0 + kstep * 4, line1.getY() + 2, kstep - 6, 54 }, 32, padGrainPitch, gtL);
            // line 2: MIDI grp compressed right
            r5.removeFromTop (4);
            auto line2 = r5.removeFromTop (28);
            auto mgrp = juce::Rectangle<int> (line2.getRight() - 330, line2.getY(), 330, 28);
            grpRects.push_back (mgrp);
            slabelAt (mgrp.getX() + 8, mgrp.getY() + 10, "MIDI");
            transcribeBtn.setBounds      (mgrp.getX() + 40,  mgrp.getY() + 1, 92, 26);
            transcribeQuantBtn.setBounds (mgrp.getX() + 136, mgrp.getY() + 1, 78, 26);
            if (transcribeChip) transcribeChip->setBounds (mgrp.getRight() - 48, mgrp.getY() + 1, 42, 26);
            transcribeLbl.setBounds (mgrp.getX() + 218, mgrp.getY() + 1, 58, 26);
        }
    }

    granularShown = grainBtn.getToggleState();
    root.repaint();   // grp boxes + section labels live on the root canvas — no stale paint on reflow
}

void GentSamplerAudioProcessorEditor::paint (juce::Graphics& g)
{
    // the scaled root paints the whole UI; fill behind it so any sub-pixel edge is dark
    g.fillAll (Theme::canvas.darker (0.25f));
}

void GentSamplerAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
    // mockup .plugin::before — radial vignette. Built ONCE as a square tile and drawn
    // stretched to the window: stretching makes it elliptical and no resize rebuild.
    if (! vignetteImg.isValid())
    {
        constexpr int N = 256;
        vignetteImg = juce::Image (juce::Image::ARGB, N, N, true);
        juce::Graphics vg (vignetteImg);
        juce::ColourGradient rad (juce::Colours::transparentBlack, N * 0.5f, N * 0.35f,
                                  juce::Colours::black.withAlpha (0.42f), N * 0.5f, N * 0.35f + N * 0.95f, true);
        rad.addColour (0.58, juce::Colours::transparentBlack);
        vg.setGradientFill (rad);
        vg.fillAll();
    }
    g.drawImage (vignetteImg, getLocalBounds().toFloat());

    // mockup .plugin::after — film grain at ~3.5% (pre-rendered 96x96 tile)
    if (! noiseImg.isValid())
    {
        juce::Random rng (0x67656e74);          // fixed seed: identical grain every run
        noiseImg = juce::Image (juce::Image::ARGB, 96, 96, true);
        for (int yy = 0; yy < 96; ++yy)
            for (int xx = 0; xx < 96; ++xx)
            {
                const auto v = (juce::uint8) rng.nextInt (256);
                noiseImg.setPixelAt (xx, yy, juce::Colour (v, v, v));
            }
    }
    g.setTiledImageFill (noiseImg, 0, 0, 0.035f);
    g.fillRect (getLocalBounds());
}

void GentSamplerAudioProcessorEditor::resized()
{
    p.editorW = getWidth();
    p.editorH = getHeight();
    // hold the whole UI at the fixed design size, then scale it to the window as one
    // unit — aspect-locked by the constrainer, so sx == sy (uniform zoom, no distortion)
    root.setTransform (juce::AffineTransform());
    root.setBounds (0, 0, kDesignW, kDesignH);
    root.setTransform (juce::AffineTransform::scale ((float) getWidth()  / (float) kDesignW,
                                                     (float) getHeight() / (float) kDesignH));
}

bool GentSamplerAudioProcessorEditor::keyPressed (const juce::KeyPress& k, juce::Component*)
{
    const bool ctrl = k.getModifiers().isCommandDown() || k.getModifiers().isCtrlDown();
    if (ctrl)
    {
        if (k.getKeyCode() == 'Z' && k.getModifiers().isShiftDown()) { p.redo(); return true; }
        if (k.getKeyCode() == 'Z') { p.undo(); return true; }
        if (k.getKeyCode() == 'Y') { p.redo(); return true; }
        return false;
    }

    // F4: arrow-key nudge + FL fallback (SLICE_FEEL_TASK.md F4). Both surfaces call
    // setWantsKeyboardFocus(true) so clicking either puts focus inside the plugin;
    // this is the ONE place the nudge/arm decision lives (ground rule 1) — reached via
    // JUCE's normal focused-component -> parent-chain KeyListener bubbling, which is
    // untouched by the new setWantsKeyboardFocus calls (AC-F4.7).
    const int code = k.getKeyCode();
    if (code == juce::KeyPress::upKey)   { armedHandle = HandleDragEngine::Handle::cue; armedHandlePad = p.selectedPad.load(); sliceDetail.setArmedHandle (armedHandle); return true; }
    if (code == juce::KeyPress::downKey) { armedHandle = HandleDragEngine::Handle::end; armedHandlePad = p.selectedPad.load(); sliceDetail.setArmedHandle (armedHandle); return true; }

    // Comma/period matched by KEY CODE, not text character: getKeyCode() for these two
    // keys is derived from the unmodified scan code (JUCE's doKeyChar on Windows), so it
    // stays ',' / '.' whether or not Shift is held (Shift+',' types '<' as TEXT but the
    // physical key code is unchanged) — required so the fallback's Shift-fine-mode
    // behaves identically to the arrow keys (SLICE_FEEL_TASK.md F4 "same Shift behavior").
    const bool fine = k.getModifiers().isShiftDown();
    if (code == juce::KeyPress::rightKey || code == '.') { nudgeHandle (true,  fine); return true; }
    if (code == juce::KeyPress::leftKey  || code == ',') { nudgeHandle (false, fine); return true; }

    return false;
}

// F4: single shared nudge-edit path — CUE via setCue(pad, s, false), END via
// applyEndHandleDrag (both already the drag path's own edit calls, no third
// implementation). Snap never applies to nudges (SLICE_FEEL_TASK.md F4). Increment
// is read from the strip's own current zoom (stripSpp()), matching drag resolution
// 1:1 per spec. No-op when the selected pad is unassigned or no source is loaded.
void GentSamplerAudioProcessorEditor::nudgeHandle (bool right, bool fine)
{
    const int pad = p.selectedPad.load();
    if (p.getCue (pad) < 0 || p.getSource() == nullptr)
        return;

    // Final-review fix: the timer resets armed state on pad-selection change at
    // only 15Hz, leaving a ~67ms window where a nudge fired right after clicking
    // a new pad would hit the PREVIOUS pad's armed handle. Self-heal at point of
    // use: a stale armedHandlePad means selection changed since arming — apply
    // the spec's default (CUE) for the new pad before editing anything.
    if (armedHandlePad != pad)
    {
        armedHandle    = HandleDragEngine::Handle::cue;
        armedHandlePad = pad;
        sliceDetail.setArmedHandle (armedHandle);
    }

    const double spp = sliceDetail.stripSpp();
    const juce::int64 inc = fine ? juce::jmax ((juce::int64) 1, (juce::int64) juce::roundToInt (spp / 10.0))
                                  : juce::jmax ((juce::int64) 1, (juce::int64) juce::roundToInt (spp));
    const juce::int64 delta = right ? inc : -inc;

    // Undo coalescing: a burst of nudges within 600ms of the previous nudge edit
    // shares one undo entry (AC-F4.4); the first edit after a 600ms gap pushes a
    // fresh undo snapshot.
    const juce::uint32 now = juce::Time::getMillisecondCounter();
    const bool coalesce = nudgeUndoPending && (now - lastNudgeMs) < 600;
    if (! coalesce)
        p.pushUndo();
    lastNudgeMs = now;
    nudgeUndoPending = true;

    if (armedHandle == HandleDragEngine::Handle::end)
    {
        const int end = p.getEffectiveCueEnd (pad);
        const int proposed = (int) ((juce::int64) end + delta);
        const int tol = (int) (8.0 * spp);
        applyEndHandleDrag (p, pad, proposed, tol);   // snap NEVER applies to nudges
    }
    else
    {
        const int cue = p.getCue (pad);
        const int proposed = (int) ((juce::int64) cue + delta);
        p.setCue (pad, proposed, false);              // snap NEVER applies to nudges
    }
    ++p.uiDirty;
}

bool GentSamplerAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
    {
        const auto lower = f.toLowerCase();
        if (lower.endsWith (".wav") || lower.endsWith (".mp3") || lower.endsWith (".aif")
            || lower.endsWith (".aiff") || lower.endsWith (".flac") || lower.endsWith (".ogg")
            || lower.endsWith (".gentkit"))
            return true;
    }
    return false;
}

void GentSamplerAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (files.isEmpty()) return;
    const juce::File f (files[0]);
    if (f.hasFileExtension ("gentkit"))
        p.loadKit (f);
    else
        p.loadFile (f);
}

void GentSamplerAudioProcessorEditor::attachPad (int pad)
{
    attachedPad = pad;
    aPitch.reset();
    aLevel.reset();
    aAtt.reset();
    aRel.reset();
    aMode.reset();
    aSlice.reset();
    aCrush.reset();
    aSpeed.reset();
    aPan.reset();
    aChoke.reset();
    aCutoff.reset();
    aReso.reset();
    aFType.reset();
    aLoop.reset();
    aReverse.reset();
    aBleed.reset();
    aGrainOn.reset();
    aGrainFreeze.reset();
    aGrainSize.reset();
    aGrainDens.reset();
    aGrainPos.reset();
    aGrainSpray.reset();
    aGrainPitch.reset();
    aPitch = std::make_unique<SA> (p.apvts, pid (pad, "pitch"), padPitch);
    aLevel = std::make_unique<SA> (p.apvts, pid (pad, "level"), padLevel);
    aAtt   = std::make_unique<SA> (p.apvts, pid (pad, "att"),   padAtt);
    aRel   = std::make_unique<SA> (p.apvts, pid (pad, "rel"),   padRel);
    aMode  = std::make_unique<CA> (p.apvts, pid (pad, "mode"),  playMode);
    aSlice = std::make_unique<BA> (p.apvts, pid (pad, "slice"), sliceStop);
    aCrush = std::make_unique<SA> (p.apvts, pid (pad, "crush"), padCrush);
    aSpeed = std::make_unique<SA> (p.apvts, pid (pad, "speed"), padSpeed);
    aPan   = std::make_unique<SA> (p.apvts, pid (pad, "pan"),   padPan);
    aChoke = std::make_unique<CA> (p.apvts, pid (pad, "choke"), chokeBox);
    aCutoff = std::make_unique<SA> (p.apvts, pid (pad, "cutoff"), padCutoff);
    aReso   = std::make_unique<SA> (p.apvts, pid (pad, "reso"),   padReso);
    aFType  = std::make_unique<CA> (p.apvts, pid (pad, "ftype"),  ftypeBox);
    aLoop    = std::make_unique<BA> (p.apvts, pid (pad, "loop"),    loopBtn);
    aReverse = std::make_unique<BA> (p.apvts, pid (pad, "reverse"), revBtn);
    aBleed   = std::make_unique<SA> (p.apvts, pid (pad, "bleed"),   padBleed);
    aGrainOn     = std::make_unique<BA> (p.apvts, pid (pad, "grainOn"),     grainBtn);
    aGrainFreeze = std::make_unique<BA> (p.apvts, pid (pad, "grainFreeze"), freezeBtn);
    aGrainSize   = std::make_unique<SA> (p.apvts, pid (pad, "grainSize"),   padGrainSize);
    aGrainDens   = std::make_unique<SA> (p.apvts, pid (pad, "grainDens"),   padGrainDens);
    aGrainPos    = std::make_unique<SA> (p.apvts, pid (pad, "grainPos"),    padGrainPos);
    aGrainSpray  = std::make_unique<SA> (p.apvts, pid (pad, "grainSpray"),  padGrainSpray);
    aGrainPitch  = std::make_unique<SA> (p.apvts, pid (pad, "grainPitch"),  padGrainPitch);
    padTitle.setText (juce::String (pad + 1), juce::dontSendNotification);
    padTitle.setColour (juce::Label::textColourId, Theme::accent);   // .padNum is accent per mockup

    // mockup value formats — set AFTER the attachments: SliderAttachment overwrites
    // textFromValueFunction/valueFromTextFunction with the parameter's own strings on
    // every rebind, so this is the only spot where custom formats survive pad switches.
    padPan.textFromValueFunction = [] (double v)
    {
        const int n = juce::roundToInt (std::abs (v) * 100.0);
        if (n == 0) return juce::String ("C");
        return (v < 0 ? juce::String ("L") : juce::String ("R")) + juce::String (n);
    };
    padPan.valueFromTextFunction = [] (const juce::String& t)
    {
        auto s = t.trim().toUpperCase();
        if (s.startsWithChar ('C')) return 0.0;
        if (s.startsWithChar ('L') || s.startsWithChar ('R'))
        {
            const double mag = s.retainCharacters ("0123456789.").getDoubleValue() / 100.0;
            return s.startsWithChar ('L') ? -mag : mag;
        }
        return juce::jlimit (-1.0, 1.0, s.retainCharacters ("-0123456789.").getDoubleValue());
    };
    padPitch.textFromValueFunction = [] (double v)
    {
        const int i = juce::roundToInt (v);
        return (i > 0 ? "+" : "") + juce::String (i) + " st";
    };
    padPitch.valueFromTextFunction = [] (const juce::String& t)
    { return t.retainCharacters ("-0123456789.").getDoubleValue(); };
    padPan.updateText();
    padPitch.updateText();
}

void GentSamplerAudioProcessorEditor::timerCallback()
{
    const int sel = p.selectedPad.load();
    if (sel != attachedPad)
        attachPad (sel);

    // F4: the arrow-nudge armed handle resets to CUE whenever the selected pad changes
    // (SLICE_FEEL_TASK.md F4 — "defaults to CUE and resets to CUE on pad-selection
    // change"), covering pad-grid clicks / MIDI-triggered selection, not just mouse
    // grabs on the two edit surfaces (those set armedHandlePad directly, so this is a
    // no-op right after a same-tick grab).
    if (sel != armedHandlePad)
    {
        armedHandlePad = sel;
        armedHandle = HandleDragEngine::Handle::cue;
        sliceDetail.setArmedHandle (armedHandle);
    }

    const double srcB = p.getEffectiveSourceBpm();

    if (! bpmLbl.isBeingEdited())
    {
        if (p.analyzing.load())
            bpmLbl.setText ("ANALYZING...", juce::dontSendNotification);
        else if (srcB > 0.0)
            bpmLbl.setText (juce::String (srcB, 1)
                            + (p.getSourceBpmOverride() > 1.0 ? "*" : ""),
                            juce::dontSendNotification);
        else
            bpmLbl.setText ("--", juce::dontSendNotification);
    }

    if (! playLbl.isBeingEdited())
    {
        const int tm = (int) std::round (p.apvts.getRawParameterValue ("tempoMode")->load());
        double playB = 0.0;
        float alpha = 1.0f;
        if (tm == 0)      { playB = srcB; alpha = 0.45f; }
        else if (tm == 1) { playB = p.getHostBpm(); alpha = 0.65f; }
        else              { playB = (double) p.apvts.getRawParameterValue ("customBpm")->load(); }
        playLbl.setColour (juce::Label::textColourId, Theme::t1.withAlpha (alpha));   // theme, kept per-tick (mode-dependent alpha)
        playLbl.setText (playB > 0.0 ? juce::String (playB, 1) : "--",
                         juce::dontSendNotification);
    }

    {
        const double ratio = p.currentTargetSpeed();
        ratioLbl.setText (std::abs (ratio - 1.0) < 0.005 ? juce::String ("x1.00")
                                                         : "x" + juce::String (ratio, 2),
                          juce::dontSendNotification);
    }

    // key picker shows the ACTIVE key; small label beside it shows base + offset
    const auto key = p.getDetectedKey();
    const int from = rootIndexOf (key);
    const bool minor = key.containsIgnoreCase ("min");

    if (key != keyItemsBuiltFor)
    {
        keyItemsBuiltFor = key;
        keyPick.clear (juce::dontSendNotification);
        if (from >= 0)
            for (int i = 0; i < 12; ++i)
                keyPick.addItem (juce::String (kRootNames[i]) + (minor ? " Min" : " Maj"), i + 1);
    }

    if (from >= 0)
    {
        const int mp = (int) std::round (p.apvts.getRawParameterValue ("masterPitch")->load());
        const int activeRoot = ((from + mp) % 12 + 12) % 12;
        if (keyPick.getSelectedId() != activeRoot + 1)
            keyPick.setSelectedId (activeRoot + 1, juce::dontSendNotification);
    }
    else
    {
        if (keyPick.getNumItems() > 0)
            keyPick.clear (juce::dontSendNotification);
        keyItemsBuiltFor.clear();
    }

    undoBtn.setEnabled (p.canUndo());
    redoBtn.setEnabled (p.canRedo());
    fileLbl.setText (p.getFileName(), juce::dontSendNotification);
    padRead.setText ("PAD " + juce::String (sel + 1), juce::dontSendNotification);

    // SELECTED PAD detail: big source-coloured number + cue line + meta line
    {
        padTitle.setText (juce::String (sel + 1), juce::dontSendNotification);
        padTitle.setColour (juce::Label::textColourId, Theme::accent);   // .padNum is accent per mockup
        const bool assigned = p.getCue (sel) >= 0;
        if (assigned)
        {
            auto src = p.getSource();
            const double sr = (src != nullptr) ? src->sampleRate : 0.0;
            const double dur = sr > 0.0 ? (double) (p.getEffectiveCueEnd (sel) - p.getCue (sel)) / sr : 0.0;
            juce::String sname = "FULL";
            const auto m = p.getPadStemMask (sel);
            if (m != 0)
            {
                static const char* n[6] = { "DRUMS", "BASS", "VOX", "GTR", "PNO", "OTHER" };
                sname = "MIX";
                if ((std::uint8_t) (m & (m - 1)) == 0)
                    for (int k = 0; k < 6; ++k) if (m == (std::uint8_t) (1u << k)) sname = n[k];
            }
            // start cue -> end cue range. The end is another pad's cue when the
            // region runs up to the next cue, otherwise the sample end.
            const juce::String lenTag = p.isOpenSlice (sel) ? juce::String ("OPEN")
                                                            : (juce::String (dur, 1) + "s");
            padMetaLbl.setText (lenTag + " \xc2\xb7 " + keyPick.getText()
                                + " \xc2\xb7 " + sname, juce::dontSendNotification);
            // .m2 second meta line: SLICE n - CUE m:ss.s  (middot concatenated —
            // String::formatted mangles UTF-8 bytes in its format string)
            const double cueSec = sr > 0.0 ? (double) p.getCue (sel) / sr : 0.0;
            padMeta2Lbl.setText ("SLICE " + juce::String (sel + 1) + " \xc2\xb7 CUE "
                                 + juce::String::formatted ("%d:%04.1f",
                                       (int) (cueSec / 60.0),
                                       cueSec - 60.0 * (int) (cueSec / 60.0)),
                                 juce::dontSendNotification);
        }
        else
        {
            padMetaLbl.setText ("no region assigned", juce::dontSendNotification);
            padMeta2Lbl.setText ("", juce::dontSendNotification);
        }
    }

    // TRIGGER icon buttons mirror the (hidden) playMode combo
    {
        const int mode = playMode.getSelectedItemIndex();
        for (int i = 0; i < 3; ++i)
            trigSeg[(size_t) i].setActive (mode == i);
    }

    // PAD SOURCE tags reflect the selected pad's mask
    {
        const bool full = p.isPadFull (sel);
        const bool enable = p.getCue (sel) >= 0;
        for (int i = 0; i < 7; ++i)
        {
            const bool on = (i == 0) ? full : (! full && p.isPadStemOn (sel, i - 1));
            if (srcTag[(size_t) i].getToggleState() != on)
                srcTag[(size_t) i].setToggleState (on, juce::dontSendNotification);
            srcTag[(size_t) i].setEnabled (enable);
        }
        // BLEED only does something when stems are filtered (not FULL) and assigned —
        // applies to granular too (grains read the selected stem mix + bleed)
        padBleed.setEnabled (enable && ! full);

        // GRANULAR knobs are live only when the pad is assigned and GRAIN is on
        const bool gOn = grainBtn.getToggleState();
        if (gOn != granularShown)     // GRAIN toggled (or pad switched) -> collapse/expand + reflow
            layoutContent();          // relayout at design size; the root transform is unchanged
        grainBtn.setEnabled (enable);
        freezeBtn.setEnabled (enable && gOn);
        padGrainSize.setEnabled  (enable && gOn);
        padGrainDens.setEnabled  (enable && gOn);
        padGrainPos.setEnabled   (enable && gOn);
        padGrainSpray.setEnabled (enable && gOn);
        padGrainPitch.setEnabled (enable && gOn);

        // TRANSCRIBE -> MIDI: live status + the drag chip appears only when ready
        const bool tBusy  = p.isTranscribing();
        const bool tReady = p.transcriptionReady();
        transcribeBtn.setEnabled (enable && ! tBusy);
        if (transcribeQuantBtn.getToggleState() != p.getTranscribeQuantize())
            transcribeQuantBtn.setToggleState (p.getTranscribeQuantize(), juce::dontSendNotification);
        transcribeLbl.setText (p.getTranscribeStatus(), juce::dontSendNotification);
        if (transcribeChip && transcribeChip->isVisible() != tReady)
            transcribeChip->setVisible (tReady);

        clearPadBtn.setEnabled (enable);

        // FILTER knobs read as disabled (muted arc, no glow) while TYPE = Off
        const bool filtOn = ftypeBox.getSelectedId() > 1;
        padCutoff.setEnabled (enable && filtOn);
        padReso.setEnabled   (enable && filtOn);
        if (velBtn.getToggleState() != p.getVelToLevel())
            velBtn.setToggleState (p.getVelToLevel(), juce::dontSendNotification);
    }

    // active-state tint: the LnF paints a filled chip whenever buttonColourId is
    // specified (see drawButtonBackground) — set while active, removed when idle
    if (p.isPreviewing())
    {
        previewBtn.setButtonText ("STOP");
        previewBtn.setColour (juce::TextButton::buttonColourId, Theme::previewActive);
    }
    else
    {
        previewBtn.setButtonText ("PREVIEW");
        previewBtn.removeColour (juce::TextButton::buttonColourId);
    }

    if (p.isCapturing())
    {
        recBtn.setButtonText ("STOP (" + juce::String (p.capturedEventCount()) + ")");
        recBtn.setColour (juce::TextButton::buttonColourId, Theme::recActive);
    }
    else
    {
        recBtn.setButtonText ("REC MIDI");
        recBtn.removeColour (juce::TextButton::buttonColourId);
    }

    // ---- stem section live state ----
    {
        const bool busy  = p.isSeparating();
        const bool dling = p.isDownloadingModels();
        const bool ready = p.hasStems();
        sepStemsBtn.setEnabled (! busy);
        qualityBox.setEnabled (! busy);
        if (qualityBox.getSelectedId() != juce::jlimit (0, 2, p.getStemQuality()) + 1)
            qualityBox.setSelectedId (juce::jlimit (0, 2, p.getStemQuality()) + 1, juce::dontSendNotification);

        if (dling)
            stemStatusLbl.setText (p.getStemStatus(), juce::dontSendNotification);   // "Downloading models 42%"
        else if (busy)
            stemStatusLbl.setText ("Separating " + juce::String (juce::roundToInt (p.getStemProgress() * 100.0f)) + "%",
                                   juce::dontSendNotification);
        else if (ready)
            stemStatusLbl.setText (p.getStemStatus().isNotEmpty() ? p.getStemStatus() : juce::String ("Stems ready"),
                                   juce::dontSendNotification);
        else
            stemStatusLbl.setText ("No stems yet", juce::dontSendNotification);
    }
}
