#pragma once

#include "DeckLookAndFeel.h"

namespace tf
{
//==============================================================================
/** Amber display, two or three lines, clickable. */
class LcdButton : public juce::Button
{
public:
    LcdButton() : juce::Button ("lcd") { setWantsKeyboardFocus (false); setMouseClickGrabsKeyboardFocus (false); }

    juce::String top, sub, low;
    bool showLamps = false, nrLamp = false, eqLamp = false, midiLamp = false;
    juce::Colour nrColour = Pal::amber, eqColour = Pal::green;
    float topSize = 17.0f;

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto& lf = DeckLookAndFeel::get();
        auto r = getLocalBounds().toFloat();
        g.setColour (Pal::lcd);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        if (over && isEnabled() && onClick != nullptr)
        {
            g.setColour (Pal::lcdText.withAlpha (0.06f));
            g.fillRoundedRectangle (r, 4.0f);
        }

        auto in = getLocalBounds().reduced (10, 7);
        g.setColour (Pal::lcdText);
        g.setFont (lf.mono (topSize));
        g.drawFittedText (top, in.removeFromTop ((int) topSize + 2), juce::Justification::topLeft, 1, 0.9f);

        auto bottom = in.removeFromBottom (14);
        if (showLamps)
        {
            auto lamps = bottom.removeFromRight (136);
            g.setFont (lf.mono (12.0f));
            g.setColour (Pal::lcdDim);
            g.drawText ("NR", lamps.removeFromLeft (20), juce::Justification::centredLeft);
            drawLamp (g, lamps.removeFromLeft (12).toFloat().withSizeKeepingCentre (8, 8), nrLamp, nrColour);
            lamps.removeFromLeft (10);
            g.setColour (Pal::lcdDim);
            g.drawText ("EQ", lamps.removeFromLeft (20), juce::Justification::centredLeft);
            drawLamp (g, lamps.removeFromLeft (12).toFloat().withSizeKeepingCentre (8, 8), eqLamp, eqColour);
            lamps.removeFromLeft (10);
            g.setColour (Pal::lcdDim);
            g.drawText ("MIDI", lamps.removeFromLeft (36), juce::Justification::centredLeft);
            drawLamp (g, lamps.removeFromLeft (12).toFloat().withSizeKeepingCentre (8, 8), midiLamp, Pal::green);
            bottom.removeFromRight (8);
        }
        g.setColour (Pal::lcdDim);
        if (low.isNotEmpty() || showLamps)
        {
            // three lines: routing in the middle, audio path at the bottom
            g.setFont (lf.mono (13.0f));
            g.drawFittedText (sub, in.withTrimmedTop (std::max (0, in.getHeight() - 16)), juce::Justification::bottomLeft, 1, 0.9f);
            g.setFont (lf.mono (12.0f));
            g.drawFittedText (low, bottom, juce::Justification::bottomLeft, 1, 0.9f);
        }
        else
        {
            g.setFont (lf.mono (13.0f));
            g.drawFittedText (sub, bottom, juce::Justification::bottomLeft, 1, 0.9f);
        }
    }
};

//==============================================================================
/** Cream-card choice: dark when chosen, outlined when not. */
class ChipButton : public juce::Button
{
public:
    explicit ChipButton (const juce::String& text, const juce::String& subText = {})
        : juce::Button (text), hint (subText) { setWantsKeyboardFocus (false); setMouseClickGrabsKeyboardFocus (false); }

    juce::String hint;
    bool chosen = false;
    float textSize = 17.0f;

    int idealWidth() const
    {
        auto f = DeckLookAndFeel::get().cond (textSize, true);
        return (int) juce::GlyphArrangement::getStringWidth (f, getButtonText()) + 30;
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto& lf = DeckLookAndFeel::get();
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        auto ink = juce::Colour (0xff1a1917);
        if (chosen)
        {
            g.setColour (ink);
            g.fillRoundedRectangle (r, 5.0f);
        }
        else
        {
            g.setColour (over || down ? juce::Colour (0xfffffaea) : juce::Colour (0xfff6f1e2));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (ink);
            g.drawRoundedRectangle (r.reduced (1.0f), 5.0f, 2.0f);
        }

        g.setColour (chosen ? Pal::lcdText : ink);
        if (hint.isEmpty())
        {
            g.setFont (lf.cond (textSize, true).withExtraKerningFactor (0.04f));
            g.drawFittedText (getButtonText(), getLocalBounds().reduced (10, 0), juce::Justification::centred, 1, 0.8f);
        }
        else
        {
            auto in = getLocalBounds().reduced (12, 10);
            g.setFont (lf.cond (19.0f, true).withExtraKerningFactor (0.05f));
            g.drawFittedText (getButtonText(), in.removeFromTop (in.getHeight() / 2), juce::Justification::bottomLeft, 1, 0.8f);
            g.setFont (lf.cond (15.0f));
            g.drawFittedText (hint, in.withTrimmedTop (4), juce::Justification::topLeft, 1, 0.8f);
        }
    }
};

