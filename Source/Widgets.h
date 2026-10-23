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

    /** The track's chain as a row of little blocks (strip displays). */
    bool chainRow = false, highlighted = false;
    juce::StringArray blocks;
    juce::Array<int> blockStates;     // 0 on, 1 bypassed, 2 loading or missing
    bool roomForMore = true;

    /** Repaints only if something shown has changed since the last call. */
    void repaintIfChanged()
    {
        juce::String chain;
        for (int i = 0; i < blocks.size(); ++i) chain << blocks[i] << blockStates[i] << ",";
        auto now = top + "|" + sub + "|" + low + "|" + chain + (highlighted ? "H" : "") + juce::String ((int) nrLamp + 2 * (int) eqLamp + 4 * (int) midiLamp)
                   + "|" + juce::String::toHexString ((int) nrColour.getARGB()) + juce::String::toHexString ((int) eqColour.getARGB());
        if (now != shown) { shown = now; repaint(); }
    }
    juce::String shown;
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

        if (highlighted)
        {
            g.setColour (Pal::lcdText);
            g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.2f);
        }

        auto in = getLocalBounds().reduced (10, 7);
        g.setColour (Pal::lcdText);
        g.setFont (lf.mono (topSize));
        g.drawFittedText (top, in.removeFromTop ((int) topSize + 2), juce::Justification::topLeft, 1, 0.9f);

        if (chainRow)
        {
            paintChainLayout (g, in);
            return;
        }

        if (showLamps)
        {
            // the lamps get a row of their own, so narrow strips keep all three lines of text
            auto lamps = in.removeFromBottom (12).removeFromRight (136);
            in.removeFromBottom (3);
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
        }
        auto bottom = in.removeFromBottom (14);
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

    void paintChainLayout (juce::Graphics& g, juce::Rectangle<int> in)
    {
        auto& lf = DeckLookAndFeel::get();
        g.setColour (Pal::lcdDim);
        g.setFont (lf.mono (12.0f));
        g.drawFittedText (sub, in.removeFromTop (16), juce::Justification::topLeft, 1, 0.85f);

        // bottom line: what's happening, and the lamps
        auto foot = in.removeFromBottom (12);
        auto lamps = foot.removeFromRight (104);
        foot.removeFromRight (6);
        g.setFont (lf.mono (11.0f));
        g.setColour (Pal::lcdDim);
        g.drawFittedText (low, foot, juce::Justification::centredLeft, 1, 0.8f);
        auto lamp = [&] (const char* name, int w, bool on, juce::Colour c)
        {
            g.setColour (Pal::lcdDim);
            g.drawText (name, lamps.removeFromLeft (w), juce::Justification::centredLeft);
            drawLamp (g, lamps.removeFromLeft (10).toFloat().withSizeKeepingCentre (7, 7), on, c);
            lamps.removeFromLeft (5);
        };
        lamp ("NR", 16, nrLamp, nrColour);
        lamp ("EQ", 16, eqLamp, eqColour);
        lamp ("MIDI", 28, midiLamp, Pal::green);

        // the chain, as blocks
        in.removeFromBottom (5);
        auto row = in.removeFromBottom (17);
        g.setFont (lf.mono (10.5f));
        int x = row.getX();
        const int right = row.getRight();
        auto box = [&] (const juce::String& text, int state, bool ghost)
        {
            auto w = std::max (22, (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 8);
            auto b = juce::Rectangle<int> (x, row.getY(), w, row.getHeight()).toFloat();
            auto alpha = ghost || state != 0 ? 0.5f : 1.0f;
            g.setColour (Pal::lcdDim.withAlpha (alpha));
            if (state == 2) { juce::Path p; p.addRoundedRectangle (b.reduced (0.5f), 2.0f); juce::PathStrokeType (1.0f).createDashedStroke (p, p, std::array<float, 2> { 3.0f, 2.0f }.data(), 2); g.fillPath (p); }
            else g.drawRoundedRectangle (b.reduced (0.5f), 2.0f, 1.0f);
            g.setColour ((state == 0 && ! ghost ? Pal::lcdText : Pal::lcdDim).withAlpha (alpha));
            g.drawText (text, b, juce::Justification::centred, false);
            x += w;
        };
        const int n = blocks.size();
        for (int i = 0; i < n; ++i)
        {
            // keep room for "+N" if the rest won't fit
            auto w = (int) juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), blocks[i]) + 8 + 10;
            if (x + w > right - (i < n - 1 ? 30 : 0))
            {
                box ("+" + juce::String (n - i), 0, true);
                return;
            }
            if (i > 0)
            {
                g.setColour (Pal::lcdDim);
                g.drawText (juce::CharPointer_UTF8 ("\xe2\x80\xba"), juce::Rectangle<int> (x, row.getY(), 9, row.getHeight()), juce::Justification::centred);
                x += 9;
            }
            box (blocks[i], blockStates[i], false);
        }
        if (roomForMore && x + 30 < right)
        {
            if (n > 0)
            {
                g.setColour (Pal::lcdDim);
                g.drawText (juce::CharPointer_UTF8 ("\xe2\x80\xba"), juce::Rectangle<int> (x, row.getY(), 9, row.getHeight()), juce::Justification::centred);
                x += 9;
            }
            box ("+", 0, true);
        }
    }
};

