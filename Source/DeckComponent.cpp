#include "DeckComponent.h"
#include "SetupWindow.h"

namespace tf
{
//==============================================================================
PluginWindow::PluginWindow (juce::AudioPluginInstance& plugin, std::function<void()> onClose)
    : juce::DocumentWindow (plugin.getName(), Pal::panel, juce::DocumentWindow::closeButton),
      closeCallback (std::move (onClose))
{
    setUsingNativeTitleBar (true);
    if (auto* editor = plugin.createEditorIfNeeded())
    {
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
    }
    centreWithSize (std::max (200, getWidth()), std::max (100, getHeight()));
    setAlwaysOnTop (true);   // stays in front of the deck while you tweak it
    setVisible (true);
}

//==============================================================================
namespace
{
    // design coordinates
    // Portastudio layout: mixer on the left, the tape section in a column on the right.
    const juce::Rectangle<int> bayPanel { 1020, 74, 560, 404 };
    const juce::Rectangle<int> transportPanel { 1020, 490, 560, 390 };
    enum { audioTab = 0, midiTab = 1, tapeTab = 2, instrumentsTab = 3 };
    constexpr int mixerX = 20, mixerY = 74, mixerH = 806, stripW = 197, stripGap = 12, masterW = 150;

    void panelBackground (juce::Graphics& g, juce::Rectangle<int> r, juce::Colour c)
    {
        auto f = r.toFloat();
        g.setColour (Pal::darker);
        g.fillRoundedRectangle (f.translated (0, 2), 10.0f);
        g.setColour (c);
        g.fillRoundedRectangle (f, 10.0f);
        g.setColour (Pal::panelHi);
        g.drawLine (f.getX() + 10, f.getY() + 1, f.getRight() - 10, f.getY() + 1, 1.0f);
    }

