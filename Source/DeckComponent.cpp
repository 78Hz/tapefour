#include "DeckComponent.h"
#include "SetupWindow.h"
#include "MasterDeckCard.h"

namespace tf
{
//==============================================================================
namespace
{
    // design coordinates
    // Portastudio layout: mixer on the left, the tape section in a column on the right.
    const juce::Rectangle<int> bayPanel { 1020, 74, 560, 404 };
    const juce::Rectangle<int> transportPanel { 1020, 490, 560, 390 };
    enum { audioTab = 0, midiTab = 1, deckTab = 2, instrumentsTab = 3 };
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
                 auto pct = std::abs (v) < 0.05 ? 0.0 : v;
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
    learnBtn.onClick = [this] { setLearnMode (learnLayer == nullptr); };
    learnBtn.setTooltip ("MIDI learn: click a control, then move a knob, fader or button on your controller. Right-click any control to learn or forget it.");
    addAndMakeVisible (learnBtn);

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
    resetBtn.textSize = 11.0f;
    rtzBtn.textSize = 11.0f;
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
    // REC and PLAY carry their own LEDs: no separate lamps beside the status display.
    recKey.hasLed = true;
    recKey.ledColour = juce::Colour (0xffff3b22);
    playKey.hasLed = true;
    playKey.ledColour = juce::Colour (0xff4fb33a);
    for (auto* k : { &recKey, &rewKey, &ffKey, &playKey, &stopKey })
        addAndMakeVisible (k);

    // Continuous varispeed, ±12%. Dragging is fine-grained; double-click centres it.
    pitch.slider.setRange (-12.0, 12.0, 0.0);
    pitch.slider.setMouseDragSensitivity (600);
    pitch.slider.setDoubleClickReturnValue (true, 0.0);
    pitch.slider.setValue (engine.tapeEngine.params.pitch.load(), juce::dontSendNotification);
    pitch.slider.onValueChange = [this] { engine.tapeEngine.params.pitch = (float) pitch.slider.getValue(); pitch.repaint(); };
    addAndMakeVisible (pitch);

    for (auto* b : { &speedLo, &speedHi })
    {
        b->textSize = 13.0f;
        addAndMakeVisible (b);
    }
    speedLo.onClick = [this] { engine.tapeEngine.params.speedSel = 1; syncSpeed(); };
    speedHi.onClick = [this] { engine.tapeEngine.params.speedSel = 2; syncSpeed(); };
    syncSpeed();

    nrButton.textSize = 13.0f;
    nrButton.onClick = [this] { engine.tapeEngine.params.nrOn = ! engine.tapeEngine.params.nrOn.load(); nrButton.setLed (engine.tapeEngine.params.nrOn.load()); };
    addAndMakeVisible (nrButton);
    nrType.topSize = 14.0f;
    nrType.onClick = [this] { openSetup (deckTab); };
    nrType.setTooltip ("Noise reduction type and tape character");
    addAndMakeVisible (nrType);

    // mixer
    for (int t = 0; t < numTracks; ++t)
    {
        auto* s = strips.add (new StripComponent (engine, t));
        s->onOpenChain = [this] (int track) { if (chainTrack == track) closeOverlay(); else openChain (track); };
        s->onEditSound = [this] (int track) { editSound (track); };
        addAndMakeVisible (s);
    }
    addAndMakeVisible (master);
    master.onMasterDeck = [this] { openMasterDeck(); };
    master.onSide = [this] { flipMasterTape(); };
    master.onMixdown = [this] { toggleMixdown(); };

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
    engine.onBlockRemoving = [this] (int t, int id) { closeBlockWindow (t, id); };

    // MIDI learn
    gatherLearnTargets();
    for (auto& lt : learnTargets)
        lt.component->addMouseListener (this, true);   // right-click: learn or forget
    engine.mapper.onLearned = [this] (const MidiMapper::Mapping& m)
    {
        say ("LEARNED: " + engine.mapper.describeTarget (m) + utf (" \xe2\x86\x90 ") + engine.mapper.describeMessage (m), 4.0);
        engine.saveSettings();
        if (learnLayer != nullptr) learnLayer->repaint();
    };

