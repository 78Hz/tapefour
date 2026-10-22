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

    lcd.showLamps = true;
    // Click: the patch card. Double-click: the instrument's own window.
    lcd.onClick = [this]
    {
        if (clickPending)
        {
            clickPending = false;
            if (onEditSound) onEditSound (t);
            return;
        }
        clickPending = true;
        juce::Component::SafePointer<StripComponent> safe (this);
        juce::Timer::callAfterDelay (juce::MouseEvent::getDoubleClickTimeout(), [safe]
        {
            if (safe == nullptr || ! safe->clickPending) return;
            safe->clickPending = false;
            if (safe->onSourceMenu) safe->onSourceMenu (safe->t, &safe->lcd);
        });
    };
    lcd.setTooltip ("Click to patch this track. Double-click to open its instrument's window.");

    arm.lamp = true;
    arm.lampColour = Pal::red;
    arm.setClickingTogglesState (true);
    arm.onClick = [this, &p] { p.armed = arm.getToggleState(); };

    for (auto* c : std::initializer_list<juce::Component*> { &lcd, &gain, &high, &mid, &freq, &low, &pan, &meter, &fader, &arm })
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
    arm.setToggleState (p.armed.load(), juce::dontSendNotification);
    repaint();
}

void StripComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && dynamic_cast<juce::Slider*> (e.originalComponent) == nullptr && onSourceMenu)
        onSourceMenu (t, this);
}

TrackLines describeTrack (const AudioEngine& engine, int t)
{
    auto& p = engine.track[t];
    TrackLines d;
    d.top = engine.describeSource (t);
    const auto arrow = utf (" \xe2\x86\x92 ");
    switch ((Source) p.source.load())
    {
        case Source::audio:
        {
            auto names = engine.getActiveInputNames();
            auto ch = p.inputChannel.load();
            d.mid = juce::isPositiveAndBelow (ch, names.size()) ? names[ch].toUpperCase() : juce::String ("INPUT NOT ACTIVE");
            if (p.inputPair.load() && juce::isPositiveAndBelow (ch + 1, names.size()))
                d.mid << " + " << names[ch + 1].toUpperCase();
            d.low = "NO MIDI";
            d.tag = "AUDIO"; d.tagColour = Pal::orange;
            break;
        }
        case Source::instrument:
        {
            auto port = p.midiPort.load();
            d.mid = "MIDI IN: " + engine.midiPortLabel (port) + (engine.isMidiPortMpe (port) ? juce::String (" MPE") : juce::String());
            d.low = "NO AUDIO IN";
            auto* inst = engine.getInstrument (t);
            d.tag = inst != nullptr && inst->getPlugin() == nullptr ? "INST" : "VST3";
            d.tagColour = Pal::midiBlue;
            break;
        }
        case Source::hardware:
        {
            auto port = p.midiPort.load();
            auto ch = p.midiOutChannel.load();
            auto outValid = juce::isPositiveAndBelow (p.midiOutPort.load(), AudioEngine::maxMidiPorts);
            d.mid = engine.midiPortLabel (port) + arrow
                    + (! outValid ? juce::String ("NO MIDI OUT")
                       : engine.isMidiPortMpe (port) || ch == 0 ? juce::String ("ALL CH") : "CH " + juce::String (ch));
            d.low = utf ("AUDIO \xe2\x86\x90 ") + engine.describeInput (t);
            if (! outValid) d.top = "HARDWARE SYNTH";
            d.tag = "HW SYNTH"; d.tagColour = Pal::green;
            break;
        }
        case Source::bounce:
            d.mid = "TAPE" + arrow + "TRK " + juce::String (t + 1);
            d.low = "NO MIDI";
            d.tag = "BOUNCE"; d.tagColour = Pal::bounce;
            break;
        case Source::none: default:
            d.mid = "RIGHT-CLICK TO PATCH";
            break;
    }
    return d;
}

void StripComponent::refresh (bool blink)
{
    auto& p = engine.track[t];
    auto* tape = engine.getTape();
    auto mode = engine.tapeEngine.getMode();

    auto d = describeTrack (engine, t);
    lcd.top = d.top;
    lcd.sub = d.mid;
    // Armed means live: say so, so nobody waits to hear the tape on an armed track.
    lcd.low = p.armed.load() && mode != Mode::rec && (Source) p.source.load() != Source::none ? juce::String ("ARMED: LIVE INPUT") : d.low;
    tag = d.tag;
    tagColour = d.tagColour;

    // What's on the tape under the head: noise reduction and recorded speed.
    lcd.nrLamp = false;
    lcd.nrColour = Pal::amber;
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
            auto eqOf = [] (int p) { return p == 0 ? 120 : 70; };
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
    lcd.repaint();

    meter.setLevels (p.meter.exchange (0.0f));

    bool armed = p.armed.load();
    if (arm.getToggleState() != armed)
        arm.setToggleState (armed, juce::dontSendNotification);
    arm.lampOn = armed && (mode == Mode::rec || blink);
    arm.face = armed ? Pal::armRed : Pal::button;
    arm.pressedLook = armed;
    arm.repaint();
    repaint (0, 0, getWidth(), 50);
}

void StripComponent::resized()
{
    auto r = getLocalBounds().reduced (10);
    r.removeFromTop (30 + 6);
    lcd.setBounds (r.removeFromTop (78));
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

    arm.setBounds (r.removeFromBottom (42));
    r.removeFromBottom (12);
    r.removeFromTop (10);

    auto area = r.withSizeKeepingCentre (26 + 14 + 100, r.getHeight());
    meter.setBounds (area.removeFromLeft (26));
    area.removeFromLeft (14);
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
    g.setColour (Pal::orange);
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
    addAndMakeVisible (phones);
    addAndMakeVisible (meter);
    addAndMakeVisible (fader);
    syncFromEngine();
}

void MasterStrip::syncFromEngine()
{
    phones.slider.setValue (engine.phones.load(), juce::dontSendNotification);
    fader.slider.setValue (engine.masterFader.load(), juce::dontSendNotification);
}

void MasterStrip::refresh()
{
    meter.setLevels (engine.meterL.exchange (0.0f), engine.meterR.exchange (0.0f));
}

void MasterStrip::resized()
{
    auto r = getLocalBounds().reduced (12);
    r.removeFromTop (52);
    phones.setBounds (r.removeFromTop (44));
    r.removeFromTop (16);
    r.removeFromBottom (24 + 12);
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
    g.drawText (utf ("L \xc2\xb7 R"), juce::Rectangle<int> (meter.getX() - 6, getHeight() - 36, meter.getWidth() + 12, 24),
                juce::Justification::centred);
}
} // namespace tf