    void smallCaps (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                    juce::Justification j = juce::Justification::centredLeft, juce::Colour c = Pal::muted)
    {
        g.setColour (c);
        g.setFont (DeckLookAndFeel::get().cond (14.0f).withExtraKerningFactor (0.22f));
        g.drawText (text, r, j, false);
    }
}

DeckComponent::DeckComponent (AudioEngine& e, juce::PropertiesFile& settings)
    : engine (e), props (settings),
      pitch ("PITCH", false, [] (double v)
             {
                 auto pct = juce::roundToInt (v) * 2.4;
                 return (pct > 0 ? "+" : "") + juce::String (pct, 1) + "%";
             }),
      master (e)
{
    setWantsKeyboardFocus (true);

    // header
    setupLcd.topSize = 15.0f;
    setupLcd.onClick = [this] { openSetup (audioTab); };
    setupLcd.setTooltip ("Setup: audio driver and interface, sample rate, buffer, MIDI controllers and synths, tape character");
    addAndMakeVisible (setupLcd);

    // cassette bay
    cassette.onNameChanged = [this] (const juce::String& name)
    {
        if (auto* t = engine.getTape())
        {
            t->name = name;
            t->trackSheet.set (Inlay::title, name);
            t->markMetaChanged();
            inlayThumb.repaint();
        }
    };
    addAndMakeVisible (cassette);
    eject.onClick = [this] { showEjectMenu(); };
    addAndMakeVisible (eject);
    tapeSwitch.onChange = [this] (int p) { engine.tapeEngine.params.tapeSwitch = p; repaint (bayPanel); };
    tapeSwitch.setTooltip ("Record bias and EQ, and playback EQ. A tape going in sets it to its own type; move it to hear mismatches.");
    addAndMakeVisible (tapeSwitch);
    inlayThumb.onClick = [this] { openInlay(); };
    inlayThumb.setTooltip ("The cassette inlay: title, songs, counter numbers and track notes");
    addAndMakeVisible (inlayThumb);

    // transport
    resetBtn.textSize = 13.0f;
    rtzBtn.textSize = 13.0f;
    resetBtn.onClick = [this]
    {
        if (auto* t = engine.getTape()) { t->counterZero = t->position.load(); t->markMetaChanged(); }
    };
    rtzBtn.onClick = [this] { transport (TapeEngine::Command::rtz); };
    rtzBtn.setTooltip ("Return to zero: rewinds to where the counter reads 000");
    addAndMakeVisible (resetBtn);
    addAndMakeVisible (rtzBtn);

    status.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (status);

    recKey.onClick  = [this] { transport (TapeEngine::Command::rec); };
    rewKey.onClick  = [this] { transport (TapeEngine::Command::rew); };
    ffKey.onClick   = [this] { transport (TapeEngine::Command::ff); };
    playKey.onClick = [this] { transport (TapeEngine::Command::play); };
    stopKey.onClick = [this] { transport (TapeEngine::Command::stop); };
    for (auto* k : { &recKey, &rewKey, &ffKey, &playKey, &stopKey })
        addAndMakeVisible (k);

    pitch.slider.setRange (-5.0, 5.0, 1.0);
    pitch.slider.setDoubleClickReturnValue (true, 0.0);
    pitch.slider.setValue (engine.tapeEngine.params.pitchStep.load(), juce::dontSendNotification);
    pitch.slider.onValueChange = [this] { engine.tapeEngine.params.pitchStep = (int) pitch.slider.getValue(); pitch.repaint(); };
    addAndMakeVisible (pitch);

    for (auto* b : { &speedLo, &speedHi })
    {
        b->pill = true;
        b->textSize = 15.0f;
        b->setRadioGroupId (101);
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }
    speedLo.onClick = [this] { if (speedLo.getToggleState()) engine.tapeEngine.params.speedSel = 1; };
    speedHi.onClick = [this] { if (speedHi.getToggleState()) engine.tapeEngine.params.speedSel = 2; };
    (engine.tapeEngine.params.speedSel.load() == 2 ? speedHi : speedLo).setToggleState (true, juce::dontSendNotification);

    nrButton.lamp = true;
    nrButton.textSize = 14.0f;
    nrButton.onClick = [this] { engine.tapeEngine.params.nrOn = ! engine.tapeEngine.params.nrOn.load(); };
    addAndMakeVisible (nrButton);
    nrType.topSize = 14.0f;
    nrType.onClick = [this] { openSetup (tapeTab); };
    nrType.setTooltip ("Noise reduction type and tape character");
    addAndMakeVisible (nrType);

    // mixer
    for (int t = 0; t < numTracks; ++t)
    {
        auto* s = strips.add (new StripComponent (engine, t));
        s->onSourceMenu = [this] (int track, juce::Component*) { openPatch (track); };
        s->onEditSound = [this] (int track)
        {
            auto* inst = engine.getInstrument (track);
            if ((Source) engine.track[track].source.load() == Source::instrument && inst != nullptr && inst->getPlugin() != nullptr)
                openPluginWindow (track);
            else
                openPatch (track);
        };
        addAndMakeVisible (s);
    }
    addAndMakeVisible (master);

    // Keep the keyboard for the deck (transport and playing notes), except where text is typed.
    std::function<void (juce::Component&)> noFocus = [&] (juce::Component& c)
    {
        for (auto* child : c.getChildren())
        {
            if (dynamic_cast<juce::TextEditor*> (child) == nullptr)
            {
                child->setWantsKeyboardFocus (false);
                child->setMouseClickGrabsKeyboardFocus (false);
            }
            noFocus (*child);
        }
    };
    noFocus (*this);

    engine.onDeckStateApplied = [safe = juce::Component::SafePointer<DeckComponent> (this)]
    {
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->deckStateApplied(); });
    };
    // A plug-in's window always closes before the plug-in itself goes.
    engine.onInstrumentChanging = [this] (int t) { closePluginWindow (t); };

    loadStartupTape();
    if (engine.getStartupProblem().isNotEmpty())
        say (engine.getStartupProblem() + " - OPEN SETUP", 10.0);
    startTimerHz (30);
}

DeckComponent::~DeckComponent()
{
    stopTimer();
    engine.onDeckStateApplied = nullptr;
    engine.onInstrumentChanging = nullptr;
    overlay.reset();
    for (int t = 0; t < numTracks; ++t)
        pluginWindows[t].reset();
    setupWindow.reset();
}

void DeckComponent::prepareToQuit()
{
    stopTimer();
    engine.tapeEngine.command (TapeEngine::Command::stop);
    juce::Thread::sleep (60);   // let the audio thread stop the tape
    overlay.reset();
    saveTape (false, true, true);
    writer.waitUntilIdle();     // everything on disk before we go
    if (auto* t = engine.getTape())
        props.setValue ("lastTape", t->id);
    for (int t = 0; t < numTracks; ++t)
        pluginWindows[t].reset();
    setupWindow.reset();
}