//==============================================================================
/** Mute, solo and arm: a hardware key with an LED above its label. The LED lights; the key doesn't change colour. */
class LedKey : public juce::Button
{
public:
    LedKey (const juce::String& label, juce::Colour led) : juce::Button (label), ledColour (led)
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    juce::Colour ledColour;
    bool ledOn = false;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto& lf = DeckLookAndFeel::get();
        auto r = getLocalBounds().toFloat();
        auto face = r.withTrimmedBottom (3.0f).translated (0, down ? 2.0f : 0.0f);
        if (! down)
        {
            g.setColour (Pal::darker);
            g.fillRoundedRectangle (r.withTrimmedTop (3.0f), 5.0f);
        }
        g.setGradientFill (juce::ColourGradient (juce::Colour (over ? 0xff57554e : 0xff4d4b45), 0, face.getY(),
                                                 juce::Colour (over ? 0xff43413b : 0xff3b3934), 0, face.getBottom(), false));
        g.fillRoundedRectangle (face, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine ((int) face.getY() + 1, face.getX() + 4, face.getRight() - 4);

        auto led = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ face.getCentreX(), face.getY() + face.getHeight() * 0.33f });
        if (ledOn)
        {
            g.setColour (ledColour.withAlpha (0.35f));
            g.fillEllipse (led.expanded (4.0f));
            g.setColour (ledColour);
            g.fillEllipse (led);
        }
        else
        {
            g.setColour (juce::Colour (0xff2b2824));
            g.fillEllipse (led);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawEllipse (led, 1.0f);
        }

        g.setColour (Pal::cream);
        g.setFont (lf.cond (12.0f, true).withExtraKerningFactor (0.14f));
        g.drawText (getButtonText(), face.withTrimmedTop (face.getHeight() * 0.52f).toNearestInt(), juce::Justification::centredTop);
    }
};

//==============================================================================
/** A choice in the amber display style: outlined, or filled when chosen. Optionally a second line and a signal bar. */
class LcdChip : public juce::Button
{
public:
    explicit LcdChip (const juce::String& text, const juce::String& subText = {})
        : juce::Button (text), sub (subText) { setWantsKeyboardFocus (false); setMouseClickGrabsKeyboardFocus (false); }

    juce::String sub;
    bool chosen = false, dim = false, dashed = false;
    float signal = -1.0f;            // 0..1 for an input's level, or -1 for none
    float textSize = 12.5f;

