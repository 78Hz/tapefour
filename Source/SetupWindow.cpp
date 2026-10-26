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

        for (auto* l : { &mainLabel, &phonesLabel, &latencyLabel })
        {
            l->setFont (DeckLookAndFeel::get().cond (17.0f));
            l->setColour (juce::Label::textColourId, Pal::muted);
            l->setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (l);
        }

        mainOut.onChange   = [this] { engine.mainOutPair = std::max (0, mainOut.getSelectedItemIndex()); engine.saveSettings(); };
        phonesOut.onChange = [this] { engine.phonesPair = phonesOut.getSelectedItemIndex() - 1; engine.saveSettings(); };

        latency.setRange (-50.0, 50.0, 1.0);
        latency.setTextValueSuffix (" ms");
        latency.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 24);
        latency.setValue (engine.tapeEngine.params.latencyOffsetMs.load(), juce::dontSendNotification);
        latency.onValueChange = [this] { engine.tapeEngine.params.latencyOffsetMs = (int) latency.getValue(); };
        latency.onDragEnd = [this] { engine.saveSettings(); };
        latency.setTooltip ("Overdubs are shifted earlier by the driver's reported latency. "
                            "If they still land late or early, adjust here.");

        for (auto* c : std::initializer_list<juce::Component*> { &mainOut, &phonesOut, &latency })
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
        row (latencyLabel, latency);
    }

    int selectorHeight() const { return selector.getHeight() > 0 ? selector.getHeight() : 360; }
    int preferredHeight() const { return selectorHeight() + 24 + 12 + 3 * 38 + 24; }
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
                latencyLabel { {}, "Overdub timing" };
    juce::ComboBox mainOut, phonesOut;
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
        recallNote.setText ("Every tape remembers the deck it was recorded on: each track's inputs and signal chain with every plug-in's settings and MIDI mappings, "
                            "MIDI routing, EQ, pan, faders, master, speed and pitch. With this ticked, loading a tape "
                            "puts the deck back the way it was. Unticked, the deck stays as it is.", juce::dontSendNotification);
        style (recallNote, Pal::faint, 15.0f);
        recallNote.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (recallNote);

        mmc.setToggleState (engine.mapper.followMmc.load(), juce::dontSendNotification);
        mmc.onClick = [this] { engine.mapper.followMmc = mmc.getToggleState(); engine.saveSettings(); };
        mmc.setTooltip ("Play, stop, record, rewind and fast-forward buttons that send MIDI Machine Control move the tape, with no learning needed");
        addAndMakeVisible (mmc);

        rebuild();
        startTimer (1000);
    }

    int preferredHeight() const
    {
        return 12 + 34 + 26 + (int) inRows.size() * 36 + 40 + 34 + 26 + std::max (1, (int) outRows.size()) * 36 + 40
               + 34 + 36 + 26 + std::max (1, (int) mapRows.size()) * 34 + 40 + 34 + 36 + 70 + 12;
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
        heading ("MIDI CONTROL");
        mmc.setBounds (r.removeFromTop (32));
        r.removeFromTop (4);
        cols ({ { "FROM", 0 }, { "MESSAGE", 160 }, { "CONTROLS", 260 }, { "MOVES", 450 }, { "PICKUP", 540 }, { "ALSO PLAYS", 610 } });
        for (auto& row : mapRows)
        {
            auto line = r.removeFromTop (30);
            row->from.setBounds (line.removeFromLeft (160));
            row->message.setBounds (line.removeFromLeft (100));
            row->target.setBounds (line.removeFromLeft (190));
            row->mode.setBounds (line.removeFromLeft (90));
            row->pickup.setBounds (line.removeFromLeft (70).withSizeKeepingCentre (30, 30));
            row->also.setBounds (line.removeFromLeft (80).withSizeKeepingCentre (30, 30));
            row->forget.setBounds (line.removeFromLeft (36).withSizeKeepingCentre (28, 26));
            r.removeFromTop (4);
        }
        if (mapRows.empty())
            noMappings.setBounds (r.removeFromTop (30));
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
    struct MapRow
    {
        int id = 0;
        juce::Label from, message, target, mode;
        juce::ToggleButton pickup, also;
        juce::TextButton forget { juce::String (juce::CharPointer_UTF8 ("\xe2\x9c\x95")) };
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
        for (auto& m : engine.mapper.getMappings()) sig << "m" << m.id << (int) m.pickup << (int) m.passThrough << m.paramName;
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
            row->use.setTooltip ("Listen to this controller. Each track chooses which controller plays it in its signal chain (click the track's display).");
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

        hintIn.setText ("Each track chooses which controller plays it in its signal chain: click the track's display.",
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
        mapRows.clear();
        for (auto& m : engine.mapper.getMappings())
        {
            auto row = std::make_unique<MapRow>();
            row->id = m.id;
            row->from.setText (m.portName, juce::dontSendNotification);
            row->message.setText (engine.mapper.describeMessage (m), juce::dontSendNotification);
            row->target.setText (engine.mapper.describeTarget (m), juce::dontSendNotification);
            using Mo = MidiMapper::Mode;
            row->mode.setText (m.mode == Mo::toggle ? "PRESS" : m.mode == Mo::absolute ? (m.msg == MidiMapper::Msg::cc14 ? "14-BIT" : "KNOB") : "ENCODER",
                               juce::dontSendNotification);
            style (row->from, Pal::muted, 15.0f);
            style (row->message, Pal::lcdText, 15.0f);
            style (row->target, Pal::cream, 15.0f);
            style (row->mode, Pal::muted, 15.0f);
            const bool knob = m.mode == Mo::absolute;
            row->pickup.setToggleState (m.pickup, juce::dontSendNotification);
            row->pickup.setEnabled (knob);
            row->pickup.setTooltip ("The knob only takes over once it passes the control's current position, so nothing jumps");
            row->also.setToggleState (m.passThrough, juce::dontSendNotification);
            row->also.setTooltip ("Also pass the message on to the track's instruments, instead of keeping it for this control");
            auto id = m.id;
            row->pickup.onClick = [this, raw = row.get(), id] { engine.mapper.setPickup (id, raw->pickup.getToggleState()); engine.saveSettings(); };
            row->also.onClick = [this, raw = row.get(), id] { engine.mapper.setPassThrough (id, raw->also.getToggleState()); engine.saveSettings(); };
            row->forget.setTooltip ("Forget this mapping");
            row->forget.onClick = [this, id]
            {
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MidiPanel> (this), id]
                {
                    if (safe == nullptr) return;
                    safe->engine.mapper.remove (id);
                    safe->engine.saveSettings();
                    safe->rebuild();
                });
            };
            for (auto* c : std::initializer_list<juce::Component*> { &row->from, &row->message, &row->target, &row->mode, &row->pickup, &row->also, &row->forget })
                addAndMakeVisible (c);
            mapRows.push_back (std::move (row));
        }
        style (noMappings, Pal::faint, 15.0f);
        noMappings.setText ("Nothing mapped yet. Press LEARN on the deck, click a control, then move a knob or press a button on your controller. "
                            "Right-click any control to learn or forget it.", juce::dontSendNotification);
        addChildComponent (noMappings);
        noMappings.setVisible (mapRows.empty());

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
    std::vector<std::unique_ptr<MapRow>> mapRows;
    juce::Label hintIn, noOutputs, recallNote, noMappings;
    juce::ToggleButton mmc { "Follow transport buttons that send MIDI Machine Control (MMC)" };
    juce::ToggleButton recall { "Recall the deck with each tape" };
    std::vector<Heading> headings, columns;
    juce::String lastSignature;
};

