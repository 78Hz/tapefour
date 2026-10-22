#include "SetupWindow.h"

namespace tf
{
namespace
{
//==============================================================================
class AudioPanel : public juce::Component,
                   private juce::ChangeListener,
                   private juce::ComponentListener
{
public:
    explicit AudioPanel (AudioEngine& e)
        : engine (e),
          // MIDI lives on its own tab, not in JUCE's selector.
          selector (e.deviceManager, 0, 16, 2, 16, false, false, false, false)
    {
        addAndMakeVisible (selector);
        selector.addComponentListener (this);

        for (auto* l : { &mainLabel, &phonesLabel, &monitorLabel, &latencyLabel })
        {
            l->setFont (DeckLookAndFeel::get().cond (17.0f));
            l->setColour (juce::Label::textColourId, Pal::muted);
            l->setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (l);
        }

        mainOut.onChange   = [this] { engine.mainOutPair = std::max (0, mainOut.getSelectedItemIndex()); engine.saveSettings(); };
        phonesOut.onChange = [this] { engine.phonesPair = phonesOut.getSelectedItemIndex() - 1; engine.saveSettings(); };
        monitor.addItem ("Auto: hear the input when stopped or recording, the tape when playing", 1);
        monitor.addItem ("Always hear the input on armed tracks", 2);
        monitor.setSelectedItemIndex (engine.monitorMode.load(), juce::dontSendNotification);
        monitor.onChange = [this] { engine.monitorMode = monitor.getSelectedItemIndex(); engine.saveSettings(); };

        latency.setRange (-50.0, 50.0, 1.0);
        latency.setTextValueSuffix (" ms");
        latency.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 24);
        latency.setValue (engine.tapeEngine.params.latencyOffsetMs.load(), juce::dontSendNotification);
        latency.onValueChange = [this] { engine.tapeEngine.params.latencyOffsetMs = (int) latency.getValue(); };
        latency.onDragEnd = [this] { engine.saveSettings(); };
        latency.setTooltip ("Overdubs are shifted earlier by the driver's reported latency. "
                            "If they still land late or early, adjust here.");

        for (auto* c : std::initializer_list<juce::Component*> { &mainOut, &phonesOut, &monitor, &latency })
            addAndMakeVisible (c);

        engine.deviceManager.addChangeListener (this);
        refreshOutputs();
    }

    ~AudioPanel() override
    {
        engine.deviceManager.removeChangeListener (this);
        selector.removeComponentListener (this);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        // The device selector sizes itself to its contents; we only set its width.
        selector.setBounds (r.getX(), r.getY(), r.getWidth(), selectorHeight());
        r.removeFromTop (selectorHeight() + 12);

        auto row = [&] (juce::Label& l, juce::Component& c)
        {
            auto line = r.removeFromTop (30);
            l.setBounds (line.removeFromLeft (200));
            line.removeFromLeft (10);
            c.setBounds (line);
            r.removeFromTop (8);
        };
        row (mainLabel, mainOut);
        row (phonesLabel, phonesOut);
        row (monitorLabel, monitor);
        row (latencyLabel, latency);
    }

    int selectorHeight() const { return selector.getHeight() > 0 ? selector.getHeight() : 360; }
    int preferredHeight() const { return selectorHeight() + 24 + 12 + 4 * 38 + 24; }
    std::function<void()> onPreferredHeightChanged;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refreshOutputs(); }