//==============================================================================
/** Dark hardware button; optionally a latching "pill" or with an indicator lamp. */
class FlatButton : public juce::Button
{
public:
    explicit FlatButton (const juce::String& text) : juce::Button (text) { setWantsKeyboardFocus (false); setMouseClickGrabsKeyboardFocus (false); }

    bool pill = false, lamp = false, lampOn = false, pressedLook = false;
    juce::Colour lampColour = Pal::amber, onColour = Pal::orange, face = Pal::button;
    float textSize = 15.0f;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto& lf = DeckLookAndFeel::get();
        auto r = getLocalBounds().toFloat();

        if (pill)
        {
            bool on = getToggleState();
            g.setColour (on ? onColour : juce::Colour (0xff1f1e1b));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (on ? juce::Colour (0xff1a1917) : Pal::muted);
        }
        else
        {
            bool sunk = down || pressedLook;
            auto faceR = r.withTrimmedBottom (3.0f).translated (0, sunk ? 2.0f : 0.0f);
            if (! sunk)
            {
                g.setColour (Pal::dark);
                g.fillRoundedRectangle (r.withTrimmedTop (3.0f), 5.0f);
            }
            g.setColour (over ? face.brighter (0.08f) : face);
            g.fillRoundedRectangle (faceR, 5.0f);
            g.setColour (Pal::cream);
            r = faceR;
        }

        g.setFont (lf.cond (textSize, true).withExtraKerningFactor (0.14f));
        auto label = getButtonText();
        auto tr = r.toNearestInt();
        if (lamp)
        {
            auto tw = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), label) + 34;
            auto block = tr.withSizeKeepingCentre (std::min (tr.getWidth() - 8, tw), tr.getHeight());
            drawLamp (g, block.removeFromLeft (12).toFloat().withSizeKeepingCentre (10, 10), lampOn, lampColour);
            block.removeFromLeft (10);
            g.setColour (Pal::cream);
            g.drawFittedText (label, block, juce::Justification::centredLeft, 1, 0.85f);
        }
        else
        {
            g.drawFittedText (label, tr.reduced (4, 0), juce::Justification::centred, 1, 0.8f);
        }
    }
};

//==============================================================================
/** Cream piano-style transport key. */
class KeyButton : public juce::Button
{
public:
    enum Icon { rec, rew, ff, play, stop };
    KeyButton (Icon i, const juce::String& label) : juce::Button (label), icon (i) {}

    bool latched = false;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto& lf = DeckLookAndFeel::get();
        auto r = getLocalBounds().toFloat();
        bool in = latched || down;
        auto face = r.withTrimmedBottom (6.0f).translated (0, in ? 4.0f : 0.0f);

        if (! in)
        {
            g.setColour (juce::Colour (0xff8a8578));
            g.fillRoundedRectangle (r.withTrimmedTop (6.0f), 7.0f);
        }
        g.setColour (in ? juce::Colour (0xffc3bcac) : (over ? juce::Colour (0xffeee9dc) : juce::Colour (0xffe6e0d2)));
        g.fillRoundedRectangle (face, 5.0f);
        if (in)
        {
            g.setColour (juce::Colours::black.withAlpha (0.18f));
            g.fillRoundedRectangle (face.withHeight (4.0f), 2.0f);
        }

        auto ink = icon == rec ? Pal::inkRed : juce::Colour (0xff1a1917);
        g.setColour (ink);
        auto ic = face.withTrimmedBottom (face.getHeight() * 0.42f).withSizeKeepingCentre (28, 18);
        juce::Path p;
        auto cx = ic.getCentreX(), cy = ic.getCentreY();
        switch (icon)
        {
            case rec:  p.addEllipse (cx - 8, cy - 8, 16, 16); break;
            case stop: p.addRectangle (cx - 7, cy - 7, 14, 14); break;
            case play: p.addTriangle (cx - 6, cy - 9, cx - 6, cy + 9, cx + 9, cy); break;
            case ff:   p.addTriangle (cx - 12, cy - 7, cx - 12, cy + 7, cx, cy);
                       p.addTriangle (cx, cy - 7, cx, cy + 7, cx + 12, cy); break;
            case rew:  p.addTriangle (cx + 12, cy - 7, cx + 12, cy + 7, cx, cy);
                       p.addTriangle (cx, cy - 7, cx, cy + 7, cx - 12, cy); break;
        }
        g.fillPath (p);
        g.setFont (lf.cond (15.0f, true).withExtraKerningFactor (0.16f));
        g.drawText (getButtonText(), face.withTrimmedTop (face.getHeight() * 0.55f).toNearestInt(), juce::Justification::centredTop);
    }

