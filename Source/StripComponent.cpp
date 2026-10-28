#include "StripComponent.h"

namespace tf
{
namespace
{
    juce::String fmtDb (double db, int decimals = 0)
    {
        auto scale = std::pow (10.0, decimals);
        auto v = std::round (db * scale) / scale;
        if (std::abs (v) < 0.5 / scale) v = 0.0;
        auto text = decimals == 0 ? juce::String (juce::roundToInt (v)) : juce::String (v, decimals);
        return (v > 0.0 ? "+" : "") + text;
    }

    void setupBipolar (juce::Slider& s)
    {
        s.setRange (-1.0, 1.0);
        s.setDoubleClickReturnValue (true, 0.0);
    }

    /** MIDI learn may have moved a control: show it, unless someone has hold of it. */
    void follow (juce::Slider& s, double v)
    {
        if (! s.isMouseButtonDown() && std::abs (s.getValue() - v) > 1.0e-6)
            s.setValue (v, juce::dontSendNotification);
    }
}

StripComponent::StripComponent (AudioEngine& e, int index)
    : engine (e), t (index),
      gain ("GAIN", false, [] (double v) { return fmtDb (-20.0 + 60.0 * v) + " dB"; }),
      high ("HIGH", true,  [] (double v) { return fmtDb (v * 12.0, 1); }),
      mid  ("MID",  true,  [] (double v) { return fmtDb (v * 12.0, 1); }),
      freq ("FREQ", true,  [] (double v)
             {
                 auto hz = AudioEngine::midFreqHz ((float) v);
                 return hz >= 1000.0 ? juce::String (hz / 1000.0, hz >= 9950.0 ? 0 : 1) + "k" : juce::String (juce::roundToInt (hz / 10.0) * 10);
             }),
      low  ("LOW",  true,  [] (double v) { return fmtDb (v * 12.0, 1); }),
      pan  ("PAN",  false, [] (double v)
             {
                 auto p = juce::roundToInt (v * 100.0);
                 return p == 0 ? juce::String ("C") : (p < 0 ? "L" + juce::String (-p) : "R" + juce::String (p));
             })
{
    auto& p = engine.track[t];

    gain.slider.setRange (0.0, 1.0);
    gain.slider.setDoubleClickReturnValue (true, 1.0 / 3.0);
    for (auto* k : { &high, &mid, &low, &pan })
        setupBipolar (k->slider);
    freq.slider.setRange (0.0, 1.0);
    freq.slider.setDoubleClickReturnValue (true, AudioEngine::defaultMidFreq);
    freq.setSecondary (36);
    freq.slider.setTooltip ("Mid frequency: 250 Hz to 5 kHz");

    gain.slider.onValueChange = [this, &p] { p.gain = (float) gain.slider.getValue(); gain.repaint(); };
    high.slider.onValueChange = [this, &p] { p.high = (float) high.slider.getValue(); high.repaint(); };
    mid.slider.onValueChange  = [this, &p] { p.mid  = (float) mid.slider.getValue();  mid.repaint(); };
    freq.slider.onValueChange = [this, &p] { p.midFreq = (float) freq.slider.getValue(); freq.repaint(); };
    low.slider.onValueChange  = [this, &p] { p.low  = (float) low.slider.getValue();  low.repaint(); };
    pan.slider.onValueChange  = [this, &p] { p.pan  = (float) pan.slider.getValue();  pan.repaint(); };
    fader.slider.onValueChange = [this, &p] { p.fader = (float) fader.slider.getValue(); };

    lcd.chainRow = true;
    lcd.topSize = 15.0f;
    // Click: the chain, straight away. A double-click's second click lands on the chain's overlay,
    // which turns it into "open the first block's window" (DeckComponent::openChain).
    lcd.onClick = [this] { if (onOpenChain) onOpenChain (t); };
    lcd.setTooltip ("Click for this track's inputs and chain. Double-click to open its first plug-in's window.");

    mute.onClick = [&p] { p.mute = ! p.mute.load(); };
    solo.onClick = [&p] { p.solo = ! p.solo.load(); };
    arm.onClick  = [&p] { p.armed = ! p.armed.load(); };
    mute.setTooltip ("Mute: silences the track (it still records when armed)");
    solo.setTooltip ("Solo: hear only soloed tracks. Changes what you hear, not what's recorded or bounced");
    arm.setTooltip ("Arm: hear this track's live input; it records when you press REC");

    for (auto* c : std::initializer_list<juce::Component*> { &lcd, &gain, &high, &mid, &freq, &low, &pan, &meter, &fader, &mute, &solo, &arm })
        addAndMakeVisible (c);

    addMouseListener (this, true);
    syncFromEngine();
}

StripComponent::~StripComponent()
{
    removeMouseListener (this);
}

void StripComponent::syncFromEngine()
{
    auto& p = engine.track[t];
    gain.slider.setValue (p.gain.load(), juce::dontSendNotification);
    high.slider.setValue (p.high.load(), juce::dontSendNotification);
    mid.slider.setValue (p.mid.load(), juce::dontSendNotification);
    freq.slider.setValue (p.midFreq.load(), juce::dontSendNotification);
    low.slider.setValue (p.low.load(), juce::dontSendNotification);
    pan.slider.setValue (p.pan.load(), juce::dontSendNotification);
    fader.slider.setValue (p.fader.load(), juce::dontSendNotification);
    repaint();
}

void StripComponent::forEachLearnTarget (const std::function<void (juce::Component&, MidiMapper::Control)>& fn)
{
    using C = MidiMapper::Control;
    fn (gain.slider, C::gain);
    fn (high.slider, C::high);
    fn (mid.slider, C::mid);
    fn (freq.slider, C::freq);
    fn (low.slider, C::low);
    fn (pan.slider, C::pan);
    fn (fader.slider, C::fader);
    fn (mute, C::mute);
    fn (solo, C::solo);
    fn (arm, C::arm);
}

void StripComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && e.originalComponent == &lcd && onOpenChain)
        onOpenChain (t);
}