    void componentMovedOrResized (juce::Component&, bool, bool wasResized) override
    {
        if (! wasResized) return;
        // Re-lay out later, outside the selector's own resize, so the two never chase each other.
        juce::Component::SafePointer<AudioPanel> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe == nullptr) return;
            if (safe->onPreferredHeightChanged) safe->onPreferredHeightChanged();
            else safe->resized();
        });
    }

    void refreshOutputs()
    {
        auto pairs = engine.getOutputPairNames();
        mainOut.clear (juce::dontSendNotification);
        phonesOut.clear (juce::dontSendNotification);
        phonesOut.addItem ("Same as the stereo out", 1);
        for (int i = 0; i < pairs.size(); ++i)
        {
            mainOut.addItem (pairs[i], i + 1);
            phonesOut.addItem (pairs[i], i + 2);
        }
        mainOut.setSelectedItemIndex (juce::jlimit (0, std::max (0, pairs.size() - 1), engine.mainOutPair.load()), juce::dontSendNotification);
        phonesOut.setSelectedItemIndex (engine.phonesPair.load() + 1, juce::dontSendNotification);
    }

    AudioEngine& engine;
    juce::AudioDeviceSelectorComponent selector;
    juce::Label mainLabel { {}, "Stereo out" }, phonesLabel { {}, "Phones" },
                monitorLabel { {}, "Input monitoring" }, latencyLabel { {}, "Overdub timing" };
    juce::ComboBox mainOut, phonesOut, monitor;
    juce::Slider latency { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
};