//==============================================================================
void DeckComponent::resized()
{
    setupLcd.setBounds (980, 16, 260, 46);

    auto bp = bayPanel;
    cassette.setBounds (bp.getX() + 30, bp.getY() + 52, 400, 255);
    inlayThumb.setBounds (bp.getX() + 448, bp.getY() + 56, 86, 226);
    eject.setBounds (bp.getX() + 18, bp.getY() + 335, 100, 42);
    tapeSwitch.setBounds (bp.getX() + 132, bp.getY() + 333, TapeSwitch::labelW + 3 * TapeSwitch::segW + 8, 46);

    auto tp = transportPanel;
    resetBtn.setBounds (tp.getX() + 146, tp.getY() + 36, 62, 26);
    rtzBtn.setBounds (tp.getX() + 146, tp.getY() + 66, 62, 26);
    status.setBounds (tp.getX() + 218, tp.getY() + 36, 254, 56);

    auto keys = juce::Rectangle<int> (tp.getX() + 18, tp.getY() + 112, 524, 92).reduced (8, 10);
    for (auto* k : { &recKey, &rewKey, &ffKey, &playKey, &stopKey })
    {
        k->setBounds (keys.removeFromLeft (92));
        keys.removeFromLeft (10);
    }

    pitch.setBounds (tp.getX() + 2, tp.getY() + 236, 200, 44);
    speedLo.setBounds (tp.getX() + 236, tp.getY() + 246, 150, 34);
    speedHi.setBounds (tp.getX() + 392, tp.getY() + 246, 150, 34);
    nrButton.setBounds (tp.getX() + 18, tp.getY() + 318, 254, 42);
    nrType.setBounds (tp.getX() + 286, tp.getY() + 316, 256, 46);

    for (int t = 0; t < numTracks; ++t)
        strips[t]->setBounds (mixerX + t * (stripW + stripGap), mixerY, stripW, mixerH);
    master.setBounds (mixerX + numTracks * (stripW + stripGap), mixerY, masterW, mixerH);

    if (overlay != nullptr)
        overlay->setBounds (getLocalBounds());
}