void StripComponent::rebuildText()
{
    auto& p = engine.track[t];
    const bool bounce = (AudioEngine::Feed) p.feed.load() == AudioEngine::Feed::bounce;
    const auto chain = engine.getChain (t);

    lcd.top = engine.describeSource (t);
    auto port = p.midiPort.load();
    bool chainPlays = false;   // anything in the chain that listens to a controller
    for (auto& b : chain)
        chainPlays = chainPlays || b.role == BlockRole::instrument || b.role == BlockRole::midiFx || b.role == BlockRole::external;
    const bool hasMidi = ! bounce && port != AudioEngine::midiNone && (chainPlays || port != AudioEngine::midiAny);
    const bool hasAudio = ! bounce && p.inputChannel.load() >= 0;
    juce::StringArray parts;
    if (bounce)  parts.add (utf ("TAPE \xe2\x86\x92 TRK ") + juce::String (t + 1));
    if (hasMidi) parts.add ("MIDI: " + engine.midiPortLabel (port) + (engine.isMidiPortMpe (port) ? " MPE" : ""));
    if (hasAudio)
    {
        auto names = engine.getActiveInputNames();
        auto ch = p.inputChannel.load();
        parts.add (juce::isPositiveAndBelow (ch, names.size()) ? engine.describeInput (t) : juce::String ("INPUT NOT ACTIVE"));
    }
    lcd.sub = parts.isEmpty() ? juce::String ("NO INPUTS") : parts.joinIntoString (utf (" \xc2\xb7 "));

    lcd.blocks = engine.blockAbbreviations (t);
    lcd.blockStates.clearQuick();
    for (auto& b : chain)
        lcd.blockStates.add (b.role == BlockRole::pending ? 2 : (b.bypass ? 1 : 0));
    lcd.roomForMore = (int) chain.size() < AudioEngine::maxBlocks;

    if (bounce)                                      { tag = "BOUNCE";     tagColour = Pal::bounce; }
    else if (hasAudio && hasMidi && ! chain.empty()) { tag = "AUDIO+MIDI"; tagColour = Pal::accent; }
    else if (hasAudio)                               { tag = "AUDIO";      tagColour = Pal::accent; }
    else if (! chain.empty())                        { tag = "MIDI";       tagColour = Pal::midiBlue; }
    else                                             { tag = {};           tagColour = {}; }
}