    if (auto f = props.getXmlValue ("exportFormats")) exportFormats = ExportFormats::fromXml (f.get());
    loadStartupTape();
    // The master tape that was in the master deck (or a new one of the kind chosen).
    {
        const auto id = engine.masterDeck.settings.masterId;
        if (id.isNotEmpty()) loadMasterTape (id);
        else if (! engine.masterDeck.settings.direct()) newMasterTape();
    }
    if (engine.getStartupProblem().isNotEmpty())
        say (engine.getStartupProblem() + " - OPEN SETUP", 10.0);
    startTimerHz (30);
}

DeckComponent::~DeckComponent()
{
    stopTimer();
    engine.onDeckStateApplied = nullptr;
    engine.onBlockRemoving = nullptr;
    engine.mapper.onLearned = nullptr;
    engine.mapper.cancelLearn();
    for (auto& lt : learnTargets)
        lt.component->removeMouseListener (this);
    learnLayer.reset();
    overlay.reset();
    pluginWindows.clear();
    setupWindow.reset();
}

void DeckComponent::prepareToQuit()
{
    stopTimer();
    engine.tapeEngine.command (TapeEngine::Command::stop);
    // The audio thread carries the command out: wait for it (briefly; with no device there's nothing to stop).
    for (int i = 0; i < 100 && engine.tapeEngine.getMode() != Mode::stop
                    && engine.deviceManager.getCurrentAudioDevice() != nullptr; ++i)
        juce::Thread::sleep (5);
    overlay.reset();
    // A mixdown running ends with the tape: wait for its files, then keep the master tape.
    engine.masterDeck.disarm();
    for (int i = 0; i < 300 && engine.masterDeck.getState() != MasterDeck::State::off
                    && engine.masterDeck.getState() != MasterDeck::State::done
                    && engine.deviceManager.getCurrentAudioDevice() != nullptr; ++i)
        juce::Thread::sleep (10);
    collectMixdown();
    if (auto* m = engine.masterDeck.getTape()) writer.saveTape (*m);
    saveTape (false, true, true);
    writer.waitUntilIdle();     // everything on disk before we go
    if (auto* t = engine.getTape())
        props.setValue ("lastTape", t->id);
    pluginWindows.clear();
    setupWindow.reset();
}

//==============================================================================
void DeckComponent::resized()
{
    setupLcd.setBounds (980, 16, 260, 46);
    learnBtn.setBounds (888, 16, 76, 46);

    auto bp = bayPanel;
    cassette.setBounds (bp.getX() + 30, bp.getY() + 52, 400, 255);
    inlayThumb.setBounds (bp.getX() + 448, bp.getY() + 56, 86, 226);
    eject.setBounds (bp.getX() + 18, bp.getY() + 336, 100, 42);
    tapeSwitch.setBounds (bp.getX() + 136, bp.getY() + 334, TapeSwitch::labelW + 3 * TapeSwitch::segW, 46);

    auto tp = transportPanel;
    resetBtn.setBounds (tp.getX() + 146, tp.getY() + 36, 62, 26);
    rtzBtn.setBounds (tp.getX() + 146, tp.getY() + 66, 62, 26);
    status.setBounds (tp.getX() + 218, tp.getY() + 36, 324, 56);

    auto keys = juce::Rectangle<int> (tp.getX() + 18, tp.getY() + 112, 524, 92).reduced (8, 10);
    for (auto* k : { &recKey, &rewKey, &ffKey, &playKey, &stopKey })
    {
        k->setBounds (keys.removeFromLeft (92));
        keys.removeFromLeft (10);
    }

    pitch.setBounds (tp.getX() + 2, tp.getY() + 236, 200, 44);
    speedLo.setBounds (tp.getX() + 236, tp.getY() + 246, 148, 40);
    speedHi.setBounds (tp.getX() + 394, tp.getY() + 246, 148, 40);
    nrButton.setBounds (tp.getX() + 18, tp.getY() + 316, 254, 46);
    nrType.setBounds (tp.getX() + 286, tp.getY() + 316, 256, 46);

    for (int t = 0; t < numTracks; ++t)
        strips[t]->setBounds (mixerX + t * (stripW + stripGap), mixerY, stripW, mixerH);
    master.setBounds (mixerX + numTracks * (stripW + stripGap), mixerY, masterW, mixerH);

    if (overlay != nullptr)
        overlay->setBounds (getLocalBounds());
    if (learnLayer != nullptr)
        learnLayer->setBounds (getLocalBounds());
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


    g.setColour (Pal::dark);
    g.fillRoundedRectangle (juce::Rectangle<float> ((float) tp.getX() + 18, (float) tp.getY() + 112, 524, 92), 8.0f);
    smallCaps (g, "TAPE SPEED", { tp.getX() + 236, tp.getY() + 224, 200, 18 });
}

//==============================================================================
void DeckComponent::syncSpeed()
{
    const bool hi = engine.tapeEngine.params.speedSel.load() == 2;
    speedLo.setLed (! hi);
    speedHi.setLed (hi);
}

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
        say (busyText.isNotEmpty() ? busyText : juce::String ("LOADING TAPE..."));
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

    if (c == TapeEngine::Command::rec && engine.getTape()->protectA.load())
    {
        say ("WRITE-PROTECTED: THE TAB IS BROKEN OUT", 4.0);
        return;
    }
    if (c == TapeEngine::Command::rec)
    {
        bool any = false;
        for (auto& p : engine.track) any = any || p.armed.load();
        if (! any) { say ("ARM A TRACK FIRST"); return; }

        // Recording grows the tape's files: warn (but still record) when the disk couldn't take the
        // rest of the tape. Saving keeps retrying, so nothing already recorded is lost by this.
        if (TapeShelf::isShort (TapeShelf::bytesForFullTape (engine.getTape()->getTypeInfo())))
        {
            engine.tapeEngine.command (c);
            say ("DISK NEARLY FULL - THE TAKE MAY NOT ALL BE SAVED", 5.0);
            return;
        }
    }
    message = {};
    engine.tapeEngine.command (c);
}

