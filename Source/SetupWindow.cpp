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
          // MIDI inputs are handled below, not by JUCE's selector.
          selector (e.deviceManager, 0, 16, 2, 16, false, false, false, false)
    {
        addAndMakeVisible (selector);
        selector.addComponentListener (this);

        for (auto* l : { &mainLabel, &phonesLabel, &monitorLabel, &latencyLabel, &midiLabel, &mpeLabel, &bendLabel })
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

        follow.setToggleState (engine.followArm.load(), juce::dontSendNotification);
        follow.onClick = [this] { engine.followArm = follow.getToggleState(); engine.saveSettings(); };

        latency.setRange (-50.0, 50.0, 1.0);
        latency.setTextValueSuffix (" ms");
        latency.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 24);
        latency.setValue (engine.tapeEngine.params.latencyOffsetMs.load(), juce::dontSendNotification);
        latency.onValueChange = [this] { engine.tapeEngine.params.latencyOffsetMs = (int) latency.getValue(); };
        latency.onDragEnd = [this] { engine.saveSettings(); };
        latency.setTooltip ("Overdubs are shifted earlier by the driver's reported latency. "
                            "If they still land late or early, adjust here.");

        mpe.setToggleState (engine.mpeEnabled.load(), juce::dontSendNotification);
        mpe.onClick = [this] { engine.setMpe (mpe.getToggleState(), (int) bend.getValue()); updateEnablement(); };
        bend.setRange (1.0, 96.0, 1.0);
        bend.setTextValueSuffix (" semitones");
        bend.setTextBoxStyle (juce::Slider::TextBoxRight, false, 110, 24);
        bend.setValue (engine.mpeBendRange.load(), juce::dontSendNotification);
        bend.onDragEnd = [this] { engine.setMpe (mpe.getToggleState(), (int) bend.getValue()); };
        bend.setTooltip ("Must match the pitch-bend range your MPE controller sends (Osmose default: 48).");

        for (auto* c : std::initializer_list<juce::Component*> { &mainOut, &phonesOut, &monitor, &follow, &latency, &mpe, &bend })
            addAndMakeVisible (c);

        noMidi.setFont (DeckLookAndFeel::get().cond (17.0f));
        noMidi.setColour (juce::Label::textColourId, Pal::faint);
        addChildComponent (noMidi);

        engine.deviceManager.addChangeListener (this);
        refreshOutputs();
        rebuildMidiList();
        updateEnablement();
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

        r.removeFromTop (10);
        auto first = true;
        for (auto* t : midiToggles)
        {
            auto line = r.removeFromTop (28);
            auto lbl = line.removeFromLeft (200);
            if (first) midiLabel.setBounds (lbl);
            first = false;
            line.removeFromLeft (10);
            t->setBounds (line);
        }
        if (midiToggles.isEmpty())
        {
            auto line = r.removeFromTop (28);
            midiLabel.setBounds (line.removeFromLeft (200));
            line.removeFromLeft (10);
            noMidi.setBounds (line);
        }
        r.removeFromTop (12);

        auto line = r.removeFromTop (30);
        mpeLabel.setBounds (line.removeFromLeft (200));
        line.removeFromLeft (10);
        mpe.setBounds (line);
        r.removeFromTop (8);
        row (bendLabel, bend);
        line = r.removeFromTop (30);
        line.removeFromLeft (210);
        follow.setBounds (line);
    }

    int selectorHeight() const { return selector.getHeight() > 0 ? selector.getHeight() : 360; }
    int preferredHeight() const
    {
        return selectorHeight() + 24 + 12 + 4 * 38 + 10 + std::max (1, midiToggles.size()) * 28 + 12 + 3 * 38 + 24;
    }
    std::function<void()> onPreferredHeightChanged;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        refreshOutputs();
        // Never rebuild the MIDI list from inside one of its own clicks.
        juce::Component::SafePointer<AudioPanel> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->rebuildMidiList(); });
    }

    void componentMovedOrResized (juce::Component&, bool, bool wasResized) override
    {
        if (wasResized) relayoutLater();
    }

    void relayoutLater()
    {
        // Re-lay out later, outside the selector's own resize, so the two never chase each other.
        juce::Component::SafePointer<AudioPanel> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe == nullptr) return;
            if (safe->onPreferredHeightChanged) safe->onPreferredHeightChanged();
            else safe->resized();
        });
    }

    void rebuildMidiList()
    {
        auto devices = engine.getMidiInputs();
        juce::StringArray ids;
        for (auto& d : devices) ids.add (d.identifier);

        noMidi.setVisible (ids.isEmpty());
        if (ids == midiIds && midiToggles.size() == ids.size())
        {
            for (int i = 0; i < midiToggles.size(); ++i)
                midiToggles[i]->setToggleState (engine.isMidiInputEnabled (midiIds[i]), juce::dontSendNotification);
            return;
        }

        midiIds = ids;
        midiToggles.clear();
        for (auto& d : devices)
        {
            auto* t = midiToggles.add (new juce::ToggleButton (d.name));
            t->setToggleState (engine.isMidiInputEnabled (d.identifier), juce::dontSendNotification);
            auto id = d.identifier;
            auto& eng = engine;
            t->onClick = [t, id, &eng]
            {
                auto on = t->getToggleState();
                // Open or close the port after the click has finished.
                juce::MessageManager::callAsync ([&eng, id, on] { eng.setMidiInputEnabled (id, on); });
            };
            addAndMakeVisible (t);
        }
        relayoutLater();
    }

    void updateEnablement()
    {
        bend.setEnabled (mpe.getToggleState());
        follow.setEnabled (! mpe.getToggleState());
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
                monitorLabel { {}, "Input monitoring" }, latencyLabel { {}, "Overdub timing" },
                midiLabel { {}, "MIDI inputs" }, mpeLabel { {}, "MPE" }, bendLabel { {}, "MPE pitch-bend range" },
                noMidi { {}, "No MIDI inputs found. Connect a controller and reopen this window." };
    juce::ComboBox mainOut, phonesOut, monitor;
    juce::ToggleButton follow { "Keyboard plays the armed instrument track (off: track n listens on MIDI channel n)" };
    juce::ToggleButton mpe { "MPE controller (Osmose, Seaboard, LinnStrument...): every instrument gets per-note bend, pressure and timbre" };
    juce::Slider latency { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Slider bend { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::OwnedArray<juce::ToggleButton> midiToggles;
    juce::StringArray midiIds;
};

//==============================================================================
class TapePanel : public juce::Component
{
public:
    explicit TapePanel (AudioEngine& e) : engine (e)
    {
        auto& tp = engine.tapeEngine.params;
        add ("HISS",            &tp.hiss,    [] (double v) { return v <= 0 ? juce::String ("OFF") : juce::String (-80.0 + v * 40.0, 0) + " dB"; });
        add ("WOW",             &tp.wow,     [] (double v) { return juce::String (v * 0.5, 2) + " %"; });
        add ("FLUTTER",         &tp.flutter, [] (double v) { return juce::String (v * 0.3, 2) + " %"; });
        add ("SATURATION",      &tp.sat,     [] (double v) { return juce::String (juce::roundToInt (v * 100)) + " %"; });
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
        formulation.addItemList ({ "Type I  normal", "Type II  chrome" }, 1);
        formulation.setSelectedItemIndex (tp.formulation.load(), juce::dontSendNotification);
        formulation.onChange = [this] { engine.tapeEngine.params.formulation = formulation.getSelectedItemIndex(); };
        for (auto* c : std::initializer_list<juce::Component*> { &nrType, &formulation, &nrLabel, &formLabel, &presetLabel })
            addAndMakeVisible (c);
        for (auto* l : { &nrLabel, &formLabel, &presetLabel })
        {
            l->setFont (DeckLookAndFeel::get().cond (17.0f));
            l->setColour (juce::Label::textColourId, Pal::muted);
        }
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
        r.removeFromTop (8);
        auto l2 = r.removeFromTop (30);
        formLabel.setBounds (l2.removeFromLeft (150));
        formulation.setBounds (l2.withWidth (360));
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

    void applyPreset (int i)
    {
        static const float p[4][8] {
            { 0, 0, 0, 0, 0, 0, 0, 0 },
            { 0.55f, 0.30f, 0.25f, 0.40f, 0.20f, 0.30f, 0.10f, 0.20f },
            { 0.75f, 0.60f, 0.50f, 0.60f, 0.55f, 0.50f, 0.35f, 0.40f },
            { 0.90f, 0.90f, 0.80f, 0.75f, 0.80f, 0.70f, 0.75f, 0.70f } };
        for (size_t k = 0; k < controls.size(); ++k)
            controls[k]->slider.setValue (p[i][k], juce::sendNotificationSync);
    }

    AudioEngine& engine;
    std::vector<std::unique_ptr<Control>> controls;
    juce::OwnedArray<juce::TextButton> presets;
    juce::ComboBox nrType, formulation;
    juce::Label nrLabel { {}, "Noise reduction" }, formLabel { {}, "Tape formulation" }, presetLabel { {}, "Tape condition" };
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
                      "Instruments found here appear in each track's right-click menu.", juce::dontSendNotification);
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

class ScrollingAudioPanel : public juce::Viewport
{
public:
    explicit ScrollingAudioPanel (AudioEngine& e) : panel (e)
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
    AudioPanel panel;
};
} // namespace

//==============================================================================
SetupWindow::SetupWindow (AudioEngine& engineRef, juce::PropertiesFile& settings)
    : juce::DocumentWindow (utf ("TAPE\xc2\xb7" "FOUR  Audio Setup"), Pal::panel, juce::DocumentWindow::closeButton),
      engine (engineRef)
{
    setUsingNativeTitleBar (true);
    tabs = std::make_unique<juce::TabbedComponent> (juce::TabbedButtonBar::TabsAtTop);
    tabs->setTabBarDepth (36);
    tabs->addTab ("AUDIO & MIDI", Pal::panel, new ScrollingAudioPanel (engineRef), true);
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
