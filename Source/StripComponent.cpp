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

    gain.slider.onValueChange = [this, &p] { p.gain = (float) gain.slider.getValue(); gain.repaint(); };
    high.slider.onValueChange = [this, &p] { p.high = (float) high.slider.getValue(); high.repaint(); };
    mid.slider.onValueChange  = [this, &p] { p.mid  = (float) mid.slider.getValue();  mid.repaint(); };
    low.slider.onValueChange  = [this, &p] { p.low  = (float) low.slider.getValue();  low.repaint(); };
    pan.slider.onValueChange  = [this, &p] { p.pan  = (float) pan.slider.getValue();  pan.repaint(); };
    fader.slider.onValueChange = [this, &p] { p.fader = (float) fader.slider.getValue(); };

    lcd.showLamps = true;
    lcd.onClick = [this] { if (onSourceMenu) onSourceMenu (t, &lcd); };
    lcd.setTooltip ("Source for this track. Right-click anywhere on the strip for the same menu.");

    arm.lamp = true;
    arm.lampColour = Pal::red;
    arm.setClickingTogglesState (true);
    arm.onClick = [this, &p] { p.armed = arm.getToggleState(); };

    for (auto* c : std::initializer_list<juce::Component*> { &lcd, &gain, &high, &mid, &low, &pan, &meter, &fader, &arm })
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
    low.slider.setValue (p.low.load(), juce::dontSendNotification);
    pan.slider.setValue (p.pan.load(), juce::dontSendNotification);
    fader.slider.setValue (p.fader.load(), juce::dontSendNotification);
    arm.setToggleState (p.armed.load(), juce::dontSendNotification);
    repaint();
}

void StripComponent::mouseDown (const juce::MouseEvent& e)
{
    engine.focusedTrack = t;

    if (e.mods.isPopupMenu() && dynamic_cast<juce::Slider*> (e.originalComponent) == nullptr && onSourceMenu)
        onSourceMenu (t, this);
}

void StripComponent::refresh (bool blink)
{
    auto& p = engine.track[t];
    auto source = (Source) p.source.load();
    auto* tape = engine.getTape();
    auto mode = engine.tapeEngine.getMode();

    lcd.top = engine.describeSource (t);
    switch (source)
    {
        case Source::audio:
        {
            auto names = engine.getActiveInputNames();
            auto ch = p.inputChannel.load();
            lcd.sub = juce::isPositiveAndBelow (ch, names.size()) ? names[ch].toUpperCase() : juce::String ("INPUT NOT ACTIVE");
            tag = "AUDIO"; tagColour = Pal::orange;
            break;
        }
        case Source::instrument:
            lcd.sub = engine.followArm.load() ? juce::String ("MIDI IN") : "MIDI CH " + juce::String (t + 1);
            tag = "MIDI"; tagColour = Pal::midiBlue;
            break;
        case Source::bounce:
            lcd.sub = utf ("TAPE \xe2\x86\x92 TRK ") + juce::String (t + 1);
            tag = "BOUNCE"; tagColour = Pal::bounce;
            break;
        case Source::none: default:
            lcd.sub = "RIGHT-CLICK TO PLUG IN";
            tag = {}; break;
    }

    // What's on the tape under the head: noise reduction and recorded speed.
    lcd.nrLamp = false;
    lcd.nrColour = Pal::amber;
    if (tape != nullptr)
    {
        auto block = tape->blockAt (tape->position.load());
        bool recordingHere = mode == Mode::rec && p.armed.load();
        if (recordingHere)
        {
            lcd.nrLamp = engine.tapeEngine.params.nrOn.load();
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

            if (listening)
            {
                auto rs = tape->recordedSpeed (t, block);
                if (rs > 0.0f)
                {
                    auto semis = 12.0 * std::log2 (engine.tapeEngine.getNominalSpeed() / rs);
                    if (std::abs (semis) >= 0.1)
                        lcd.sub = (semis > 0 ? "+" : "") + juce::String (semis, 1) + " SEMITONES";
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
    auto r = getLocalBounds().reduced (12);
    r.removeFromTop (32 + 8);
    lcd.setBounds (r.removeFromTop (52));
    r.removeFromTop (10);
    gain.setBounds (r.removeFromTop (44));
    r.removeFromTop (14);
    auto eq = r.removeFromTop (176).reduced (0, 16);
    high.setBounds (eq.removeFromTop (44)); eq.removeFromTop (6);
    mid.setBounds (eq.removeFromTop (44));  eq.removeFromTop (6);
    low.setBounds (eq.removeFromTop (44));
    r.removeFromTop (6);
    pan.setBounds (r.removeFromTop (44));

    arm.setBounds (r.removeFromBottom (46));
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

    auto head = getLocalBounds().reduced (14, 12).removeFromTop (32);
    g.setColour (Pal::cream);
    g.setFont (lf.cond (40.0f, true));
    g.drawText (juce::String (t + 1), head.removeFromLeft (24), juce::Justification::centredLeft);
    g.setColour (Pal::muted);
    g.setFont (lf.cond (14.0f).withExtraKerningFactor (0.2f));
    g.drawText ("TRACK", head.removeFromLeft (70).withTrimmedTop (10), juce::Justification::centredLeft);

    if (tag.isNotEmpty())
    {
        g.setFont (lf.cond (14.0f, true).withExtraKerningFactor (0.16f));
        auto w = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), tag) + 18;
        auto badge = head.removeFromRight (w).withSizeKeepingCentre (w, 24).toFloat();
        g.setColour (tagColour);
        g.fillRoundedRectangle (badge, 3.0f);
        g.setColour (juce::Colour (0xff1a1917));
        g.drawText (tag, badge.toNearestInt(), juce::Justification::centred);
    }

    // EQ box
    auto eqBox = high.getBounds().getUnion (low.getBounds()).expanded (0, 14).toFloat();
    eqBox = eqBox.withX (12.0f).withWidth ((float) getWidth() - 24.0f);
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
    r.removeFromTop (40);
    phones.setBounds (r.removeFromTop (44));
    r.removeFromTop (16);
    r.removeFromBottom (46 + 12);
    auto area = r.withSizeKeepingCentre (44 + 14 + 100, r.getHeight());
    meter.setBounds (area.removeFromLeft (44));
    area.removeFromLeft (14);
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

    auto head = getLocalBounds().reduced (14, 12).removeFromTop (32);
    g.setColour (Pal::cream);
    g.setFont (lf.cond (30.0f, true).withExtraKerningFactor (0.1f));
    g.drawText ("MASTER", head, juce::Justification::centredLeft);
    g.setColour (Pal::muted);
    g.setFont (lf.cond (14.0f).withExtraKerningFactor (0.2f));
    g.drawText ("STEREO OUT", head, juce::Justification::centredRight);

    g.setFont (lf.cond (15.0f).withExtraKerningFactor (0.2f));
    g.drawText (utf ("L \xc2\xb7 R"), getLocalBounds().reduced (12).removeFromBottom (46).withWidth (44 + 14 + 100)
                                         .withX (meter.getX()), juce::Justification::centredLeft);
}
} // namespace tf