void DeckComponent::timerCallback()
{
    ++frame;
    // The cassette's calendar: new recordings are stamped with today, old ones age from theirs.
    if (frame % (30 * 60) == 1)
        if (auto* t = engine.getTape()) t->setToday (tfmt::dayNumberToday());
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
    auto pitchPct = engine.tapeEngine.params.pitch.load();
    auto spd = speedLabel() + (std::abs (pitchPct) >= 0.05f ? " " + juce::String (pitchPct > 0 ? "+" : "") + juce::String (pitchPct, 1) + "%" : juce::String());
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
    // A mixdown shows on the status display while it runs, and while it waits for PLAY.
    juce::String mixSub;
    {
        const auto& md = engine.masterDeck;
        const auto st = md.getState();
        if (st == MasterDeck::State::recording || st == MasterDeck::State::tailing)
        {
            text = (mode == Mode::rec ? "REC" : "PLAY") + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 MIX ")) + juce::String (md.currentTakeNumber())
                   + (blink ? juce::String (juce::CharPointer_UTF8 (" \xe2\x97\x8f ")) : juce::String ("   ")) + formatTime (md.takeSeconds());
            mixSub = md.clippedAtSeconds() >= 0.0 ? "CLIPPED AT " + formatTime (md.clippedAtSeconds()) + " - LOWER MASTER"
                   : md.settings.direct() ? utf ("\xe2\x86\x92 ") + exportFormats.describe()
                   : utf ("\xe2\x86\x92 MASTER SIDE ") + (md.settings.side == 0 ? "A " : "B ")
                         + formatTime (md.sideUsedSeconds (md.settings.side) + md.takeSeconds() + (md.takes.empty() ? MasterDeck::leaderSeconds : MasterDeck::gapSeconds))
                         + " " + exportFormats.describe();
        }
        else if (st == MasterDeck::State::finishing)
        {
            text = "SAVING MIX " + juce::String (md.currentTakeNumber());
            mixSub = exportFormats.describe();
        }
        else if (st == MasterDeck::State::armed && mode == Mode::stop)
        {
            text = "MIXDOWN ARMED";
            mixSub = "PLAY STARTS MIX " + juce::String (md.currentTakeNumber()) + ", STOP ENDS IT";
        }
    }
    if (message.isNotEmpty() && juce::Time::getMillisecondCounterHiRes() * 0.001 < messageUntil)
        text = message;
    else if (learnLayer != nullptr)
        text = engine.mapper.isLearning() ? "LEARN: MOVE A CONTROL" : "LEARN: CLICK A CONTROL";
    status.top = text;
    status.sub = mixSub.isNotEmpty() ? mixSub
               : engine.soloedTracksAllMuted() ? juce::String ("SOLOED TRACK IS MUTED")
               : tape != nullptr && engine.tapeEngine.params.tapeSwitch.load() != tape->naturalPosition()
                     ? juce::String (engine.tapeEngine.params.tapeSwitch.load() < tape->naturalPosition() ? "BIAS LOW: " : "BIAS HIGH: ")
                           + iecTypeName (tape->naturalPosition()) + " TAPE AT " + positionName (engine.tapeEngine.params.tapeSwitch.load())
                 : tape != nullptr ? "REMAIN " + formatTime (((double) tape->getLengthCells() - pos) / cellRate / std::max (0.1f, nominal))
                                 : juce::String ("NO TAPE");
    status.repaintIfChanged();

    auto latch = [] (KeyButton& k, bool on) { if (k.latched != on) { k.latched = on; k.repaint(); } };
    latch (recKey, mode == Mode::rec);
    latch (playKey, mode == Mode::play || mode == Mode::rec);
    latch (ffKey, mode == Mode::ff);
    latch (rewKey, mode == Mode::rew);
    recKey.setLed (mode == Mode::rec);
    playKey.setLed (mode == Mode::play || mode == Mode::rec);

    if (tape != nullptr && (tapeSwitch.position != engine.tapeEngine.params.tapeSwitch.load() || tapeSwitch.tapeType != tape->naturalPosition()))
    {
        tapeSwitch.position = engine.tapeEngine.params.tapeSwitch.load();
        tapeSwitch.tapeType = tape->naturalPosition();
    }
    tapeSwitch.hasTape = tape != nullptr;
    tapeSwitch.refresh();

    nrButton.setLed (engine.tapeEngine.params.nrOn.load());
    eject.setEnabled (mode == Mode::stop && ! tapeBusy);
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
    master.refresh (exportFormats.describe(), blink);
    collectMixdown();

    // MIDI learn and remote control
    engine.mapper.poll();
    if (auto c = engine.remoteCommand.exchange (0); c > 0)
        remoteControl ((MidiMapper::Control) (c - 1));
    if (auto id = engine.mapper.pickupWaitingId.exchange (0); id != 0)
        for (auto& m : engine.mapper.getMappings())
            if (m.id == id)
                say ("PICKUP: TURN " + juce::String (engine.mapper.pickupDirection.load() > 0 ? "UP" : "DOWN") + " TO TAKE "
                     + engine.mapper.describeTarget (m), 1.5);
    learnBtn.setLed (learnLayer != nullptr && (! engine.mapper.isLearning() || blink));
    if (learnLayer != nullptr)
    {
        // Redrawn when what it shows changes: a mapping, or what's being learned.
        const auto target = engine.mapper.getLearnTarget();
        const auto shown = juce::String (engine.mapper.changes.load()) + ":" + juce::String ((int) engine.mapper.isLearning())
                           + ":" + juce::String ((int) target.control) + ":" + juce::String (target.track);
        if (shown != learnShown) { learnShown = shown; learnLayer->repaint(); }
    }
    // controls a controller moved
    if (std::abs (pitch.slider.getValue() - engine.tapeEngine.params.pitch.load()) > 1.0e-4)
    {
        pitch.slider.setValue (engine.tapeEngine.params.pitch.load(), juce::dontSendNotification);
        pitch.repaint();
    }
    syncSpeed();

    if (frame % 15 == 1)
    {
        setupLcd.top = engine.getDeviceSummary();
        setupLcd.sub = utf ("SETUP \xe2\x80\xba");
        setupLcd.repaintIfChanged();
    }

    // The counter and lamps are painted by the deck itself: only when they change.
    if (counterText != shownTransport)
    {
        shownTransport = counterText;
        repaint (transportPanel.getX(), transportPanel.getY(), transportPanel.getWidth(), 100);
    }
    if (inBefore != midiLamp || outBefore != midiOutLamp)
        repaint (1250, 20, 340, 44);

    // Auto-save the tape once it has been stopped for a second, and again each second after any change.
    // While recording, the new audio goes to disk every five seconds, so a crash costs seconds, not a take.
    stoppedFrames = mode == Mode::stop ? stoppedFrames + 1 : 0;
    if (mode != Mode::stop) hasRunSinceCapture = true;
    recordingFrames = mode == Mode::rec ? recordingFrames + 1 : 0;
    if (tape != nullptr && ! tapeBusy && ! writer.isBusy())
    {
        if (stoppedFrames >= 30 && frame % 30 == 0 && tape->needsSaving())
            saveTape (false);
        else if (recordingFrames > 0 && recordingFrames % 150 == 0 && tape->hasUnsavedAudio())
            saveTape (false, false);
    }

    // Knobs, faders and sources are kept too, every 5 seconds while stopped (only written if changed).
    // A second after the tape stops, the plug-ins are asked for their sound as well.
    if (stoppedFrames == 30 && hasRunSinceCapture)
    {
        hasRunSinceCapture = false;   // not at start-up: only after the tape has run
        engine.saveSettings (true);
    }
    else if (stoppedFrames > 0 && frame % 150 == 0)
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

    // The session as it is now goes with the tape, so it can come back with it.
    if (withDeck)
    {
        auto deck = engine.createDeckState (fresh);
        if (t->session == nullptr || ! t->session->isEquivalentTo (deck.get(), false))
        {
            t->session = std::move (deck);
            t->sessionNeedsWriting = true;
        }
    }

    writer.saveTape (*t, [safe = juce::Component::SafePointer<DeckComponent> (this), announce] (bool ok)
    {
        if (safe == nullptr) return;
        if (! ok)          safe->say ("COULD NOT SAVE TAPE", 5.0);
        else if (announce) safe->say ("SAVED");
    });
}