void DeckComponent::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    g.fillAll (Pal::body);

    // ---- header ----
    g.setFont (lf.brand (46.0f));
    juce::AttributedString brand;
    brand.append ("TAPE", lf.brand (46.0f), Pal::cream);
    brand.append (utf ("\xc2\xb7"), lf.brand (46.0f), Pal::orange);
    brand.append ("FOUR", lf.brand (46.0f), Pal::cream);
    brand.draw (g, juce::Rectangle<float> (20, 12, 220, 52));
    smallCaps (g, utf ("MODEL TF-4 \xc2\xb7 4-TRACK CASSETTE MULTITRACKER"), { 232, 28, 420, 24 });

    auto jack = [&] (juce::Point<float> c, float r)
    {
        g.setColour (Pal::dark);
        g.fillEllipse (juce::Rectangle<float> (r * 2 + 10, r * 2 + 10).withCentre (c));
        g.setColour (Pal::line);
        g.fillEllipse (juce::Rectangle<float> (r * 2 + 8, r * 2 + 8).withCentre (c));
        g.setColour (Pal::darker);
        g.fillEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (c));
    };
    drawLamp (g, { 1262, 35, 8, 8 }, midiLamp, Pal::green);
    jack ({ 1290, 39 }, 12);
    smallCaps (g, "MIDI IN", { 1310, 28, 62, 24 });
    drawLamp (g, { 1384, 35, 8, 8 }, midiOutLamp, Pal::green);
    jack ({ 1412, 39 }, 12);
    smallCaps (g, "MIDI OUT", { 1432, 28, 76, 24 });
    drawLamp (g, { 1520, 34, 10, 10 }, true, Pal::red);
    smallCaps (g, "POWER", { 1536, 28, 50, 24 });

    // ---- cassette bay ----
    panelBackground (g, bayPanel, Pal::panel);
    smallCaps (g, utf ("CASSETTE \xc2\xb7 SIDE A ONLY \xc2\xb7 4 TRACKS ONE DIRECTION"),
               { bayPanel.getX() + 18, bayPanel.getY() + 12, 400, 20 });
    smallCaps (g, "INLAY", { bayPanel.getX() + 448, bayPanel.getY() + 12, 90, 20 });
    auto bay = juce::Rectangle<float> ((float) bayPanel.getX() + 18, (float) bayPanel.getY() + 36, 524, 286);
    g.setColour (Pal::bay);
    g.fillRoundedRectangle (bay, 8.0f);
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (bay.reduced (1), 8.0f, 2.0f);

    if (auto* t = engine.getTape())
    {
        auto info = t->productName() + utf (" \xc2\xb7 ") + iecTypeName (t->naturalPosition()) + utf (" \xc2\xb7 ")
                    + formatTime (t->getLengthSeconds() / engine.tapeEngine.params.speedSel.load())
                    + " @ " + speedLabel() + " IPS";
        g.setFont (lf.mono (14.0f));
        g.setColour (Pal::muted);
        g.drawFittedText (info, juce::Rectangle<int> (bayPanel.getX() + 18, bayPanel.getY() + 382, 524, 18), juce::Justification::centredRight, 1, 0.8f);
    }

    // ---- transport ----
    auto tp = transportPanel;
    panelBackground (g, tp, Pal::panel);
    smallCaps (g, "TAPE COUNTER", { tp.getX() + 18, tp.getY() + 12, 140, 20 });
    smallCaps (g, "STATUS", { tp.getX() + 218, tp.getY() + 12, 140, 20 });

    auto counter = juce::Rectangle<int> (tp.getX() + 18, tp.getY() + 36, 3 * 36 + 10, 56);
    g.setColour (Pal::darker);
    g.fillRoundedRectangle (counter.toFloat(), 4.0f);
    auto digits = counter.reduced (5);
    for (int i = 0; i < 3; ++i)
    {
        auto d = digits.removeFromLeft (34).toFloat();
        digits.removeFromLeft (2);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1f1e1c), d.getX(), d.getY(),
                                                 juce::Colour (0xff1f1e1c), d.getX(), d.getBottom(), false));
        g.fillRoundedRectangle (d, 2.0f);
        g.setColour (juce::Colour (0xff2c2b28));
        g.fillRect (d.withHeight (d.getHeight() * 0.5f).withY (d.getCentreY() - d.getHeight() * 0.25f));
        g.setColour (juce::Colour (0xfff2eee4));
        g.setFont (lf.mono (34.0f));
        g.drawText (counterText.substring (i, i + 1), d, juce::Justification::centred);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawHorizontalLine ((int) d.getCentreY(), d.getX(), d.getRight());
    }

    drawLamp (g, { (float) tp.getX() + 486, (float) tp.getY() + 44, 12, 12 }, recLamp, Pal::red);
    smallCaps (g, "REC", { tp.getX() + 504, tp.getY() + 40, 50, 20 });
    drawLamp (g, { (float) tp.getX() + 486, (float) tp.getY() + 72, 12, 12 }, playLamp, Pal::green);
    smallCaps (g, "PLAY", { tp.getX() + 504, tp.getY() + 68, 50, 20 });

    g.setColour (Pal::dark);
    g.fillRoundedRectangle (juce::Rectangle<float> ((float) tp.getX() + 18, (float) tp.getY() + 112, 524, 92), 8.0f);
    smallCaps (g, "TAPE SPEED", { tp.getX() + 236, tp.getY() + 224, 200, 18 });
}

//==============================================================================
juce::String DeckComponent::speedLabel() const
{
    return engine.tapeEngine.params.speedSel.load() == 2 ? utf ("3\xc2\xbe") : utf ("1\xe2\x85\x9e");
}

void DeckComponent::say (const juce::String& m, double seconds)
{
    message = m;
    messageUntil = juce::Time::getMillisecondCounterHiRes() * 0.001 + seconds;
}

void DeckComponent::transport (TapeEngine::Command c)
{
    if (tapeBusy)
    {
        say ("LOADING TAPE...");
        return;
    }
    if (engine.getTape() == nullptr)
    {
        say ("NO TAPE - PRESS EJECT");
        return;
    }
    if (engine.deviceManager.getCurrentAudioDevice() == nullptr)
    {
        say ("NO AUDIO DEVICE - OPEN SETUP", 4.0);
        return;
    }

    if (c == TapeEngine::Command::rec)
    {
        bool any = false;
        for (auto& p : engine.track) any = any || p.armed.load();
        if (! any) { say ("ARM A TRACK FIRST"); return; }
    }
    message = {};
    engine.tapeEngine.command (c);
}