//==============================================================================
/** Controllers in, synths out, and whether the deck comes back with each tape. */
class MidiPanel : public juce::Component,
                  private juce::Timer
{
public:
    explicit MidiPanel (AudioEngine& e) : engine (e)
    {
        recall.setToggleState (engine.recallDeckWithTape.load(), juce::dontSendNotification);
        recall.onClick = [this] { engine.recallDeckWithTape = recall.getToggleState(); engine.saveSettings(); };
        addAndMakeVisible (recall);
        recallNote.setText ("Every tape remembers the deck it was recorded on: sources, instruments and their settings, "
                            "MIDI routing, EQ, pan, faders, master, speed and pitch. With this ticked, loading a tape "
                            "puts the deck back the way it was. Unticked, the deck stays as it is.", juce::dontSendNotification);
        style (recallNote, Pal::faint, 15.0f);
        recallNote.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (recallNote);

        rebuild();
        startTimer (1000);
    }

    int preferredHeight() const
    {
        return 12 + 34 + 26 + (int) inRows.size() * 36 + 40 + 34 + 26 + std::max (1, (int) outRows.size()) * 36 + 40 + 34 + 36 + 70 + 12;
    }
    std::function<void()> onPreferredHeightChanged;

    void paint (juce::Graphics& g) override
    {
        auto& lf = DeckLookAndFeel::get();
        g.setColour (Pal::cream);
        g.setFont (lf.cond (19.0f, true).withExtraKerningFactor (0.12f));
        for (auto& h : headings)
            g.drawText (h.text, h.area, juce::Justification::bottomLeft);
        g.setColour (Pal::line);
        for (auto& h : headings)
            g.fillRect (h.area.getX(), h.area.getBottom() + 2, getWidth() - 2 * h.area.getX(), 1);
        g.setColour (Pal::faint);
        g.setFont (lf.cond (13.0f).withExtraKerningFactor (0.2f));
        for (auto& c : columns)
            g.drawText (c.text, c.area, juce::Justification::centredLeft);
    }

    void resized() override
    {
        headings.clear();
        columns.clear();
        auto r = getLocalBounds().reduced (16, 12);

        auto heading = [&] (const juce::String& text)
        {
            headings.push_back ({ text, r.removeFromTop (30) });
            r.removeFromTop (4);
        };
        auto cols = [&] (std::initializer_list<std::pair<const char*, int>> list)
        {
            auto line = r.removeFromTop (22);
            int x = line.getX();
            for (auto& [text, cx] : list)
                columns.push_back ({ text, { x + cx, line.getY(), 200, line.getHeight() } });
            r.removeFromTop (4);
        };

        heading ("CONTROLLERS IN");
        cols ({ { "USE", 0 }, { "CONTROLLER", 60 }, { "MPE", 400 }, { "BEND RANGE", 480 } });
        for (auto& row : inRows)
        {
            auto line = r.removeFromTop (32);
            row->use.setBounds (line.removeFromLeft (60).withSizeKeepingCentre (30, 30));
            row->name.setBounds (line.removeFromLeft (340));
            row->mpe.setBounds (line.removeFromLeft (80).withSizeKeepingCentre (30, 30));
            row->bend.setBounds (line.removeFromLeft (64).reduced (0, 2));
            row->bendUnit.setBounds (line.removeFromLeft (110));
            r.removeFromTop (4);
        }
        r.removeFromTop (10);
        hintIn.setBounds (r.removeFromTop (26));

        r.removeFromTop (14);
        heading ("HARDWARE SYNTHS OUT");
        cols ({ { "USE", 0 }, { "MIDI OUT PORT", 60 }, { "NAME ON THE DECK", 400 } });
        for (auto& row : outRows)
        {
            auto line = r.removeFromTop (32);
            row->use.setBounds (line.removeFromLeft (60).withSizeKeepingCentre (30, 30));
            row->name.setBounds (line.removeFromLeft (340));
            row->alias.setBounds (line.removeFromLeft (260).reduced (0, 2));
            r.removeFromTop (4);
        }
        if (outRows.empty())
            noOutputs.setBounds (r.removeFromTop (32).withTrimmedLeft (60));
        r.removeFromTop (10);

        r.removeFromTop (14);
        heading ("DECK");
        recall.setBounds (r.removeFromTop (32));
        r.removeFromTop (4);
        recallNote.setBounds (r.removeFromTop (70).withTrimmedLeft (30));
        repaint();
    }

private:
    struct Heading { juce::String text; juce::Rectangle<int> area; };

    struct InRow
    {
        int slot = 0;
        juce::ToggleButton use, mpe;
        juce::Label name, bendUnit { {}, "semitones" };
        juce::TextEditor bend;
    };
    struct OutRow
    {
        int slot = 0;
        juce::ToggleButton use;
        juce::Label name;
        juce::TextEditor alias;
    };

    static void style (juce::Label& l, juce::Colour c, float size = 17.0f)
    {
        l.setFont (DeckLookAndFeel::get().cond (size));
        l.setColour (juce::Label::textColourId, c);
        l.setMinimumHorizontalScale (0.8f);
    }

    static void styleEditor (juce::TextEditor& ed)
    {
        ed.setFont (DeckLookAndFeel::get().mono (16.0f));
        ed.setColour (juce::TextEditor::backgroundColourId, Pal::darker);
        ed.setColour (juce::TextEditor::textColourId, Pal::lcdText);
        ed.setColour (juce::TextEditor::outlineColourId, Pal::line);
        ed.setColour (juce::TextEditor::focusedOutlineColourId, Pal::orange);
        ed.setIndents (6, 5);
    }

    juce::String signature() const
    {
        juce::String sig;
        for (auto& i : engine.getMidiInputs())  sig << "i" << i.slot << i.id << (int) i.available;
        for (auto& o : engine.getMidiOutputs()) sig << "o" << o.slot << o.id << (int) o.available;
        return sig;
    }

    void timerCallback() override
    {
        // Pick up controllers and synths plugged in while the window is open,
        // but never pull the rows out from under someone typing.
        if (auto* f = juce::Component::getCurrentlyFocusedComponent(); f != nullptr && isParentOf (f))
            return;
        if (signature() != lastSignature)
            rebuild();
    }

    void rebuild()
    {
        lastSignature = signature();
        inRows.clear();
        outRows.clear();
        auto& eng = engine;

        for (auto& d : engine.getMidiInputs())
        {
            auto row = std::make_unique<InRow>();
            row->slot = d.slot;
            row->name.setText (d.name + (d.available ? juce::String() : juce::String ("   (not connected)")), juce::dontSendNotification);
            style (row->name, d.available ? Pal::cream : Pal::faint);
            style (row->bendUnit, Pal::muted, 15.0f);

            row->use.setToggleState (d.active, juce::dontSendNotification);
            row->use.setTooltip ("Listen to this controller. Each track chooses which controller plays it on its patch card.");
            auto* raw = row.get();
            row->use.onClick = [raw, &eng]
            {
                auto on = raw->use.getToggleState();
                auto slot = raw->slot;
                // Open or close the port after the click has finished.
                juce::MessageManager::callAsync ([&eng, slot, on] { eng.setMidiInputActive (slot, on); });
            };

            row->mpe.setToggleState (d.mpe, juce::dontSendNotification);
            row->mpe.setTooltip ("MPE controller (Osmose, Seaboard, LinnStrument...): per-note bend, pressure and timbre");
            styleEditor (row->bend);
            row->bend.setInputRestrictions (2, "0123456789");
            row->bend.setJustification (juce::Justification::centred);
            row->bend.setText (juce::String (d.bendRange), false);
            row->bend.setTooltip ("Must match the pitch-bend range the controller sends (Osmose default: 48)");
            row->bend.setEnabled (d.mpe);

            auto apply = [raw, &eng]
            {
                auto bend = juce::jlimit (1, 96, raw->bend.getText().getIntValue() > 0 ? raw->bend.getText().getIntValue() : 48);
                raw->bend.setText (juce::String (bend), false);
                raw->bend.setEnabled (raw->mpe.getToggleState());
                eng.setMidiInputMpe (raw->slot, raw->mpe.getToggleState(), bend);
            };
            row->mpe.onClick = apply;
            row->bend.onReturnKey = [raw, apply] { apply(); raw->bend.unfocusAllComponents(); };
            row->bend.onFocusLost = apply;

            for (auto* c : std::initializer_list<juce::Component*> { &row->use, &row->name, &row->mpe, &row->bend, &row->bendUnit })
                addAndMakeVisible (c);
            inRows.push_back (std::move (row));
        }

        hintIn.setText ("Choose which controller plays each track on the track's patch card: right-click the track's display.",
                        juce::dontSendNotification);
        style (hintIn, Pal::faint, 15.0f);
        addAndMakeVisible (hintIn);

        for (auto& d : engine.getMidiOutputs())
        {
            auto row = std::make_unique<OutRow>();
            row->slot = d.slot;
            row->name.setText (d.name + (d.available ? juce::String() : juce::String ("   (not connected)")), juce::dontSendNotification);
            style (row->name, d.available ? Pal::cream : Pal::faint);
            row->use.setToggleState (d.active, juce::dontSendNotification);
            row->use.setTooltip ("Open this port so tracks can play a hardware synth through it");
            auto* raw = row.get();
            row->use.onClick = [raw, &eng]
            {
                auto on = raw->use.getToggleState();
                auto slot = raw->slot;
                juce::MessageManager::callAsync ([&eng, slot, on] { eng.setMidiOutputActive (slot, on); });
            };
            styleEditor (row->alias);
            row->alias.setTextToShowWhenEmpty ("e.g. JUNO-106", Pal::faint);
            row->alias.setText (d.alias, false);
            row->alias.setInputRestrictions (24);
            auto applyAlias = [raw, &eng] { eng.setMidiOutputAlias (raw->slot, raw->alias.getText()); };
            row->alias.onReturnKey = [raw, applyAlias] { applyAlias(); raw->alias.unfocusAllComponents(); };
            row->alias.onFocusLost = applyAlias;

            for (auto* c : std::initializer_list<juce::Component*> { &row->use, &row->name, &row->alias })
                addAndMakeVisible (c);
            outRows.push_back (std::move (row));
        }
        style (noOutputs, Pal::faint);
        noOutputs.setText ("No MIDI outputs found. Connect an interface or synth and they will appear here.", juce::dontSendNotification);
        addChildComponent (noOutputs);
        noOutputs.setVisible (outRows.empty());

        juce::Component::SafePointer<MidiPanel> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe == nullptr) return;
            if (safe->onPreferredHeightChanged) safe->onPreferredHeightChanged();
            else safe->resized();
        });
    }

    AudioEngine& engine;
    std::vector<std::unique_ptr<InRow>> inRows;
    std::vector<std::unique_ptr<OutRow>> outRows;
    juce::Label hintIn, noOutputs, recallNote;
    juce::ToggleButton recall { "Recall the deck with each tape" };
    std::vector<Heading> headings, columns;
    juce::String lastSignature;
};