void DeckComponent::switchTape (std::function<std::unique_ptr<Tape>()> make, std::function<void (Tape&)> after, const juce::String& whatItsDoing)
{
    if (tapeBusy) return;
    tapeBusy = true;
    busyText = whatItsDoing;
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
                        d.busyText = {};
                        d.say ({}, 0.0);
                        if (*made == nullptr)
                        {
                            d.say ("THAT TAPE WOULD NOT LOAD", 4.0);
                            d.pendingCassette = juce::File();
                            return;
                        }
                        d.writer.retire (d.engine.swapTape (std::move (*made)));
                        d.tapeChanged();
                        after (*d.engine.getTape());
                        if (d.pendingCassette != juce::File())
                        {
                            auto next = std::exchange (d.pendingCassette, juce::File());
                            d.openCassetteFile (next);
                        }
                    });
}

void DeckComponent::insertBlank (int type, int model, const juce::String& grade)
{
    // A tape that couldn't be filled is refused at the start, not discovered halfway through a take.
    if (const auto needed = TapeShelf::bytesForFullTape (tapeTypes[type]); TapeShelf::isShort (needed))
    {
        say ("NOT ENOUGH DISK SPACE: A " + juce::String (tapeModels[model].name) + juce::String (tapeTypes[type].minutes * 2) + " NEEDS "
             + juce::String (juce::roundToInt ((double) needed / (1024.0 * 1024.0))) + " MB", 6.0);
        return;
    }
    switchTape ([type, model, grade]
                {
                    auto tape = TapeShelf::createBlank (type, model, grade);
                    TapeShelf::save (*tape);
                    return tape;
                },
                [safe = juce::Component::SafePointer<DeckComponent> (this)] (Tape& t)
                {
                    if (safe != nullptr)
                        safe->say ((t.getCondition().grade == "new" ? juce::String ("NEW BLANK ") : juce::String (t.getCondition().grade).toUpperCase() + " BLANK ")
                                   + t.productName());
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
                    if (d.engine.recallDeckWithTape.load() && loaded.session != nullptr)
                    {
                        auto state = std::make_unique<juce::XmlElement> (*loaded.session);
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
        {
            // The cassette's condition is chosen as it comes off the shelf: new, or one that has seen use.
            juce::PopupMenu grades;
            static const char* gradeText[] { "NEW", "USED", "WORN", "CHEWED" };
            for (int g = 0; g < 4; ++g)
            {
                auto grade = juce::String (tfmt::Condition::grades()[(size_t) g]);
                grades.addItem (gradeText[g], [this, i, k, grade] { insertBlank (i, k, grade); });
            }
            lengths.addSubMenu (juce::String (tm.name) + juce::String (tapeTypes[i].minutes * 2) + "   " + juce::String (tapeTypes[i].minutes) + " MIN PER SIDE",
                                grades);
        }
        m.addSubMenu (juce::String (tm.name) + utf ("   \xc2\xb7   ") + utf (tm.position) + utf ("   \xc2\xb7   ") + utf (tm.line), lengths);
    }

    if (auto* t = engine.getTape())
    {
        m.addSeparator();
        m.addSectionHeader ("THIS CASSETTE");
        m.addItem ("Export as a .cass file...", [this] { exportCassette(); });
        m.addItem ("Export its tracks (" + exportFormats.describe() + ")...", [this] { exportTracks(); });
        m.addSubMenu ("File formats for exports and mixdowns", formatsMenu());
        m.addItem ("Write-protect it (break out the tab)", true, t->protectA.load(), [this] { toggleWriteProtect(); });
    }

    m.addSeparator();
    m.addSectionHeader ("TAPE SHELF");
    m.addItem ("Open a .cass file...", [this] { chooseCassetteFile(); });
    auto* cur = engine.getTape();
    auto shelf = TapeShelf::list();
    if (shelf.empty())
        m.addItem ("No saved tapes yet", false, false, nullptr);
    for (auto& e : shelf)
    {
        if (e.layout == tfmt::Layout::stereoTwoSides) continue;   // master tapes live in the master deck
        auto text = (e.name.isNotEmpty() ? e.name : juce::String ("(no label)")) + utf ("   \xc2\xb7   ")
                    + e.formulation + juce::String (e.minutes * 2) + (e.grade.isNotEmpty() && e.grade != "new" ? " " + e.grade.toUpperCase() : juce::String())
                    + "  " + formatTime (e.usedSeconds) + " used  " + juce::String (e.tracksUsed) + "/4 trk  " + e.created;
        auto id = e.id;
        m.addItem (text, true, cur != nullptr && cur->id == id, [this, id] { loadFromShelf (id); });
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&eject).withMinimumWidth (360));
}

juce::PopupMenu DeckComponent::formatsMenu()
{
    juce::PopupMenu m;
    auto f = exportFormats;
    auto toggle = [this] (bool ExportFormats::* which)
    {
        auto g = exportFormats;
        g.*which = ! (g.*which);
        if (! g.any()) return;   // at least one
        setExportFormats (g);
    };
    m.addItem ("WAV", true, f.wav, [toggle] { toggle (&ExportFormats::wav); });
    m.addItem ("FLAC", true, f.flac, [toggle] { toggle (&ExportFormats::flac); });
    m.addItem ("MP3", true, f.mp3, [toggle] { toggle (&ExportFormats::mp3); });
    m.addItem ("All three", true, f.all(), [this] { auto g = exportFormats; g.wav = g.flac = g.mp3 = true; setExportFormats (g); });
    m.addSeparator();
    juce::PopupMenu wavBits, flacBits, mp3Rate;
    for (int b : { 16, 24, 32 })
        wavBits.addItem (b == 32 ? juce::String ("32-bit float") : juce::String (b) + "-bit", true, f.wavBits == b,
                         [this, b] { auto g = exportFormats; g.wavBits = b; setExportFormats (g); });
    for (int b : { 16, 24 })
        flacBits.addItem (juce::String (b) + "-bit", true, f.flacBits == b, [this, b] { auto g = exportFormats; g.flacBits = b; setExportFormats (g); });
    for (int k : { 192, 256, 320 })
        mp3Rate.addItem (juce::String (k) + " kbps", true, f.mp3Kbps == k, [this, k] { auto g = exportFormats; g.mp3Kbps = k; setExportFormats (g); });
    m.addSubMenu ("WAV: " + (f.wavBits == 32 ? juce::String ("32-bit float") : juce::String (f.wavBits) + "-bit"), wavBits);
    m.addSubMenu ("FLAC: " + juce::String (f.flacBits) + "-bit", flacBits);
    m.addSubMenu ("MP3: " + juce::String (f.mp3Kbps) + " kbps", mp3Rate);
    return m;
}

void DeckComponent::setExportFormats (const ExportFormats& f)
{
    exportFormats = f;
    props.setValue ("exportFormats", f.toXml().get());
    props.saveIfNeeded();
    say ("FILES: " + f.describe());
}

void DeckComponent::exportTracks()
{
    auto* t = engine.getTape();
    if (t == nullptr) return;
    if (t->tracksUsed() == 0) { say ("NOTHING RECORDED ON THIS TAPE YET"); return; }
    chooser = std::make_unique<juce::FileChooser> ("Export the tracks into a folder",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("TapeFour"));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe = juce::Component::SafePointer<DeckComponent> (this)] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        auto& d = *safe;
        auto* tape = d.engine.getTape();
        if (tape == nullptr || d.tapeBusy) return;
        if (d.engine.tapeEngine.getMode() != Mode::stop) { d.say ("STOP THE TAPE FIRST"); return; }

        auto job = std::make_shared<TrackExport>();
        job->folder = fc.getResult();
        job->baseName = tape->name.isNotEmpty() ? tape->name : tape->productName();
        job->formats = d.exportFormats;
        auto* device = d.engine.deviceManager.getCurrentAudioDevice();
        job->sampleRate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
        d.tapeBusy = true;   // the transport waits: the tape is being read
        d.busyText = "EXPORTING TRACKS...";
        d.say (d.busyText, 600.0);
        auto ok = std::make_shared<bool> (false);
        auto* audio = &d.engine;
        d.writer.enqueue ([job, ok, tape, audio, safe]
                          {
                              *ok = job->run (*tape, audio->tapeEngine.params, audio->tapeEngine.machine, [safe] (float p)
                              {
                                  juce::MessageManager::callAsync ([safe, p]
                                  {
                                      if (safe != nullptr && safe->tapeBusy)
                                          safe->say ("EXPORTING TRACKS " + juce::String (juce::roundToInt (p * 100.0f)) + "%", 600.0);
                                  });
                              });
                          },
                          [safe, job, ok]
                          {
                              if (safe == nullptr) return;
                              safe->tapeBusy = false;
                              safe->busyText = {};
                              safe->say (*ok ? "EXPORTED " + juce::String (job->written.size()) + " FILES TO " + job->folder.getFileName().toUpperCase()
                                             : "COULD NOT EXPORT: " + job->error.toUpperCase().substring (0, 40), 6.0);
                          });
    });
}