    int idealWidth() const
    {
        auto f = DeckLookAndFeel::get().mono (textSize);
        auto w = juce::GlyphArrangement::getStringWidth (f, getButtonText());
        if (sub.isNotEmpty()) w = std::max (w, juce::GlyphArrangement::getStringWidth (DeckLookAndFeel::get().mono (10.0f), sub));
        return (int) w + 22 + (signal >= 0.0f ? 10 : 0);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto& lf = DeckLookAndFeel::get();
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const auto alpha = dim ? 0.55f : 1.0f;
        if (chosen)
        {
            g.setColour (Pal::lcdText.withAlpha (alpha));
            g.fillRoundedRectangle (r, 3.0f);
        }
        else
        {
            if (over || down)
            {
                g.setColour (Pal::lcdText.withAlpha (0.08f));
                g.fillRoundedRectangle (r, 3.0f);
            }
            g.setColour (Pal::lcdDim.withAlpha (alpha));
            if (dashed)
            {
                juce::Path p;
                p.addRoundedRectangle (r, 3.0f);
                const float dash[] { 4.0f, 3.0f };
                juce::PathStrokeType (1.0f).createDashedStroke (p, p, dash, 2);
                g.fillPath (p);
            }
            else g.drawRoundedRectangle (r, 3.0f, 1.0f);
        }

        auto ink = chosen ? Pal::lcd : Pal::lcdText.withAlpha (alpha);
        auto inkSub = chosen ? Pal::lcd : Pal::lcdDim.withAlpha (alpha);
        auto in = getLocalBounds().reduced (9, 2);
        if (signal >= 0.0f)
        {
            auto bar = in.removeFromRight (4).withSizeKeepingCentre (4, std::min (18, in.getHeight() - 6)).toFloat();
            g.setColour (chosen ? Pal::lcdDim : juce::Colour (0xff3a2a14));
            g.fillRoundedRectangle (bar, 1.0f);
            g.setColour (chosen ? Pal::lcd : Pal::green);
            g.fillRoundedRectangle (bar.withTrimmedTop (bar.getHeight() * (1.0f - juce::jlimit (0.0f, 1.0f, signal))), 1.0f);
            in.removeFromRight (5);
        }
        g.setColour (ink);
        g.setFont (lf.mono (textSize));
        if (sub.isEmpty())
        {
            g.drawFittedText (getButtonText(), in, juce::Justification::centredLeft, 1, 0.8f);
        }
        else
        {
            g.drawFittedText (getButtonText(), in.removeFromTop (in.getHeight() / 2 + 2), juce::Justification::bottomLeft, 1, 0.8f);
            g.setColour (inkSub);
            g.setFont (lf.mono (10.0f));
            g.drawFittedText (sub, in, juce::Justification::topLeft, 1, 0.8f);
        }
    }
};

//==============================================================================
/** A horizontal bar in the amber display style: drag to set, double-click to reset. */
class LcdSlider : public juce::Component
{
public:
    LcdSlider (const juce::String& labelText, double minimum, double maximum, double resetTo, std::function<juce::String (double)> fmt)
        : label (labelText), lo (minimum), hi (maximum), reset (resetTo), format (std::move (fmt)) {}

    std::function<void (double)> onChange;
    double value = 0.0;

    void setValue (double v) { value = juce::jlimit (lo, hi, v); repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto& lf = DeckLookAndFeel::get();
        auto r = getLocalBounds();
        g.setFont (lf.mono (11.0f));
        g.setColour (Pal::lcdDim);
        g.drawText (label, r.removeFromLeft (48), juce::Justification::centredLeft);
        g.setColour (Pal::lcdText);
        g.drawText (format (value), r.removeFromRight (64), juce::Justification::centredRight);
        bar = r.reduced (4, 0).withSizeKeepingCentre (r.getWidth() - 8, 6);
        g.setColour (juce::Colour (0xff3a2a14));
        g.fillRoundedRectangle (bar.toFloat(), 3.0f);
        g.setColour (Pal::lcdText);
        g.fillRoundedRectangle (bar.toFloat().withWidth ((float) bar.getWidth() * (float) ((value - lo) / (hi - lo))), 3.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override { set (e); }
    void mouseDrag (const juce::MouseEvent& e) override { set (e); }
    void mouseDoubleClick (const juce::MouseEvent&) override { setValue (reset); if (onChange) onChange (value); }

private:
    void set (const juce::MouseEvent& e)
    {
        if (bar.getWidth() <= 0) return;
        setValue (lo + (hi - lo) * juce::jlimit (0.0, 1.0, (double) (e.x - bar.getX()) / bar.getWidth()));
        if (onChange) onChange (value);
    }

    juce::String label;
    double lo, hi, reset;
    std::function<juce::String (double)> format;
    juce::Rectangle<int> bar;
};

//==============================================================================
/** Dims the deck and holds a card; clicking outside the card (or Escape) closes it. */
class Overlay : public juce::Component
{
public:
    Overlay (std::unique_ptr<juce::Component> content, juce::Rectangle<int> cardBounds) : card (std::move (content))
    {
        addAndMakeVisible (*card);
        card->setBounds (cardBounds);
        setWantsKeyboardFocus (false);
    }

    std::function<void()> onDismiss;
    bool cardShadow = true;   // off for cards that cast their own
    float dimming = 0.62f;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a0908).withAlpha (dimming));
        if (! cardShadow) return;
        auto r = card->getBounds().toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (r.translated (0, 14).expanded (6), 12.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == this && onDismiss)
            onDismiss();
    }

    juce::Component* getCard() const noexcept { return card.get(); }