//==============================================================================
class TapePanel : public juce::Component,
                  private juce::Timer
{
public:
    explicit TapePanel (AudioEngine& e) : engine (e)
    {
        auto& tp = engine.tapeEngine.params;
        auto signedDb = [] (float db) { return (db > 0.05f ? "+" : "") + juce::String (db, 1) + " dB"; };
        add ("HISS",            &tp.hiss,    [signedDb] (double v)
             {
                 return v <= 0 ? juce::String ("OFF") : "TAPE " + signedDb (TapeEngine::hissOffsetDb ((float) v));
             });
        add ("WOW",             &tp.wow,     [] (double v) { return juce::String (v * 0.5, 2) + " %"; });
        add ("FLUTTER",         &tp.flutter, [] (double v) { return juce::String (v * 0.3, 2) + " %"; });
        add ("TAPE DRIVE",      &tp.sat,     [signedDb] (double v) { return "0 VU " + signedDb (TapeEngine::driveDb ((float) v)); });
        add ("HF LOSS",         &tp.hf,      [] (double v) { return "-" + juce::String (v * 12.0, 1) + " dB @ 10k"; });
        add ("TRACK CROSSTALK", &tp.xtalk,   [] (double v) { return v <= 0 ? juce::String ("OFF") : juce::String (-80.0 + v * 50.0, 0) + " dB"; });
        add ("DROPOUTS",        &tp.drop,    [] (double v) { return v <= 0 ? juce::String ("NONE") : v < 0.25 ? juce::String ("RARE") : v < 0.6 ? juce::String ("OCCASIONAL") : juce::String ("FREQUENT"); });
        add ("SPEED DRIFT",     &tp.drift,   [] (double v) { return utf ("\xc2\xb1") + juce::String (v, 2) + " %"; });

        const char* presetNames[] { "CLEAN", "WELL KEPT", "WORN", "CHEWED" };
        for (int i = 0; i < 4; ++i)
        {
            auto* b = presets.add (new juce::TextButton (presetNames[i]));
            b->onClick = [this, i] { applyPreset (i); };
            addAndMakeVisible (b);
        }

        nrType.addItemList ({ "2:1 compander (broadband, dbx-style)", "Sliding band B (up to 10 dB HF)", "Sliding band C (up to 20 dB HF)" }, 1);
        nrType.setSelectedItemIndex (tp.nrType.load(), juce::dontSendNotification);
        nrType.onChange = [this] { engine.tapeEngine.params.nrType = nrType.getSelectedItemIndex(); };
        for (auto* c : std::initializer_list<juce::Component*> { &nrType, &nrLabel, &presetLabel })
            addAndMakeVisible (c);
        for (auto* l : { &nrLabel, &presetLabel })
        {
            l->setFont (DeckLookAndFeel::get().cond (17.0f));
            l->setColour (juce::Label::textColourId, Pal::muted);
        }
        controls[0]->slider.setDoubleClickReturnValue (true, 0.5);
        controls[3]->slider.setDoubleClickReturnValue (true, 0.5);
        controls[0]->slider.setTooltip ("Hiss relative to what this cassette really measures. Centre = as measured.");
        controls[3]->slider.setTooltip ("How hard 0 VU drives the tape. Centre = Dolby level, as a calibrated deck would. "
                                        "More drive: earlier saturation and treble compression.");
        startTimerHz (2);
    }

    void paint (juce::Graphics& g) override
    {
        auto* tape = engine.getTape();
        if (tape == nullptr || tapeInfo.isEmpty()) return;
        auto& lf = DeckLookAndFeel::get();
        const auto& m = tape->getModelInfo();
        g.setColour (Pal::darker);
        g.fillRoundedRectangle (tapeInfo.toFloat(), 6.0f);
        auto r = tapeInfo.reduced (14, 10);
        g.setColour (Pal::cream);
        g.setFont (lf.cond (20.0f, true));
        g.drawText ("THIS TAPE:  " + tape->productName() + utf ("  \xc2\xb7  ") + utf (m.position), r.removeFromTop (26), juce::Justification::centredLeft);
        g.setColour (Pal::lcdText);
        g.setFont (lf.mono (15.0f));
        auto f1 = [] (float v) { return (v > 0 ? "+" : "") + juce::String (v, 1); };
        g.drawText ("MOL 400 Hz " + f1 (m.mol400) + " dB   MOL 100 Hz " + f1 (m.mol100) + " dB   SOL 10 kHz " + f1 (m.sol10k) + " dB",
                    r.removeFromTop (22), juce::Justification::centredLeft);
        g.drawText ("S/N " + juce::String (m.snr, 1) + " dBA   MOD NOISE " + juce::String (m.modNoise, 1) + " dB   SENS " + f1 (m.sensitivity)
                        + " dB   EQ " + juce::String (m.eqMicroseconds) + utf (" \xc2\xb5s"),
                    r.removeFromTop (22), juce::Justification::centredLeft);
        g.setColour (Pal::faint);
        g.setFont (lf.cond (15.0f));
        g.drawText (utf ("Measured on ") + m.basedOn + utf (" (dB re Dolby level, 1\xe2\x85\x9e ips, no NR). The cassette sets these; choose another at EJECT."),
                    r.removeFromTop (22), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        auto top = r.removeFromTop (30);
        presetLabel.setBounds (top.removeFromLeft (150));
        for (auto* b : presets) { b->setBounds (top.removeFromLeft (110)); top.removeFromLeft (6); }
        r.removeFromTop (16);

        for (auto& c : controls)
        {
            auto line = r.removeFromTop (34);
            c->label.setBounds (line.removeFromLeft (150));
            c->value.setBounds (line.removeFromRight (150));
            c->slider.setBounds (line.reduced (8, 0));
            r.removeFromTop (6);
        }
        r.removeFromTop (14);
        auto l1 = r.removeFromTop (30);
        nrLabel.setBounds (l1.removeFromLeft (150));
        nrType.setBounds (l1.withWidth (360));
        r.removeFromTop (18);
        tapeInfo = r.removeFromTop (112);
    }

private:
    struct Control
    {
        juce::Label label, value;
        juce::Slider slider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        std::atomic<float>* target = nullptr;
        std::function<juce::String (double)> fmt;
    };

    void add (const juce::String& name, std::atomic<float>* target, std::function<juce::String (double)> fmt)
    {
        auto c = std::make_unique<Control>();
        c->label.setText (name, juce::dontSendNotification);
        c->label.setFont (DeckLookAndFeel::get().cond (17.0f, true));
        c->label.setColour (juce::Label::textColourId, Pal::muted);
        c->value.setFont (DeckLookAndFeel::get().mono (16.0f));
        c->value.setColour (juce::Label::textColourId, Pal::lcdText);
        c->target = target;
        c->fmt = std::move (fmt);
        c->slider.setRange (0.0, 1.0);
        auto* raw = c.get();
        c->slider.onValueChange = [raw]
        {
            raw->target->store ((float) raw->slider.getValue());
            raw->value.setText (raw->fmt (raw->slider.getValue()), juce::dontSendNotification);
        };
        c->slider.setValue (target->load(), juce::dontSendNotification);
        c->value.setText (c->fmt (target->load()), juce::dontSendNotification);
        addAndMakeVisible (c->label);
        addAndMakeVisible (c->value);
        addAndMakeVisible (c->slider);
        controls.push_back (std::move (c));
    }

    void timerCallback() override
    {
        auto* t = engine.getTape();
        auto key = t != nullptr ? t->id : juce::String();
        if (key != shownTape) { shownTape = key; repaint(); }
    }

    void applyPreset (int i)
    {
        static const float p[4][8] {
            { 0, 0, 0, 0, 0, 0, 0, 0 },
            { 0.55f, 0.30f, 0.25f, 0.40f, 0.20f, 0.30f, 0.10f, 0.20f },
            { 0.75f, 0.60f, 0.50f, 0.60f, 0.55f, 0.50f, 0.35f, 0.40f },
            { 0.90f, 0.90f, 0.80f, 0.75f, 0.80f, 0.70f, 0.75f, 0.70f } };
        for (size_t k = 0; k < controls.size(); ++k)
            if (k != 3)   // drive is calibration, not wear
                controls[k]->slider.setValue (p[i][k], juce::sendNotificationSync);
    }

    AudioEngine& engine;
    std::vector<std::unique_ptr<Control>> controls;
    juce::OwnedArray<juce::TextButton> presets;
    juce::ComboBox nrType;
    juce::Label nrLabel { {}, "Noise reduction" }, presetLabel { {}, "Tape condition" };
    juce::Rectangle<int> tapeInfo;
    juce::String shownTape;
};

//==============================================================================
class InstrumentsPanel : public juce::Component,
                         private juce::ChangeListener
{
public:
    InstrumentsPanel (AudioEngine& e, juce::PropertiesFile& settings)
        : engine (e), props (settings),
          list (e.formatManager, e.knownPlugins,
                settings.getFile().getSiblingFile ("RecentlyCrashedPluginsList"), &settings, true)
    {
        note.setText ("Press \"Options...\" then \"Scan for new or updated VST3 plug-ins\". "
                      "Instruments found here appear on each track's patch card (right-click the track's display).", juce::dontSendNotification);
        note.setFont (DeckLookAndFeel::get().cond (17.0f));
        note.setColour (juce::Label::textColourId, Pal::muted);
        addAndMakeVisible (note);
        addAndMakeVisible (list);
        engine.knownPlugins.addChangeListener (this);
    }

    ~InstrumentsPanel() override { engine.knownPlugins.removeChangeListener (this); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        note.setBounds (r.removeFromTop (44));
        list.setBounds (r);
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (auto xml = engine.knownPlugins.createXml())
        {
            props.setValue ("knownPlugins", xml.get());
            props.saveIfNeeded();
        }
    }

    AudioEngine& engine;
    juce::PropertiesFile& props;
    juce::PluginListComponent list;
    juce::Label note;
};

template <typename Panel>
class Scrolling : public juce::Viewport
{
public:
    explicit Scrolling (AudioEngine& e) : panel (e)
    {
        setViewedComponent (&panel, false);
        setScrollBarsShown (true, false);
        panel.onPreferredHeightChanged = [this]
        {
            if (panel.getHeight() != panel.preferredHeight())
                panel.setSize (getMaximumVisibleWidth(), panel.preferredHeight());
            else
                panel.resized();
        };
    }

    void resized() override
    {
        juce::Viewport::resized();
        panel.setSize (getMaximumVisibleWidth(), panel.preferredHeight());
    }

private:
    Panel panel;
};
} // namespace