//==============================================================================
juce::File DeckComponent::mixdownFolder() const
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("TapeFour").getChildFile ("Mixdowns")
               .getChildFile (juce::File::createLegalFileName (mixdownName()));
}

juce::String DeckComponent::mixdownName() const
{
    auto* t = engine.getTape();
    if (t == nullptr) return "mixdown";
    return t->name.isNotEmpty() ? t->name : t->productName();
}

void DeckComponent::openMasterDeck()
{
    closeOverlay();
    auto card = std::make_unique<MasterDeckCard> (engine, exportFormats);
    card->onClose = [this] { closeOverlay(); };
    card->onSettingsChanged = [this] { masterSettingsChanged(); };
    card->onFormatsChanged = [this] (const ExportFormats& f) { setExportFormats (f); };
    card->onFlip = [this] { flipMasterTape(); };
    card->onNewMasterTape = [this] { newMasterTape(); };
    card->onExportCassette = [this] { exportMasterCassette(); };
    card->onShowFolder = [this]
    {
        auto f = mixdownFolder();
        f.createDirectory();
        f.revealToUser();
    };
    card->onSaveSide = [this] (int side) { saveMasterSide (side); };
    card->onListen = [this] (const MixTake& take)
    {
        if (engine.tapeEngine.getMode() != Mode::stop) { say ("STOP THE TAPE FIRST"); return; }
        if (! engine.masterDeck.listen (take)) say ("CAN'T LISTEN WHILE A MIX IS BEING TAKEN");
        else say ("LISTENING: MIX " + juce::String (take.number), 3.0);
    };
    card->onLoadMaster = [this] (const juce::String& id) { loadMasterTape (id); };
    showOverlay (std::move (card), juce::Rectangle<int> (MasterDeckCard::width, MasterDeckCard::height).withCentre ({ designWidth / 2, designHeight / 2 }));
}