private:
    Icon icon;
};

//==============================================================================
/** Label | knob | value, one control per row. */
class LabeledKnob : public juce::Component
{
public:
    LabeledKnob (const juce::String& labelText, bool accent, std::function<juce::String (double)> fmt)
        : label (labelText), formatter (std::move (fmt))
    {
        slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * -0.75f, juce::MathConstants<float>::pi * 0.75f, true);
        slider.setMouseDragSensitivity (180);
        slider.getProperties().set ("accent", accent);
        slider.onValueChange = [this] { repaint(); };
        addAndMakeVisible (slider);
    }

    juce::Slider slider;

    void resized() override
    {
        auto r = getLocalBounds();
        auto third = (r.getWidth() - 46) / 2;
        slider.setBounds (r.withX (third).withWidth (46));
    }

    void paint (juce::Graphics& g) override
    {
        auto& lf = DeckLookAndFeel::get();
        auto r = getLocalBounds();
        auto third = (r.getWidth() - 46) / 2;
        g.setFont (lf.cond (15.0f).withExtraKerningFactor (0.14f));
        g.setColour (Pal::muted);
        g.drawText (label, r.withWidth (third - 12), juce::Justification::centredRight);
        g.setFont (lf.mono (14.0f));
        g.setColour (Pal::faint);
        g.drawText (formatter != nullptr ? formatter (slider.getValue()) : juce::String(),
                    r.withTrimmedLeft (third + 46 + 12), juce::Justification::centredLeft);
    }

private:
    juce::String label;
    std::function<juce::String (double)> formatter;
};

//==============================================================================
/** LED bar meter, 20 segments; 0 VU sits at -12 dBFS like a tape deck. */
class Meter : public juce::Component
{
public:
    explicit Meter (int channels = 1) : numChannels (channels) {}

    void setLevels (float l, float r = 0.0f)
    {
        auto upd = [] (float& disp, float v) { disp = std::max (v, disp * 0.86f); };
        upd (level[0], l);
        upd (level[1], r);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        static const float vu[20] { -30, -26, -22, -19, -16, -13, -11, -9, -7, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 6 };
        auto r = getLocalBounds().toFloat();
        g.setColour (Pal::dark);
        g.fillRoundedRectangle (r, 4.0f);

        auto inner = r.reduced (5.0f);
        auto colW = (inner.getWidth() - (numChannels - 1) * 3.0f) / numChannels;
        auto segH = (inner.getHeight() - 19 * 3.0f) / 20.0f;

        for (int c = 0; c < numChannels; ++c)
        {
            auto db = juce::Decibels::gainToDecibels (level[c], -100.0f) + 12.0f;
            auto x = inner.getX() + c * (colW + 3.0f);
            for (int i = 0; i < 20; ++i)
            {
                auto y = inner.getBottom() - (i + 1) * segH - i * 3.0f;
                auto col = vu[i] >= 3 ? Pal::red : (vu[i] >= 0 ? Pal::amber : Pal::green);
                bool on = db >= vu[i];
                g.setColour (on ? col : juce::Colour (0xff34322d));
                g.fillRoundedRectangle (x, y, colW, segH, 1.5f);
            }
        }
    }

private:
    int numChannels;
    float level[2] {};
};

//==============================================================================
/** Long fader with its dB scale. */
class Fader : public juce::Component
{
public:
    Fader()
    {
        slider.setSliderStyle (juce::Slider::LinearVertical);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange (0.0, 1.0);
        slider.setSliderSnapsToMousePosition (false);
        slider.setDoubleClickReturnValue (true, 0.75);
        addAndMakeVisible (slider);
    }

    juce::Slider slider;

    void resized() override
    {
        slider.setBounds (getLocalBounds().withWidth (52));
    }