//==============================================================================
SetupWindow::SetupWindow (AudioEngine& engineRef, juce::PropertiesFile& settings)
    : juce::DocumentWindow (utf ("TAPE\xc2\xb7" "FOUR  Setup"), Pal::panel, juce::DocumentWindow::closeButton),
      engine (engineRef)
{
    setUsingNativeTitleBar (true);
    tabs = std::make_unique<juce::TabbedComponent> (juce::TabbedButtonBar::TabsAtTop);
    tabs->setTabBarDepth (36);
    tabs->addTab ("AUDIO", Pal::panel, new Scrolling<AudioPanel> (engineRef), true);
    tabs->addTab ("MIDI & DECK", Pal::panel, new Scrolling<MidiPanel> (engineRef), true);
    tabs->addTab ("TAPE", Pal::panel, new TapePanel (engineRef), true);
    tabs->addTab ("INSTRUMENTS", Pal::panel, new InstrumentsPanel (engineRef, settings), true);
    setContentNonOwned (tabs.get(), false);
    setResizable (true, false);
    centreWithSize (780, 760);
}

SetupWindow::~SetupWindow()
{
    clearContentComponent();
}

void SetupWindow::showTab (int index)
{
    tabs->setCurrentTabIndex (juce::jlimit (0, tabs->getNumTabs() - 1, index));
    setVisible (true);
    toFront (true);
}
} // namespace tf