void DeckComponent::masterSettingsChanged()
{
    auto& md = engine.masterDeck;
    const auto st = md.getState();
    const bool wasArmed = st == MasterDeck::State::armed;
    if (st != MasterDeck::State::off && ! wasArmed) return;   // not during a take
    if (wasArmed) md.disarm();

    // A blank master tape in the deck is simply swapped for one of the new kind.
    const auto& ms = md.settings;
    auto* t = md.getTape();
    const bool blank = t != nullptr && md.takes.empty() && t->tracksUsed() == 0;
    const bool differs = t != nullptr && (t->getModel() != ms.model || t->getTypeInfo().minutes != tapeTypes[ms.length].minutes
                                          || juce::String (t->getCondition().grade) != ms.grade);
    if (! ms.direct() && (t == nullptr || (blank && differs)))
        newMasterTape();
    md.applySettings (engine.tapeEngine.params.nrType.load());
    engine.saveSettings();
    if (wasArmed) armMixdown (false);
}

void DeckComponent::flipMasterTape()
{
    auto& md = engine.masterDeck;
    const auto st = md.getState();
    if (md.settings.direct()) return;
    if (st != MasterDeck::State::off && st != MasterDeck::State::armed) { say ("WAIT FOR THE MIX TO FINISH"); return; }
    const bool wasArmed = st == MasterDeck::State::armed;
    if (wasArmed) md.disarm();
    md.stopListening();
    md.settings.side = 1 - md.settings.side;
    engine.saveSettings();
    say (md.settings.side == 0 ? "MASTER TAPE: SIDE A" : "MASTER TAPE: SIDE B");
    if (wasArmed) armMixdown (false);
}

bool DeckComponent::armMixdown (bool announce)
{
    juce::String why;
    auto& md = engine.masterDeck;
    if (! md.arm (exportFormats, mixdownFolder(), mixdownName(), engine.tapeEngine.params.nrType.load(), why))
    {
        say (why, 4.0);
        return false;
    }
    engine.peakHoldL = 0.0f;
    engine.peakHoldR = 0.0f;
    if (announce) say ("MIXDOWN ARMED: PLAY STARTS MIX " + juce::String (md.currentTakeNumber()), 3.0);
    return true;
}

void DeckComponent::toggleMixdown()
{
    auto& md = engine.masterDeck;
    if (md.isArmedByUser())
    {
        md.disarm();
        if (md.getState() == MasterDeck::State::off) say ("MIXDOWN OFF");
        return;
    }
    if (masterBusy) { say ("THE MASTER TAPE IS STILL LOADING"); return; }
    armMixdown (true);
}

void DeckComponent::collectMixdown()
{
    auto& md = engine.masterDeck;
    MixTake take;
    juce::String problem;
    if (! md.collectFinished (take, problem)) return;
    if (auto* m = md.getTape(); m != nullptr && take.side >= 0) writer.saveTape (*m);
    if (problem.isNotEmpty())
        say ("MIX " + juce::String (take.number) + ": " + problem.toUpperCase().substring (0, 44), 6.0);
    else
        say ("SAVED MIX " + juce::String (take.number) + utf (" \xc2\xb7 ") + formatTime (take.length / cellRate) + utf (" \xc2\xb7 ") + take.formats, 5.0);
    if (take.side >= 0 && md.nextStart() < 0)
        say ("SAVED MIX " + juce::String (take.number) + juce::String (" - SIDE ") + (take.side == 0 ? "A" : "B") + " IS FULL", 6.0);
    // Still armed: the next pass is the next take.
    if (md.isArmedByUser())
        armMixdown (false);
}

void DeckComponent::putInMasterTape (std::function<std::unique_ptr<Tape>()> make, const juce::String& doneText)
{
    auto& md = engine.masterDeck;
    const auto st = md.getState();
    if (masterBusy) return;
    if (st != MasterDeck::State::off && st != MasterDeck::State::armed) { say ("WAIT FOR THE MIX TO FINISH"); return; }
    const bool wasArmed = st == MasterDeck::State::armed;
    if (wasArmed) md.disarm();
    md.stopListening();
    if (auto* old = md.getTape()) writer.saveTape (*old);
    masterBusy = true;
    auto made = std::make_shared<std::unique_ptr<Tape>>();
    writer.enqueue ([made, make] { *made = make(); },
                    [safe = juce::Component::SafePointer<DeckComponent> (this), made, doneText, wasArmed]
                    {
                        if (safe == nullptr) return;
                        auto& d = *safe;
                        d.masterBusy = false;
                        if (*made == nullptr) { d.say ("THAT MASTER TAPE WOULD NOT LOAD", 4.0); return; }
                        auto& deck = d.engine.masterDeck;
                        if (deck.getState() != MasterDeck::State::off) deck.disarm();
                        deck.settings.masterId = (*made)->id;
                        deck.settings.model = (*made)->getModel();
                        for (int i = 0; i < numTapeTypes; ++i)
                            if (tapeTypes[i].minutes == (*made)->getTypeInfo().minutes) deck.settings.length = i;
                        deck.settings.grade = juce::String ((*made)->getCondition().grade);
                        d.writer.retire (deck.swapTape (std::move (*made)));
                        deck.applySettings (d.engine.tapeEngine.params.nrType.load());
                        d.engine.saveSettings();
                        if (doneText.isNotEmpty()) d.say (doneText + deck.getTape()->productName(), 3.0);
                        if (wasArmed) d.armMixdown (false);
                    });
}

void DeckComponent::newMasterTape()
{
    const auto ms = engine.masterDeck.settings;
    if (ms.direct()) return;
    int count = 1;
    for (auto& e : TapeShelf::list()) if (e.layout == tfmt::Layout::stereoTwoSides) ++count;
    const auto model = juce::jlimit (0, numTapeModels - 1, ms.model), length = ms.length;
    const auto grade = ms.grade;
    putInMasterTape ([model, length, grade, count]
                     {
                         auto t = Tape::makeStereo (length, model, tfmt::Condition::forGrade (grade.toStdString()));
                         t->name = "master tape " + juce::String (count);
                         TapeShelf::save (*t);
                         return t;
                     },
                     "NEW MASTER TAPE: ");
}