void DeckComponent::timerCallback()
{
    ++frame;
    const bool blink = (frame / 8) % 2 == 0;
    auto* tape = engine.getTape();
    auto mode = engine.tapeEngine.getMode();
    auto pos = tape != nullptr ? tape->position.load() : 0.0;

    cassette.update (tape, pos);

    if (engine.tapeEngine.takeEndOfTapeFlag()) say ("END OF TAPE", 4.0);
    if (engine.tapeEngine.takeNoArmFlag())     say ("ARM A TRACK FIRST");

    // counter
    if (tape != nullptr)
    {
        auto c = (int) std::floor ((pos - tape->counterZero.load()) / cellRate / 2.0);
        c = ((c % 1000) + 1000) % 1000;
        counterText = juce::String (c).paddedLeft ('0', 3);
    }

    // status display
    auto nominal = engine.tapeEngine.getNominalSpeed();
    auto pitchStep = engine.tapeEngine.params.pitchStep.load();
    auto spd = speedLabel() + (pitchStep != 0 ? " " + juce::String (pitchStep > 0 ? "+" : "") + juce::String (pitchStep * 2.4, 1) + "%" : juce::String());
    juce::String text;
    switch (mode)
    {
        case Mode::stop: text = "STOP"; break;
        case Mode::play: text = "PLAY " + spd; break;
        case Mode::rec:
        {
            juce::StringArray armedList;
            for (int t = 0; t < numTracks; ++t) if (engine.track[t].armed.load()) armedList.add (juce::String (t + 1));
            text = "REC " + spd + " TRK " + armedList.joinIntoString ("+");
            break;
        }
        case Mode::ff:  text = "FAST FORWARD"; break;
        case Mode::rew: text = engine.tapeEngine.isReturningToZero() ? "RETURN TO ZERO" : "REWIND"; break;
    }
    if (message.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() * 0.001 < messageUntil)
        text = message;
    status.top = text;
    status.sub = tape != nullptr && engine.tapeEngine.params.tapeSwitch.load() != tape->naturalPosition()
                     ? juce::String (engine.tapeEngine.params.tapeSwitch.load() < tape->naturalPosition() ? "BIAS LOW: " : "BIAS HIGH: ")
                           + iecTypeName (tape->naturalPosition()) + " TAPE AT " + positionName (engine.tapeEngine.params.tapeSwitch.load())
                 : tape != nullptr ? "REMAIN " + formatTime ((tape->getLengthCells() - pos) / cellRate / std::max (0.1f, nominal))
                                 : juce::String ("NO TAPE");
    status.repaintIfChanged();

    auto latch = [] (KeyButton& k, bool on) { if (k.latched != on) { k.latched = on; k.repaint(); } };
    latch (recKey, mode == Mode::rec);
    latch (playKey, mode == Mode::play || mode == Mode::rec);
    latch (ffKey, mode == Mode::ff);
    latch (rewKey, mode == Mode::rew);
    recLamp = mode == Mode::rec;
    playLamp = mode == Mode::play || mode == Mode::rec;

    if (tape != nullptr && (tapeSwitch.position != engine.tapeEngine.params.tapeSwitch.load() || tapeSwitch.tapeType != tape->naturalPosition()))
    {
        tapeSwitch.position = engine.tapeEngine.params.tapeSwitch.load();
        tapeSwitch.tapeType = tape->naturalPosition();
        tapeSwitch.repaint();
    }
    tapeSwitch.hasTape = tape != nullptr;

    if (nrButton.lampOn != engine.tapeEngine.params.nrOn.load())
    {
        nrButton.lampOn = engine.tapeEngine.params.nrOn.load();
        nrButton.repaint();
    }
    static const char* nrNames[] { "2:1 COMPANDER", "SLIDING BAND B", "SLIDING BAND C" };
    nrType.top = "NR TYPE";
    nrType.sub = nrNames[juce::jlimit (0, 2, engine.tapeEngine.params.nrType.load())];
    nrType.repaintIfChanged();

    const bool inBefore = midiLamp, outBefore = midiOutLamp;
    if (engine.midiFlash.exchange (false)) midiHold = 4;
    midiLamp = midiHold > 0;
    if (midiHold > 0) --midiHold;
    if (engine.midiOutFlash.exchange (false)) midiOutHold = 4;
    midiOutLamp = midiOutHold > 0;
    if (midiOutHold > 0) --midiOutHold;

    for (auto* s : strips) s->refresh (blink);
    master.refresh();

    if (frame % 15 == 1)
    {
        setupLcd.top = engine.getDeviceSummary();
        setupLcd.sub = utf ("SETUP \xe2\x80\xba");
        setupLcd.repaintIfChanged();
    }

    // The counter and lamps are painted by the deck itself: only when they change.
    if (auto shown = counterText + (recLamp ? "R" : "") + (playLamp ? "P" : ""); shown != shownTransport)
    {
        shownTransport = shown;
        repaint (transportPanel.getX(), transportPanel.getY(), transportPanel.getWidth(), 100);
    }
    if (inBefore != midiLamp || outBefore != midiOutLamp)
        repaint (1250, 20, 340, 44);

    // Auto-save the tape once it has been stopped for a second, and again each second after any change.
    // While recording, the new audio goes to disk every five seconds, so a crash costs seconds, not a take.
    stoppedFrames = mode == Mode::stop ? stoppedFrames + 1 : 0;
    recordingFrames = mode == Mode::rec ? recordingFrames + 1 : 0;
    if (tape != nullptr && ! tapeBusy && ! writer.isBusy())
    {
        if (stoppedFrames >= 30 && frame % 30 == 0 && tape->needsSaving())
            saveTape (false);
        else if (recordingFrames > 0 && recordingFrames % 150 == 0 && tape->hasUnsavedAudio())
            saveTape (false, false);
    }

    // Knobs, faders and sources are kept too, every 5 seconds while stopped (only written if changed).
    if (stoppedFrames > 0 && frame % 150 == 0)
        engine.saveSettings();
}