//==============================================================================
/** The machine (TF-4.deck): its mechanism and heads, the operator's tape drive and NR type,
    and what the cassette in it is. The cassette's own condition is chosen when it is made. */
class DeckPanel : public juce::Component,
                  private juce::Timer
{
public:
    explicit DeckPanel (AudioEngine& e) : engine (e)
    {
        auto& m = engine.tapeEngine.machine;
        auto pct = [] (double v) { return juce::String (v, 2) + " %"; };
        add ("WOW",          &m.wowPct,      0.0, 0.6, 1.0, true, pct);
        add ("FLUTTER",      &m.flutterPct,  0.0, 0.3, 1.0, true, pct);
        add ("SPEED DRIFT",  &m.driftPct,    0.0, 1.0, 1.0, true, [] (double v) { return utf ("\xc2\xb1") + juce::String (v, 2) + " %"; });
        add ("HEADS",        &m.cutoffHz,    4000.0, 20000.0, 0.5, true, [] (double v) { return "TREBLE TO " + juce::String (v / 1000.0, 1) + " kHz"; });
        add ("CROSSTALK",    &m.crosstalkDb, -100.0, -40.0, 1.0, true, [] (double v) { return v <= -99.5 ? juce::String ("NONE") : juce::String (v, 0) + " dB"; });
        add ("TAPE DRIVE",   &engine.tapeEngine.params.sat, 0.0, 1.0, 1.0, false, [] (double v)
             {
                 auto db = TapeEngine::driveDb ((float) v);
                 return "0 VU " + juce::String (db > 0.05f ? "+" : "") + juce::String (db, 1) + " dB";
             });
        // "None" is stored as -120 dB, which the slider shows at its end.
        crosstalk().slider.onValueChange = [this]
        {
            auto v = crosstalk().slider.getValue();
            engine.tapeEngine.machine.crosstalkDb = v <= -99.5 ? -120.0f : (float) v;
            crosstalk().value.setText (crosstalk().fmt (v), juce::dontSendNotification);
            machineMoved();
        };
        crosstalk().slider.setValue (std::max (-100.0f, engine.tapeEngine.machine.crosstalkDb.load()), juce::dontSendNotification);

        const char* presetNames[] { "NEW", "SERVICED", "WORN", "NEGLECTED" };
        for (int i = 0; i < 4; ++i)
        {
            auto* b = presets.add (new juce::TextButton (presetNames[i]));
            b->onClick = [this, i] { applyPreset (tfmt::DeckDescription::conditions()[(size_t) i]); };
            addAndMakeVisible (b);
        }

        asNew.setToggleState (! engine.tapeEngine.machine.honoursTapeCondition.load(), juce::dontSendNotification);
        asNew.onClick = [this]
        {
            engine.tapeEngine.machine.honoursTapeCondition = ! asNew.getToggleState();
            engine.saveSettings();
            repaint();
        };
        asNew.setColour (juce::ToggleButton::textColourId, Pal::muted);
        addAndMakeVisible (asNew);

        nrType.addItemList ({ "2:1 compander (broadband, dbx-style)", "Sliding band B (up to 10 dB HF)", "Sliding band C (up to 20 dB HF)" }, 1);
        nrType.setSelectedItemIndex (engine.tapeEngine.params.nrType.load(), juce::dontSendNotification);
        nrType.onChange = [this] { engine.tapeEngine.params.nrType = nrType.getSelectedItemIndex(); };
        for (auto* c : std::initializer_list<juce::Component*> { &nrType, &nrLabel, &presetLabel })
            addAndMakeVisible (c);
        for (auto* l : { &nrLabel, &presetLabel })
        {
            l->setFont (DeckLookAndFeel::get().cond (17.0f));
            l->setColour (juce::Label::textColourId, Pal::muted);
        }
        drive().slider.setDoubleClickReturnValue (true, 0.5);
        drive().slider.setTooltip ("How hard 0 VU drives the tape. Centre = Dolby level, as a calibrated deck would. "
                                   "More drive: earlier saturation and treble compression.");
        controls[3]->slider.setTooltip ("Worn or misaligned heads lose treble on playback, whatever the tape.");
        presetLabel.setTooltip ("The machine's mechanism and heads. A cassette's own condition is chosen when it is made, at EJECT.");
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
        auto f1 = [] (double v) { return (v > 0 ? "+" : "") + juce::String (v, 1); };
        g.drawText ("MOL 400 Hz " + f1 (m.mol400) + " dB   MOL 100 Hz " + f1 (m.mol100) + " dB   SOL 10 kHz " + f1 (m.sol10k) + " dB",
                    r.removeFromTop (22), juce::Justification::centredLeft);
        g.drawText ("S/N " + juce::String (m.snr, 1) + " dBA   MOD NOISE " + juce::String (m.modNoise, 1) + " dB   SENS " + f1 (m.sensitivity)
                        + " dB   EQ " + juce::String (m.eqMicroseconds) + utf (" \xc2\xb5s"),
                    r.removeFromTop (22), juce::Justification::centredLeft);
        const auto& c = tape->getCondition();
        const bool honoured = engine.tapeEngine.machine.honoursTapeCondition.load();
        g.setColour (honoured ? Pal::lcdText : Pal::lcdDim);
        g.drawText ("CONDITION " + juce::String (c.grade).toUpperCase() + "   HISS " + f1 (c.hissDb) + " dB   DROPOUTS "
                        + juce::String (c.dropoutsPerMinute, 2) + "/MIN   TREBLE " + f1 (-c.trebleLossDb) + " dB   MOD " + f1 (c.modNoiseDb) + " dB",
                    r.removeFromTop (22), juce::Justification::centredLeft);
        g.setColour (Pal::faint);
        g.setFont (lf.cond (15.0f));
        g.drawText (honoured ? utf ("Measured on ") + m.basedOn + utf (" (dB re Dolby level, 1\xe2\x85\x9e ips, no NR). The cassette sets these; choose another at EJECT.")
                             : juce::String ("The deck is playing every cassette as new: its condition is ignored."),
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
            c->value.setBounds (line.removeFromRight (170));
            c->slider.setBounds (line.reduced (8, 0));
            r.removeFromTop (6);
        }
        r.removeFromTop (4);
        asNew.setBounds (r.removeFromTop (28).withTrimmedLeft (150));
        r.removeFromTop (10);
        auto l1 = r.removeFromTop (30);
        nrLabel.setBounds (l1.removeFromLeft (150));
        nrType.setBounds (l1.withWidth (360));
        r.removeFromTop (18);
        tapeInfo = r.removeFromTop (134);
    }

private:
    struct Control
    {
        juce::Label label, value;
        juce::Slider slider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        std::atomic<float>* target = nullptr;
        std::function<juce::String (double)> fmt;
    };

    Control& drive()     { return *controls[5]; }
    Control& crosstalk() { return *controls[4]; }

    void add (const juce::String& name, std::atomic<float>* target, double lo, double hi, double skew, bool isMachine,
              std::function<juce::String (double)> fmt)
    {
        auto c = std::make_unique<Control>();
        c->label.setText (name, juce::dontSendNotification);
        c->label.setFont (DeckLookAndFeel::get().cond (17.0f, true));
        c->label.setColour (juce::Label::textColourId, Pal::muted);
        c->value.setFont (DeckLookAndFeel::get().mono (16.0f));
        c->value.setColour (juce::Label::textColourId, Pal::lcdText);
        c->target = target;
        c->fmt = std::move (fmt);
        c->slider.setRange (lo, hi);
        c->slider.setSkewFactor (skew);
        auto* raw = c.get();
        c->slider.onValueChange = [this, raw, isMachine]
        {
            raw->target->store ((float) raw->slider.getValue());
            raw->value.setText (raw->fmt (raw->slider.getValue()), juce::dontSendNotification);
            if (isMachine) machineMoved();
        };
        c->slider.onDragEnd = [this] { engine.saveSettings(); };
        c->slider.setValue (target->load(), juce::dontSendNotification);
        c->value.setText (c->fmt (target->load()), juce::dontSendNotification);
        addAndMakeVisible (c->label);
        addAndMakeVisible (c->value);
        addAndMakeVisible (c->slider);
        controls.push_back (std::move (c));
    }

    void machineMoved()
    {
        if (! applyingPreset) engine.setMachineCondition ("custom");
    }

    void timerCallback() override
    {
        auto* t = engine.getTape();
        auto key = t != nullptr ? t->id : juce::String();
        if (key != shownTape) { shownTape = key; repaint(); }
    }

    void applyPreset (const std::string& grade)
    {
        auto d = engine.getMachine();
        d.applyCondition (grade);
        applyingPreset = true;
        controls[0]->slider.setValue (d.transport.wowPct, juce::sendNotificationSync);
        controls[1]->slider.setValue (d.transport.flutterPct, juce::sendNotificationSync);
        controls[2]->slider.setValue (d.transport.driftPct, juce::sendNotificationSync);
        controls[3]->slider.setValue (d.heads.cutoffHz, juce::sendNotificationSync);
        controls[4]->slider.setValue (std::max (-100.0, d.heads.crosstalkDb), juce::sendNotificationSync);
        applyingPreset = false;
        engine.setMachine (d);
        engine.saveSettings();
    }

    AudioEngine& engine;
    std::vector<std::unique_ptr<Control>> controls;
    juce::OwnedArray<juce::TextButton> presets;
    juce::ToggleButton asNew { "Play every cassette as new (ignore its condition)" };
    juce::ComboBox nrType;
    juce::Label nrLabel { {}, "Noise reduction" }, presetLabel { {}, "Machine" };
    juce::Rectangle<int> tapeInfo;
    juce::String shownTape;
    bool applyingPreset = false;
};