void DeckComponent::loadMasterTape (const juce::String& id)
{
    if (auto* cur = engine.masterDeck.getTape(); cur != nullptr && cur->id == id) return;
    putInMasterTape ([id]
                     {
                         return Tape::loadForStereoDeck (TapeShelf::folderFor (id));
                     },
                     {});
}

void DeckComponent::exportMasterCassette()
{
    auto* m = engine.masterDeck.getTape();
    if (m == nullptr) { say ("NO MASTER TAPE"); return; }
    auto start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("TapeFour")
                     .getChildFile (juce::File::createLegalFileName (m->name.isNotEmpty() ? m->name : m->productName()) + ".cass");
    chooser = std::make_unique<juce::FileChooser> ("Export the master tape", start, "*.cass");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer<DeckComponent> (this)] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        auto& d = *safe;
        auto* tape = d.engine.masterDeck.getTape();
        if (tape == nullptr) return;
        auto target = fc.getResult().withFileExtension ("cass");
        d.writer.saveTape (*tape);
        auto id = tape->id;
        auto error = std::make_shared<juce::String>();
        auto ok = std::make_shared<bool> (false);
        d.writer.enqueue ([id, target, error, ok] { *ok = TapeShelf::exportCassette (id, target, *error); },
                          [safe, target, error, ok]
                          {
                              if (safe != nullptr)
                                  safe->say (*ok ? "EXPORTED " + target.getFileName().toUpperCase() : "COULD NOT EXPORT: " + error->toUpperCase().substring (0, 40), 5.0);
                          });
    });
}

void DeckComponent::saveMasterSide (int side)
{
    auto& md = engine.masterDeck;
    auto* m = md.getTape();
    if (m == nullptr) return;
    if (md.sideUsedSeconds (side) <= 0.0) { say (juce::String ("NOTHING ON SIDE ") + (side == 0 ? "A" : "B") + " YET"); return; }
    if (md.getState() != MasterDeck::State::off || md.isListening()) { say ("WAIT FOR THE MIX TO FINISH"); return; }
    auto job = std::make_shared<SideExport>();
    job->base = mixdownFolder().getChildFile (juce::File::createLegalFileName ((m->name.isNotEmpty() ? m->name : m->productName())
                                                                               + " - side " + (side == 0 ? "A" : "B")));
    job->formats = exportFormats;
    auto* device = engine.deviceManager.getCurrentAudioDevice();
    job->sampleRate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
    job->side = side;
    mixdownFolder().createDirectory();
    masterBusy = true;
    say ("SAVING SIDE " + juce::String (side == 0 ? "A" : "B") + "...", 600.0);
    auto ok = std::make_shared<bool> (false);
    auto machine = md.getMachine();
    const bool nr = md.settings.nr;
    const int nrSystem = engine.tapeEngine.params.nrType.load();
    writer.enqueue ([job, ok, m, machine, nr, nrSystem] { *ok = job->run (*m, machine, nr, nrSystem); },
                    [safe = juce::Component::SafePointer<DeckComponent> (this), job, ok, side]
                    {
                        if (safe == nullptr) return;
                        safe->masterBusy = false;
                        safe->say (*ok ? juce::String ("SAVED SIDE ") + (side == 0 ? "A" : "B") + ": " + job->formats.describe()
                                       : "COULD NOT SAVE THE SIDE: " + job->error.toUpperCase().substring (0, 36), 5.0);
                    });
}

void DeckComponent::toggleWriteProtect()
{
    auto* t = engine.getTape();
    if (t == nullptr) return;
    const bool on = ! t->protectA.load();
    t->protectA = on;
    if (t->getLayout() == tfmt::Layout::stereoTwoSides) t->protectB = on;
    t->markMetaChanged();
    saveTape (false, false);
    say (on ? "TAB BROKEN OUT: THIS TAPE CAN'T BE RECORDED ON" : "TAB TAPED OVER: THIS TAPE CAN BE RECORDED ON", 4.0);
}

void DeckComponent::exportCassette()
{
    auto* t = engine.getTape();
    if (t == nullptr) return;
    auto fileName = juce::File::createLegalFileName (t->name.isNotEmpty() ? t->name : t->productName());
    auto start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("TapeFour").getChildFile (fileName + ".cass");
    chooser = std::make_unique<juce::FileChooser> ("Export the cassette", start, "*.cass");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, safe = juce::Component::SafePointer<DeckComponent> (this)] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        auto target = fc.getResult();
        auto* tape = engine.getTape();
        if (target == juce::File() || tape == nullptr) return;
        target = target.withFileExtension ("cass");
        // What's on the shelf is what's exported: save first (the writer runs jobs in order).
        saveTape (false);
        auto id = tape->id;
        auto error = std::make_shared<juce::String>();
        auto ok = std::make_shared<bool> (false);
        writer.enqueue ([id, target, error, ok] { *ok = TapeShelf::exportCassette (id, target, *error); },
                        [safe, target, error, ok]
                        {
                            if (safe == nullptr) return;
                            safe->say (*ok ? "EXPORTED " + target.getFileName().toUpperCase() : "COULD NOT EXPORT: " + error->toUpperCase().substring (0, 40), 5.0);
                        });
    });
}

void DeckComponent::chooseCassetteFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Open a cassette", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.cass");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<DeckComponent> (this)] (const juce::FileChooser& fc)
    {
        if (safe != nullptr && fc.getResult().existsAsFile())
            safe->openCassetteFile (fc.getResult());
    });
}