//==============================================================================
void DeckComponent::loadStartupTape()
{
    auto last = props.getValue ("lastTape");
    switchTape ([last]
                {
                    std::unique_ptr<Tape> tape;
                    if (last.isNotEmpty())
                        tape = TapeShelf::load (last);
                    if (tape == nullptr)
                    {
                        auto shelf = TapeShelf::list();
                        if (! shelf.empty())
                            tape = TapeShelf::load (shelf.front().id);
                    }
                    if (tape == nullptr)
                    {
                        tape = TapeShelf::createBlank (1);
                        TapeShelf::save (*tape);
                    }
                    return tape;
                },
                [] (Tape&) {}, "LOADING TAPE...");
}

void DeckComponent::tapeChanged()
{
    auto* t = engine.getTape();
    cassette.setTape (t);
    inlayThumb.tape = t;
    inlayThumb.repaint();
    if (t != nullptr)
        props.setValue ("lastTape", t->id);
    repaint();
}

void DeckComponent::saveTape (bool announce, bool withDeck, bool fresh)
{
    auto* t = engine.getTape();
    if (t == nullptr) return;

    // The deck as it is now goes with the tape, so it can come back with it.
    if (withDeck)
    {
        auto deck = engine.createDeckState (fresh);
        if (t->deckState == nullptr || ! t->deckState->isEquivalentTo (deck.get(), false))
        {
            t->deckState = std::move (deck);
            t->deckNeedsWriting = true;
        }
    }

    writer.saveTape (*t, [safe = juce::Component::SafePointer<DeckComponent> (this), announce] (bool ok)
    {
        if (safe == nullptr) return;
        if (! ok)          safe->say ("COULD NOT SAVE TAPE", 5.0);
        else if (announce) safe->say ("SAVED");
    });
}

void DeckComponent::switchTape (std::function<std::unique_ptr<Tape>()> make, std::function<void (Tape&)> after, const juce::String& busyText)
{
    if (tapeBusy) return;
    tapeBusy = true;
    say (busyText, 120.0);
    closeOverlay();

    // Save what's in the deck first; the writer does one job at a time, in order.
    saveTape (false, true, true);

    auto made = std::make_shared<std::unique_ptr<Tape>>();
    writer.enqueue ([made, make] { *made = make(); },
                    [safe = juce::Component::SafePointer<DeckComponent> (this), made, after]
                    {
                        if (safe == nullptr) return;
                        auto& d = *safe;
                        d.tapeBusy = false;
                        d.say ({}, 0.0);
                        if (*made == nullptr)
                        {
                            d.say ("THAT TAPE WOULD NOT LOAD", 4.0);
                            return;
                        }
                        d.writer.retire (d.engine.swapTape (std::move (*made)));
                        d.tapeChanged();
                        after (*d.engine.getTape());
                    });
}