//==============================================================================
static juce::String formatList (AudioEngine& e)
{
    juce::StringArray names;
    for (auto* f : e.formatManager.getFormats())
        names.add (f->getName() == "AudioUnit" ? juce::String ("AU") : f->getName());
    return names.joinIntoString (", ");
}

class InstrumentsPanel : public juce::Component,
                         private juce::ChangeListener
{
public:
    InstrumentsPanel (AudioEngine& e, juce::PropertiesFile& settings)
        : engine (e), props (settings),
          list (e.formatManager, e.knownPlugins,
                settings.getFile().getSiblingFile ("RecentlyCrashedPluginsList"), &settings, true)
    {
        note.setText (juce::String ("Press \"Options...\" and scan each format you use (") + formatList (e) + "). "
                      "Plug-ins found here appear in each track's signal chain: click the track's display, then +. "
                      "A plug-in that comes in several formats is offered once, as CLAP, VST3, AU, LV2 or LADSPA, in that order.",
                      juce::dontSendNotification);
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
    tabs->setTabBarDepth (48);
    tabs->addTab ("AUDIO", Pal::panel, new Scrolling<AudioPanel> (engineRef), true);
    tabs->addTab ("MIDI & DECK", Pal::panel, new Scrolling<MidiPanel> (engineRef), true);
    tabs->addTab ("DECK", Pal::panel, new DeckPanel (engineRef), true);
    tabs->addTab ("PLUG-INS", Pal::panel, new InstrumentsPanel (engineRef, settings), true);
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