void DeckComponent::openCassetteFile (const juce::File& cassFile)
{
    if (tapeBusy)
    {
        pendingCassette = cassFile;   // opened as soon as the tape that's loading is in
        return;
    }
    if (engine.tapeEngine.getMode() != Mode::stop)
    {
        say ("STOP THE TAPE FIRST");
        return;
    }
    tfmt::CassetteInfo info;
    juce::String error;
    if (! TapeShelf::peek (cassFile, info, error))
    {
        say ("NOT A CASSETTE: " + error.toUpperCase().substring (0, 40), 5.0);
        return;
    }
    // The tape in the deck is put away first, so a copy of it on the shelf can be replaced.
    auto id = juce::String::fromUTF8 (info.id.c_str());
    auto label = juce::String::fromUTF8 (info.label.c_str());
    if (auto* cur = engine.getTape(); cur != nullptr && cur->id == id)
    {
        say ("THAT CASSETTE IS ALREADY IN THE DECK", 4.0);
        return;
    }
    switchTape ([cassFile]
                {
                    juce::String importedId, err;
                    if (! TapeShelf::importCassette (cassFile, importedId, err))
                        return std::unique_ptr<Tape>();
                    return TapeShelf::load (importedId);
                },
                [safe = juce::Component::SafePointer<DeckComponent> (this), label] (Tape& loaded)
                {
                    if (safe != nullptr)
                        safe->say ("OPENED: " + (label.isNotEmpty() ? label : loaded.productName()).toUpperCase(), 4.0);
                },
                "OPENING CASSETTE...");
}

//==============================================================================
void DeckComponent::closeBlockWindow (int t, int id)
{
    auto it = pluginWindows.find ({ t, id });
    if (it == pluginWindows.end()) return;
    pluginWindows.erase (it);
    if (auto* b = engine.findBlock (t, id))
    {
        b->setEditorOpen (false);
        b->captureState();   // a natural moment: the sound as it was left
        engine.saveSettings();
    }
}

void DeckComponent::openBlockWindow (int t, int id)
{
    auto* b = engine.findBlock (t, id);
    if (b == nullptr || ! b->hasEditor() || b->getProcessor() == nullptr) return;
    if (auto it = pluginWindows.find ({ t, id }); it != pluginWindows.end()) { it->second->toFront (true); return; }
    b->setEditorOpen (true);
    auto w = std::make_unique<PluginWindow> (*b->getProcessor(), "TRACK " + juce::String (t + 1) + utf (" \xc2\xb7 ") + b->getName(), [this, t, id]
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<DeckComponent> (this), t, id]
        {
            if (safe != nullptr) safe->closeBlockWindow (t, id);
        });
    });
    w->onDeckKey = [this] (const juce::KeyPress& k)
    {
        // Space, R and Home drive the transport from the plug-in's window too.
        if (k == juce::KeyPress::spaceKey || k == juce::KeyPress::homeKey
            || k.getTextCharacter() == 'r' || k.getTextCharacter() == 'R')
            return keyPressed (k);
        return false;
    };
    pluginWindows[{ t, id }] = std::move (w);
}

void DeckComponent::editSound (int t)
{
    for (auto& b : engine.getChain (t))
        if (b.hasEditor && ! b.bypass)
        {
            openBlockWindow (t, b.id);
            return;
        }
    openChain (t);
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
    if (chainTrack >= 0) strips[chainTrack]->setChainOpen (false);
    chainTrack = -1;
    overlay->setVisible (false);
    // Don't delete the card while one of its buttons is still on the call stack.
    juce::MessageManager::callAsync ([old = std::shared_ptr<Overlay> (overlay.release())] {});
    engine.saveSettings();
    grabKeyboardFocus();
    repaint();
}

void DeckComponent::openChain (int t)
{
    closeOverlay();
    auto* strip = strips[t];
    auto display = strip->getDisplayBounds().translated (strip->getX(), strip->getY());
    const int cardX = 20, cardY = display.getBottom() + 4;
    auto card = std::make_unique<ChainEditor> (engine, t, display.getCentreX() - cardX);
    card->onClose = [this] { closeOverlay(); };
    card->onEditBlock = [this] (int track, int id) { openBlockWindow (track, id); };
    card->onFindPlugins = [this] { closeOverlay(); openSetup (instrumentsTab); };
    card->onMessage = [this] (const juce::String& m) { say (m, 4.0); };
    auto* editor = card.get();
    showOverlay (std::move (card), { cardX, cardY, ChainEditor::editorWidth, ChainEditor::editorHeight + ChainEditor::notchHeight });
    overlay->cardShadow = true;
    overlay->dimming = 0.35f;
    // The second click of a double-click on the display: the first block's window instead.
    overlay->onClickOutside = [this, t, display, opened = juce::Time::getMillisecondCounter()] (juce::Point<int> pos)
    {
        if (! display.contains (pos) || juce::Time::getMillisecondCounter() - opened > (juce::uint32) juce::MouseEvent::getDoubleClickTimeout())
            return false;
        closeOverlay();
        editSound (t);
        return true;
    };
    chainTrack = t;
    strip->setChainOpen (true);
    strip->toFront (false);   // the track's display stays lit above the dimming
    overlay->toFront (false);
    editor->grabKeyboardFocus();
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
    pitch.slider.setValue (engine.tapeEngine.params.pitch.load(), juce::dontSendNotification);
    pitch.repaint();
    syncSpeed();
    repaint();
}

void DeckComponent::openSetup (int tab)
{
    if (setupWindow == nullptr)
        setupWindow = std::make_unique<SetupWindow> (engine, props);
    setupWindow->showTab (tab);
}


void DeckComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.eventComponent != this)
    {
        // a right-click on a control that MIDI learn can map
        if (e.mods.isPopupMenu() && learnLayer == nullptr)
            if (auto* lt = learnTargetFor (e.eventComponent))
                learnTargetMenu (*lt);
        return;
    }
    grabKeyboardFocus();
}

bool DeckComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && learnLayer != nullptr) { setLearnMode (false); return true; }
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