void DeckComponent::insertBlank (int type, int model)
{
    switchTape ([type, model]
                {
                    auto tape = TapeShelf::createBlank (type, model);
                    TapeShelf::save (*tape);
                    return tape;
                },
                [safe = juce::Component::SafePointer<DeckComponent> (this)] (Tape& t)
                {
                    if (safe != nullptr) safe->say ("NEW BLANK " + t.productName());
                },
                "MAKING A BLANK TAPE...");
}

void DeckComponent::loadFromShelf (const juce::String& id)
{
    if (auto* cur = engine.getTape(); cur != nullptr && cur->id == id)
        return;

    switchTape ([id] { return TapeShelf::load (id); },
                [safe = juce::Component::SafePointer<DeckComponent> (this)] (Tape& loaded)
                {
                    if (safe == nullptr) return;
                    auto& d = *safe;
                    auto label = "LOADED: " + loaded.name.toUpperCase();
                    if (d.engine.recallDeckWithTape.load() && loaded.deckState != nullptr)
                    {
                        auto state = std::make_unique<juce::XmlElement> (*loaded.deckState);
                        d.engine.applyDeckState (*state);   // plug-in windows close as their plug-ins go
                        label += "  + DECK";
                    }
                    d.say (label);
                },
                "LOADING TAPE...");
}

void DeckComponent::showEjectMenu()
{
    if (tapeBusy)
    {
        say ("LOADING TAPE...");
        return;
    }
    if (engine.tapeEngine.getMode() != Mode::stop)
    {
        say ("STOP THE TAPE FIRST");
        return;
    }

    juce::PopupMenu m;
    m.addSectionHeader ("INSERT NEW BLANK TAPE");
    for (int k = 0; k < numTapeModels; ++k)
    {
        const auto& tm = tapeModels[k];
        juce::PopupMenu lengths;
        for (int i = 0; i < numTapeTypes; ++i)
            lengths.addItem (juce::String (tm.name) + juce::String (tapeTypes[i].minutes * 2) + "   " + juce::String (tapeTypes[i].minutes) + " MIN PER SIDE",
                             [this, i, k] { insertBlank (i, k); });
        m.addSubMenu (juce::String (tm.name) + utf ("   \xc2\xb7   ") + utf (tm.position) + utf ("   \xc2\xb7   ") + utf (tm.line), lengths);
    }

    m.addSeparator();
    m.addSectionHeader ("TAPE SHELF");
    auto* cur = engine.getTape();
    auto shelf = TapeShelf::list();
    if (shelf.empty())
        m.addItem ("No saved tapes yet", false, false, nullptr);
    for (auto& e : shelf)
    {
        auto text = (e.name.isNotEmpty() ? e.name : juce::String ("(no label)")) + utf ("   \xc2\xb7   ")
                    + tapeModels[e.model].name + juce::String (tapeTypes[juce::jlimit (0, numTapeTypes - 1, e.type)].minutes * 2) + "  " + formatTime (e.usedSeconds) + " used  "
                    + juce::String (e.tracksUsed) + "/4 trk  " + e.created;
        auto id = e.id;
        m.addItem (text, true, cur != nullptr && cur->id == id, [this, id] { loadFromShelf (id); });
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&eject).withMinimumWidth (360));
}

//==============================================================================
void DeckComponent::closePluginWindow (int t)
{
    if (pluginWindows[t] == nullptr) return;
    pluginWindows[t].reset();
    if (auto* inst = engine.getInstrument (t))
        inst->setEditorOpen (false);
}

void DeckComponent::openPluginWindow (int t)
{
    if (auto* inst = engine.getInstrument (t))
        if (auto* plugin = inst->getPlugin())
        {
            if (pluginWindows[t] != nullptr) { pluginWindows[t]->toFront (true); return; }
            inst->setEditorOpen (true);
            pluginWindows[t] = std::make_unique<PluginWindow> (*plugin, [this, t]
            {
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<DeckComponent> (this), t]
                {
                    if (safe != nullptr) safe->closePluginWindow (t);
                });
            });
            pluginWindows[t]->onDeckKey = [this] (const juce::KeyPress& k)
            {
                // Space, R and Home drive the transport from the plug-in's window too.
                if (k == juce::KeyPress::spaceKey || k == juce::KeyPress::homeKey
                    || k.getTextCharacter() == 'r' || k.getTextCharacter() == 'R')
                    return keyPressed (k);
                return false;
            };
        }
}