void StripComponent::refresh (bool blink)
{
    auto& p = engine.track[t];
    auto* tape = engine.getTape();
    auto mode = engine.tapeEngine.getMode();

    // ---- the display: what plays, what feeds it, the chain (rebuilt only when something changed,
    //      or once a second in case an input or device name moved without saying) ----
    const auto version = engine.viewVersion.load();
    if (version != shownVersion || ++framesSinceText >= 30)
    {
        shownVersion = version;
        framesSinceText = 0;
        rebuildText();
    }

    // Armed means live: say so, so nobody waits to hear the tape on an armed track.
    lcd.low = p.armed.load() && mode != Mode::rec ? juce::String ("LIVE IN")
              : p.mute.load() ? juce::String ("MUTED") : utf ("\xe2\x86\x92 TAPE");

    // What's on the tape under the head: noise reduction and recorded speed.
    lcd.nrLamp = false;
    lcd.nrColour = Pal::lamp;
    lcd.eqLamp = false;
    lcd.eqColour = Pal::green;
    if (tape != nullptr)
    {
        auto block = tape->blockAt (tape->position.load());
        bool recordingHere = mode == Mode::rec && p.armed.load();
        if (recordingHere)
        {
            lcd.nrLamp = engine.tapeEngine.params.nrOn.load();
            lcd.eqLamp = true;
        }
        else if (tape->isRecorded (t, block))
        {
            bool enc = tape->isNrEncoded (t, block);
            bool listening = mode == Mode::play || mode == Mode::rec;
            if (listening && enc != engine.tapeEngine.params.nrOn.load())
            {
                lcd.nrLamp = blink;
                lcd.nrColour = Pal::red;
            }
            else
            {
                lcd.nrLamp = enc;
            }

            // EQ: green when this stretch was recorded with the switch's playback EQ, red when not
            auto eqOf = [] (int pos) { return pos == 0 ? 120 : 70; };
            if (eqOf (tape->recordedPosition (t, block)) == eqOf (engine.tapeEngine.params.tapeSwitch.load()))
                lcd.eqLamp = true;
            else
            {
                lcd.eqLamp = listening ? blink : true;
                lcd.eqColour = Pal::red;
            }

            if (listening)
            {
                auto rs = tape->recordedSpeed (t, block);
                if (rs > 0.0f)
                {
                    auto semis = 12.0 * std::log2 (engine.tapeEngine.getNominalSpeed() / rs);
                    if (std::abs (semis) >= 0.1)
                        lcd.low = (semis > 0 ? "+" : "") + juce::String (semis, 1) + " SEMITONES";
                }
            }
        }
    }

    if (p.midiActivity.exchange (false)) midiHold = 4;
    lcd.midiLamp = midiHold > 0;
    if (midiHold > 0) --midiHold;
    lcd.repaintIfChanged();

    meter.setLevels (p.meter.exchange (0.0f));

    // ---- mute, solo, arm: the LEDs light; arm flashes until it records ----
    auto led = [] (LedKey& k, bool on) { if (k.ledOn != on) { k.ledOn = on; k.repaint(); } };
    led (mute, p.mute.load());
    led (solo, p.solo.load());
    led (arm, p.armed.load() && (mode == Mode::rec || blink));

    // ---- controls MIDI learn may have moved ----
    follow (gain.slider, p.gain.load());
    follow (high.slider, p.high.load());
    follow (mid.slider, p.mid.load());
    follow (freq.slider, p.midFreq.load());
    follow (low.slider, p.low.load());
    follow (pan.slider, p.pan.load());
    follow (fader.slider, p.fader.load());

    if (tag != shownTag || tagColour != shownTagColour)
    {
        shownTag = tag;
        shownTagColour = tagColour;
        repaint (0, 0, getWidth(), 50);
    }
}

void StripComponent::resized()
{
    auto r = getLocalBounds().reduced (10);
    r.removeFromTop (30 + 6);
    lcd.setBounds (r.removeFromTop (100));
    r.removeFromTop (8);
    gain.setBounds (r.removeFromTop (44));
    r.removeFromTop (16);
    auto eq = r.removeFromTop (200).reduced (0, 12);
    high.setBounds (eq.removeFromTop (44)); eq.removeFromTop (2);
    mid.setBounds (eq.removeFromTop (44));  eq.removeFromTop (0);
    freq.setBounds (eq.removeFromTop (38)); eq.removeFromTop (2);
    low.setBounds (eq.removeFromTop (44));
    r.removeFromTop (6);
    pan.setBounds (r.removeFromTop (44));
    r.removeFromTop (10);
    r.removeFromBottom (2);

    // meter | fader | mute, solo and arm, bottom-aligned with room between them
    auto keys = r.removeFromRight (46);
    auto k = keys.removeFromBottom (3 * 40 + 2 * 14);
    for (auto* b : { &mute, &solo, &arm })
    {
        b->setBounds (k.removeFromTop (40));
        k.removeFromTop (14);
    }
    auto area = r.withSizeKeepingCentre (24 + 10 + 96, r.getHeight());
    meter.setBounds (area.removeFromLeft (24));
    area.removeFromLeft (10);
    fader.setBounds (area);
}