private:
    std::unique_ptr<juce::Component> card;
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

    int knobSize = 46;
    bool stacked = false;      // knob on the left, label and value beside it (narrow strips)

    /** A secondary control (the mid's frequency): smaller, with a blue pointer and quieter label. */
    void setSecondary (int size)
    {
        knobSize = size;
        secondary = true;
        slider.getProperties().set ("secondary", true);
        resized();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (stacked)
        {
            slider.setBounds (r.removeFromLeft (knobSize).withSizeKeepingCentre (knobSize, knobSize));
            return;
        }
        // The knob sits on the strip's centre line whatever its size.
        auto x = (r.getWidth() - knobSize) / 2;
        slider.setBounds (r.withX (x).withWidth (knobSize).withSizeKeepingCentre (knobSize, std::min (r.getHeight(), knobSize)));
    }

    void paint (juce::Graphics& g) override
    {
        auto& lf = DeckLookAndFeel::get();
        auto r = getLocalBounds();
        auto value = formatter != nullptr ? formatter (slider.getValue()) : juce::String();
        if (stacked)
        {
            auto text = r.withTrimmedLeft (knobSize + 10);
            g.setFont (lf.cond (14.0f).withExtraKerningFactor (0.14f));
            g.setColour (Pal::muted);
            g.drawText (label, text.removeFromTop (text.getHeight() / 2), juce::Justification::bottomLeft);
            g.setFont (lf.mono (13.0f));
            g.setColour (Pal::faint);
            g.drawText (value, text.withTrimmedTop (2), juce::Justification::topLeft);
            return;
        }
        const int gap = 10, side = (r.getWidth() - 46) / 2;   // labels and values line up across knob sizes
        g.setFont (lf.cond (secondary ? 13.0f : 15.0f).withExtraKerningFactor (0.14f));
        g.setColour (secondary ? Pal::faint.darker (0.15f) : Pal::muted);
        g.drawText (label, r.withWidth (side - gap), juce::Justification::centredRight);
        g.setFont (lf.mono (secondary ? 13.0f : 14.0f));
        g.setColour (Pal::faint);
        g.drawText (value, r.withTrimmedLeft (side + 46 + gap), juce::Justification::centredLeft);
    }

private:
    juce::String label;
    std::function<juce::String (double)> formatter;
    bool secondary = false;
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
        // Only redraw when a segment lights or goes out.
        auto lit = litSegments (level[0]) * 32 + litSegments (level[1]);
        if (lit != lastLit) { lastLit = lit; repaint(); }
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
    static int litSegments (float level)
    {
        static const float vu[20] { -30, -26, -22, -19, -16, -13, -11, -9, -7, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 6 };
        auto db = juce::Decibels::gainToDecibels (level, -100.0f) + 12.0f;
        int n = 0;
        while (n < 20 && db >= vu[n]) ++n;
        return n;
    }

    int numChannels;
    float level[2] {};
    int lastLit = -1;
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