void DeckComponent::showOverlay (std::unique_ptr<juce::Component> card, juce::Rectangle<int> cardBounds)
{
    overlay = std::make_unique<Overlay> (std::move (card), cardBounds);
    overlay->onDismiss = [this] { closeOverlay(); };
    overlay->setBounds (getLocalBounds());
    addAndMakeVisible (*overlay);
    overlay->toFront (false);
}

void DeckComponent::closeOverlay()
{
    if (overlay == nullptr) return;
    overlay->setVisible (false);
    // Don't delete the card while one of its buttons is still on the call stack.
    juce::MessageManager::callAsync ([old = std::shared_ptr<Overlay> (overlay.release())] {});
    engine.saveSettings();
    grabKeyboardFocus();
    repaint();
}

void DeckComponent::openPatch (int t)
{
    closeOverlay();
    auto card = std::make_unique<PatchCard> (engine, t);
    card->onDone = [this] { closeOverlay(); };
    card->onShowPluginWindow = [this] (int track) { openPluginWindow (track); };
    card->onFindInstruments = [this] { closeOverlay(); openSetup (instrumentsTab); };
    showOverlay (std::move (card), juce::Rectangle<int> (820, 660).withCentre ({ designWidth / 2, designHeight / 2 }));
}

void DeckComponent::openInlay()
{
    auto* t = engine.getTape();
    if (t == nullptr) { say ("NO TAPE - PRESS EJECT"); return; }
    closeOverlay();
    auto card = std::make_unique<InlayCard> (*t);
    card->onClose = [this] { closeOverlay(); };
    card->onTitleChanged = [this] (const juce::String&)
    {
        if (auto* tape = engine.getTape())
            cassette.setTape (tape);
        inlayThumb.repaint();
    };
    // Centre the card itself; the close button hangs off to its right.
    showOverlay (std::move (card), juce::Rectangle<int> (InlayCard::totalW, InlayCard::cardH + 14)
                                       .withCentre ({ designWidth / 2 + (InlayCard::gap + InlayCard::closeW) / 2, designHeight / 2 }));
    overlay->cardShadow = false;
}

void DeckComponent::deckStateApplied()
{
    for (auto* s : strips) s->syncFromEngine();
    master.syncFromEngine();
    pitch.slider.setValue (engine.tapeEngine.params.pitchStep.load(), juce::dontSendNotification);
    pitch.repaint();
    (engine.tapeEngine.params.speedSel.load() == 2 ? speedHi : speedLo).setToggleState (true, juce::dontSendNotification);
    repaint();
}

void DeckComponent::openSetup (int tab)
{
    if (setupWindow == nullptr)
        setupWindow = std::make_unique<SetupWindow> (engine, props);
    setupWindow->showTab (tab);
}

//==============================================================================
void DeckComponent::mouseDown (const juce::MouseEvent&)
{
    grabKeyboardFocus();
}

bool DeckComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && overlay != nullptr) { closeOverlay(); return true; }
    auto mode = engine.tapeEngine.getMode();

    if (key == juce::KeyPress::spaceKey)
    {
        transport (mode == Mode::play || mode == Mode::rec ? TapeEngine::Command::stop : TapeEngine::Command::play);
        return true;
    }
    if (key == juce::KeyPress::homeKey)  { transport (TapeEngine::Command::rtz); return true; }
    if (key.getTextCharacter() == 'r' || key.getTextCharacter() == 'R') { transport (TapeEngine::Command::rec); return true; }
    if (key.getTextCharacter() == 'z') { keyOctave = std::max (0, keyOctave - 1); say ("KEYBOARD OCTAVE " + juce::String (keyOctave)); return true; }
    if (key.getTextCharacter() == 'x') { keyOctave = std::min (8, keyOctave + 1); say ("KEYBOARD OCTAVE " + juce::String (keyOctave)); return true; }

    return keyMap.containsChar (key.getTextCharacter());
}

bool DeckComponent::keyStateChanged (bool)
{
    bool used = false;
    for (int i = 0; i < keyMap.length(); ++i)
    {
        bool down = juce::KeyPress::isKeyCurrentlyDown (juce::KeyPress (keyMap[i]).getKeyCode());
        if (down != keysDown[(size_t) i])
        {
            keysDown[(size_t) i] = down;
            auto note = juce::jlimit (0, 127, keyOctave * 12 + 12 + i);
            engine.injectMidi (down ? juce::MidiMessage::noteOn (1, note, (juce::uint8) 100)
                                    : juce::MidiMessage::noteOff (1, note));
            used = true;
        }
    }
    return used;
}
} // namespace tf