    void paint (juce::Graphics& g) override
    {
        auto& lf = DeckLookAndFeel::get();
        g.setFont (lf.mono (12.0f));
        g.setColour (Pal::faint);

        // positions where the taper (f/0.75)^3 gives these levels
        struct Mark { const char* text; double pos; };
        const Mark marks[] { { "+7", 1.0 }, { "+5", 0.75 * std::pow (10.0, 5.0 / 60.0) }, { "0", 0.75 },
                             { "-5", 0.75 * std::pow (10.0, -5.0 / 60.0) }, { "-10", 0.75 * std::pow (10.0, -10.0 / 60.0) },
                             { "-20", 0.75 * std::pow (10.0, -20.0 / 60.0) }, { "-40", 0.75 * std::pow (10.0, -40.0 / 60.0) },
                             { "-inf", 0.0 } };
        auto top = (float) slider.getY() + 13.0f, bottom = (float) slider.getBottom() - 13.0f;
        for (auto& m : marks)
        {
            auto y = bottom - (float) m.pos * (bottom - top);
            g.drawText (m.text, juce::Rectangle<float> (56.0f, y - 7.0f, 40.0f, 14.0f), juce::Justification::centredLeft);
        }
    }
};
//==============================================================================
/**
    The deck's tape switch: NORMAL / CHROME / METAL. Each position shows its type and
    playback EQ. A lamp shows the position it's at: green when that suits the loaded
    cassette, amber when it doesn't.
*/
class TapeSwitch : public juce::Component, public juce::SettableTooltipClient
{
public:
    TapeSwitch()
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        setTitle ("Tape type switch");
    }

    int position = 0, tapeType = 0;
    bool hasTape = true;
    std::function<void (int)> onChange;

    static constexpr int labelW = 62, segW = 102;

    void paint (juce::Graphics& g) override
    {
        auto& lf = DeckLookAndFeel::get();
        auto r = getLocalBounds();
        auto label = r.removeFromLeft (labelW);
        g.setColour (Pal::muted);
        g.setFont (lf.cond (12.0f, true).withExtraKerningFactor (0.22f));
        g.drawText ("TAPE", label.withTrimmedBottom (label.getHeight() / 2), juce::Justification::bottomLeft);
        g.drawText (juce::CharPointer_UTF8 ("BIAS \xc2\xb7 EQ"), label.withTrimmedTop (label.getHeight() / 2 + 3), juce::Justification::topLeft);

        auto track = r.toFloat();
        g.setColour (Pal::darker);
        g.fillRoundedRectangle (track, 7.0f);
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawRoundedRectangle (track.reduced (0.5f), 7.0f, 1.0f);

        static const char* names[] { "NORMAL", "CHROME", "METAL" };
        static const char* subs[]  { "TYPE I \xc2\xb7 120\xc2\xb5s", "TYPE II \xc2\xb7 70\xc2\xb5s", "TYPE IV \xc2\xb7 70\xc2\xb5s" };
        for (int i = 0; i < 3; ++i)
        {
            auto seg = segment (i).toFloat();
            const bool sel = i == position;
            if (sel)
            {
                g.setColour (juce::Colour (0xff6f6a5f));
                g.fillRoundedRectangle (seg.translated (0, 3), 5.0f);
                g.setGradientFill (juce::ColourGradient (juce::Colour (0xffebe5d8), 0, seg.getY(), juce::Colour (0xffc9c2b2), 0, seg.getBottom(), false));
                g.fillRoundedRectangle (seg, 5.0f);
            }
            auto in = seg.reduced (8, 0);
            auto lampR = in.removeFromLeft (9).withSizeKeepingCentre (9, 9);
            // One lamp: the position the switch is at. Green when it suits the tape, amber when it doesn't.
            if (sel) drawLamp (g, lampR, true, ! hasTape || position == tapeType ? Pal::green : Pal::amber);
            else { g.setColour (juce::Colour (0xff2c2a26)); g.fillEllipse (lampR); }
            in.removeFromLeft (8);
            g.setColour (sel ? juce::Colour (0xff1a1917) : Pal::muted);
            g.setFont (lf.cond (14.0f, true).withExtraKerningFactor (0.14f));
            g.drawText (names[i], in.withTrimmedBottom (in.getHeight() * 0.5f - 1), juce::Justification::bottomLeft);
            g.setColour (sel ? juce::Colour (0xff5c564a) : juce::Colour (0xff8c877b));
            g.setFont (lf.cond (11.0f, true).withExtraKerningFactor (0.04f));
            g.drawText (juce::CharPointer_UTF8 (subs[i]), in.withTrimmedTop (in.getHeight() * 0.5f + 2), juce::Justification::topLeft);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < 3; ++i)
            if (segment (i).contains (e.getPosition()) && i != position)
            {
                position = i;
                repaint();
                if (onChange) onChange (i);
            }
    }

private:
    juce::Rectangle<int> segment (int i) const
    {
        auto r = getLocalBounds().withTrimmedLeft (labelW).reduced (4);
        return r.withWidth (segW).translated (i * segW, 0);
    }
};
} // namespace tf