void StripComponent::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    auto r = getLocalBounds().toFloat();
    g.setColour (Pal::darker);
    g.fillRoundedRectangle (r.translated (0, 2), 10.0f);
    g.setColour (Pal::panel);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (Pal::panelHi);
    g.drawLine (10, 1, r.getWidth() - 10, 1, 1.0f);

    auto head = getLocalBounds().reduced (12, 10).removeFromTop (30);
    g.setColour (Pal::cream);
    g.setFont (lf.cond (36.0f, true));
    g.drawText (juce::String (t + 1), head.removeFromLeft (22), juce::Justification::centredLeft);
    g.setColour (Pal::muted);
    g.setFont (lf.cond (13.0f).withExtraKerningFactor (0.2f));
    g.drawText ("TRACK", head.removeFromLeft (60).withTrimmedTop (9), juce::Justification::centredLeft);

    if (tag.isNotEmpty())
    {
        g.setFont (lf.cond (13.0f, true).withExtraKerningFactor (0.14f));
        auto w = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), tag) + 18;
        auto badge = head.removeFromRight (w).withSizeKeepingCentre (w, 24).toFloat();
        g.setColour (tagColour);
        g.fillRoundedRectangle (badge, 3.0f);
        g.setColour (juce::Colour (0xff1a1917));
        g.drawText (tag, badge.toNearestInt(), juce::Justification::centred);
    }

    // EQ box
    auto eqBox = high.getBounds().getUnion (low.getBounds()).expanded (0, 10).toFloat();
    eqBox = eqBox.withX (10.0f).withWidth ((float) getWidth() - 20.0f);
    g.setColour (Pal::line);
    g.drawRoundedRectangle (eqBox, 8.0f, 1.0f);
    g.setFont (lf.cond (14.0f, true).withExtraKerningFactor (0.24f));
    auto lbl = juce::Rectangle<float> (40, 16).withCentre ({ eqBox.getCentreX(), eqBox.getY() });
    g.setColour (Pal::panel);
    g.fillRect (lbl);
    g.setColour (Pal::accent);
    g.drawText ("EQ", lbl, juce::Justification::centred);
}

//==============================================================================
MasterStrip::MasterStrip (AudioEngine& e)
    : engine (e),
      phones ("PHONES", false, [] (double v) { return juce::String (juce::roundToInt (v * 10.0)); })
{
    phones.stacked = true;
    phones.knobSize = 40;
    phones.slider.setRange (0.0, 1.0);
    phones.slider.setDoubleClickReturnValue (true, 0.6);
    phones.slider.onValueChange = [this] { engine.phones = (float) phones.slider.getValue(); phones.repaint(); };
    fader.slider.onValueChange = [this] { engine.masterFader = (float) fader.slider.getValue(); };
    display.topSize = 13.0f;
    display.onClick = [this] { if (onMasterDeck) onMasterDeck(); };
    display.setTooltip ("The master deck: the stereo cassette (or none: DIRECT) a mixdown goes to, and the files it writes");
    side.textSize = 11.0f;
    side.onClick = [this] { if (onSide) onSide(); };
    side.setTooltip ("Turn the master cassette over");
    mixdown.textSize = 11.0f;
    mixdown.onClick = [this] { if (onMixdown) onMixdown(); };
    mixdown.setTooltip ("Arm a mixdown: PLAY starts a take, STOP ends it. Each take is a new file.");
    for (auto* c : std::initializer_list<juce::Component*> { &phones, &display, &meter, &fader, &side, &mixdown })
        addAndMakeVisible (c);
    syncFromEngine();
}

void MasterStrip::syncFromEngine()
{
    phones.slider.setValue (engine.phones.load(), juce::dontSendNotification);
    fader.slider.setValue (engine.masterFader.load(), juce::dontSendNotification);
}

void MasterStrip::refresh (const juce::String& formats, bool blink)
{
    meter.setLevels (engine.meterL.exchange (0.0f), engine.meterR.exchange (0.0f));
    follow (fader.slider, engine.masterFader.load());
    follow (phones.slider, engine.phones.load());

    auto& md = engine.masterDeck;
    const auto& ms = md.settings;
    const auto sideName = ms.side == 0 ? "A" : "B";
    if (ms.direct())
    {
        display.top = "MASTER: DIRECT";
        display.sub = formats + utf (" \xe2\x80\xba");
    }
    else
    {
        display.top = "MASTER: " + md.describeTape().replace (utf (" \xc2\xb7 "), " ");
        display.sub = juce::String (sideName) + " " + formatTime (md.sideUsedSeconds (ms.side)) + " " + formats + utf (" \xe2\x80\xba");
    }
    display.repaintIfChanged();

    side.setButtonText (juce::String ("SIDE ") + sideName);
    side.setLed (! ms.direct(), Pal::accent);
    side.setEnabled (! ms.direct());
    const auto st = md.getState();
    const bool taking = st == MasterDeck::State::recording || st == MasterDeck::State::tailing || st == MasterDeck::State::finishing;
    mixdown.setLed (st == MasterDeck::State::armed || (taking && blink) || (md.isArmedByUser() && st == MasterDeck::State::done));

    auto peakDb = [] (float v) { return v < 0.0001f ? juce::String ("-INF") : juce::String (juce::Decibels::gainToDecibels (v), 1); };
    auto text = peakDb (engine.peakHoldL.load()) + "  " + peakDb (engine.peakHoldR.load());
    if (text != peakText) { peakText = text; repaint (peakArea); }
}

void MasterStrip::mouseDown (const juce::MouseEvent& e)
{
    // A click on the peak readout clears it.
    if (peakArea.contains (e.getEventRelativeTo (this).getPosition()))
    {
        engine.peakHoldL = 0.0f;
        engine.peakHoldR = 0.0f;
    }
}

void MasterStrip::forEachLearnTarget (const std::function<void (juce::Component&, MidiMapper::Control)>& fn)
{
    fn (fader.slider, MidiMapper::Control::masterFader);
    fn (phones.slider, MidiMapper::Control::phones);
}

void MasterStrip::resized()
{
    auto r = getLocalBounds().reduced (12);
    r.removeFromTop (52);
    phones.setBounds (r.removeFromTop (44));
    r.removeFromTop (8);
    display.setBounds (r.removeFromTop (42));
    r.removeFromTop (4);
    peakArea = r.removeFromTop (18);
    r.removeFromTop (6);
    // The master's keys sit at the foot, level with the tracks' ARM row.
    mixdown.setBounds (r.removeFromBottom (36));
    r.removeFromBottom (12);
    side.setBounds (r.removeFromBottom (32));
    r.removeFromBottom (26);   // L · R
    auto area = r.withSizeKeepingCentre (32 + 8 + 86, r.getHeight());
    meter.setBounds (area.removeFromLeft (32));
    area.removeFromLeft (8);
    fader.setBounds (area);
}

void MasterStrip::paint (juce::Graphics& g)
{
    auto& lf = DeckLookAndFeel::get();
    auto r = getLocalBounds().toFloat();
    g.setColour (Pal::darker);
    g.fillRoundedRectangle (r.translated (0, 2), 10.0f);
    g.setColour (Pal::master);
    g.fillRoundedRectangle (r, 10.0f);

    auto head = getLocalBounds().reduced (12, 10).removeFromTop (48);
    g.setColour (Pal::cream);
    g.setFont (lf.cond (26.0f, true).withExtraKerningFactor (0.1f));
    g.drawText ("MASTER", head.removeFromTop (28), juce::Justification::centredLeft);
    g.setColour (Pal::muted);
    g.setFont (lf.cond (12.0f).withExtraKerningFactor (0.2f));
    g.drawText ("STEREO OUT", head, juce::Justification::centredLeft);

    g.setFont (lf.cond (14.0f).withExtraKerningFactor (0.2f));
    g.drawText (utf ("L \xc2\xb7 R"), juce::Rectangle<int> (meter.getX() - 6, meter.getBottom() + 2, meter.getWidth() + 12, 22),
                juce::Justification::centred);

    // The peaks since the last click: what a mixdown would print.
    auto p = peakArea;
    g.setColour (Pal::lcd);
    g.fillRoundedRectangle (p.removeFromLeft (88).toFloat(), 3.0f);
    g.setColour (Pal::lcdText);
    g.setFont (lf.mono (11.0f));
    g.drawText (peakText, peakArea.withWidth (88), juce::Justification::centred);
    g.setColour (Pal::lcdText.withAlpha (0.85f));
    g.setFont (lf.cond (11.0f).withExtraKerningFactor (0.15f));
    g.drawText ("PEAK", p.withTrimmedLeft (6), juce::Justification::centredLeft);
}
} // namespace tf
