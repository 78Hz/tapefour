#pragma once

#include "Widgets.h"

/** Parts shared by the built-in instruments' editors (the chain panel's lower half). */
namespace tf::iw
{
inline constexpr int captionW = 154, valueW = 200, rowH = 22, chipH = 24, gapY = 7;

inline void text (juce::Graphics& g, const juce::String& s, juce::Rectangle<int> r, juce::Colour c, float size = 10.5f,
                 juce::Justification j = juce::Justification::centredLeft)
{
    g.setColour (c);
    g.setFont (DeckLookAndFeel::get().mono (size));
    g.drawText (s, r, j, true);
}

/** One continuous control: caption, bar (drag to set, double-click for its default), value. */
class Bar : public juce::Component
{
public:
    Bar (juce::RangedAudioParameter& p, juce::String captionText, std::function<juce::String (float)> f)
        : param (p), caption (std::move (captionText)), fmt (std::move (f)) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        text (g, caption, r.removeFromLeft (captionW), Pal::lcdDim);
        text (g, fmt (param.convertFrom0to1 (param.getValue())), r.removeFromRight (valueW), Pal::lcdText, 11.0f, juce::Justification::centredRight);
        bar = r.reduced (2, 0).withSizeKeepingCentre (r.getWidth() - 4, 6);
        const auto v = param.getValue();
        g.setColour (Pal::lcdRule);
        g.fillRoundedRectangle (bar.toFloat(), 3.0f);
        g.setColour (Pal::lcdText);
        const auto& range = param.getNormalisableRange();
        if (range.start < 0.0f && range.end > 0.0f)   // centred controls grow from their middle
        {
            const float mid = (float) bar.getX() + (float) bar.getWidth() * param.convertTo0to1 (0.0f);
            const float at = (float) bar.getX() + (float) bar.getWidth() * v;
            g.fillRoundedRectangle (juce::Rectangle<float> (std::min (mid, at), (float) bar.getY(), std::abs (at - mid) + 1.0f, 6.0f), 3.0f);
            g.setColour (Pal::lcdDim);
            g.fillRect (juce::Rectangle<float> (mid - 0.5f, (float) bar.getY() - 3.0f, 1.0f, 12.0f));
        }
        else
            g.fillRoundedRectangle (bar.toFloat().withWidth ((float) bar.getWidth() * v), 3.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override { param.beginChangeGesture(); set (e); }
    void mouseDrag (const juce::MouseEvent& e) override { set (e); }
    void mouseUp (const juce::MouseEvent&) override { param.endChangeGesture(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { param.setValueNotifyingHost (param.getDefaultValue()); repaint(); }

private:
    void set (const juce::MouseEvent& e)
    {
        if (bar.getWidth() <= 0) return;
        param.setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, (float) (e.x - bar.getX()) / (float) bar.getWidth()));
        repaint();
    }

    juce::RangedAudioParameter& param;
    juce::String caption;
    std::function<juce::String (float)> fmt;
    juce::Rectangle<int> bar;
};

/** A choice: caption and a row of chips (wrapping), one of them chosen. */
class Chips : public juce::Component
{
public:
    Chips (juce::String captionText, const juce::StringArray& labels, std::function<int()> chosenFn, std::function<void (int)> pickFn)
        : caption (std::move (captionText)), chosen (std::move (chosenFn)), pick (std::move (pickFn))
    {
        for (int i = 0; i < labels.size(); ++i)
        {
            auto* c = chips.add (new LcdChip (labels[i]));
            c->textSize = 11.0f;
            c->onClick = [this, i]
            {
                // (pick may rebuild the panel and delete this row: nothing of it is touched after, unless it's still here)
                juce::Component::SafePointer<Chips> safe (this);
                pick (i);
                if (safe != nullptr) safe->refresh();
            };
            addAndMakeVisible (c);
        }
        refresh();
    }

    /** Lays the chips out in a width; returns the height they need. */
    int layout (int width)
    {
        int x = captionW, y = 0;
        for (auto* c : chips)
        {
            const int w = c->idealWidth();
            if (x + w > width && x > captionW) { x = captionW; y += chipH + 5; }
            c->setBounds (x, y, w, chipH);
            x += w + 5;
        }
        return y + chipH;
    }

    void refresh()
    {
        const int now = chosen ? chosen() : -1;
        for (int i = 0; i < chips.size(); ++i)
            if (chips[i]->chosen != (i == now)) { chips[i]->chosen = i == now; chips[i]->repaint(); }
    }

    void paint (juce::Graphics& g) override { text (g, caption, { 0, 0, captionW, chipH }, Pal::lcdDim); }

    juce::OwnedArray<LcdChip> chips;

private:
    juce::String caption;
    std::function<int()> chosen;
    std::function<void (int)> pick;
};


/** A knob on a parameter: an arc; drag up or down, the wheel for fine steps, double-click its default. */
class Knob : public juce::Component,
             public juce::SettableTooltipClient
{
public:
    Knob (juce::RangedAudioParameter& p, juce::String l, std::function<juce::String (float)> f, int r)
        : param (p), label (std::move (l)), fmt (std::move (f)), radius (r) {}

    void paint (juce::Graphics& g) override
    {
        const float v = param.getValue();
        const auto c = juce::Point<float> ((float) getWidth() * 0.5f, (float) radius + 4.0f);
        constexpr float a0 = juce::MathConstants<float>::pi * 0.75f, span = juce::MathConstants<float>::pi * 1.5f;
        const float h = juce::MathConstants<float>::halfPi, am = a0 + span * v;
        juce::Path arc;
        arc.addCentredArc (c.x, c.y, (float) radius, (float) radius, 0.0f, a0 + h, a0 + span + h, true);
        g.setColour (Pal::lcdRule);
        g.strokePath (arc, juce::PathStrokeType (3.0f));
        if (v > 0.0f)
        {
            juce::Path on;
            on.addCentredArc (c.x, c.y, (float) radius, (float) radius, 0.0f, a0 + h, am + h, true);
            g.setColour (Pal::lcdText);
            g.strokePath (on, juce::PathStrokeType (3.0f));
        }
        g.setColour (Pal::lcdText);
        g.drawLine (c.x + std::cos (am) * ((float) radius - 9.0f), c.y + std::sin (am) * ((float) radius - 9.0f),
                    c.x + std::cos (am) * ((float) radius - 2.0f), c.y + std::sin (am) * ((float) radius - 2.0f), 2.0f);
        const int y = 2 * radius + 8;
        text (g, label, { 0, y, getWidth(), 13 }, Pal::lcdDim, 9.0f, juce::Justification::centred);
        text (g, fmt (param.convertFrom0to1 (v)), { 0, y + 12, getWidth(), 14 }, Pal::lcdText, 10.0f, juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent&) override { from = param.getValue(); param.beginChangeGesture(); }
    void mouseUp (const juce::MouseEvent&) override { param.endChangeGesture(); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        param.setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, from - (float) e.getDistanceFromDragStartY() / 200.0f));
        changed();
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        param.setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, param.getValue() + (w.deltaY > 0 ? 0.005f : -0.005f)));
        changed();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { param.setValueNotifyingHost (param.getDefaultValue()); changed(); }
    /** Where it sits for a centre and radius. */
    void place (int cx, int cy) { setBounds (cx - 45, cy - radius - 4, 90, 2 * radius + 36); }
    std::function<void()> onChange;

private:
    void changed() { repaint(); if (onChange) onChange(); }
    juce::RangedAudioParameter& param;
    juce::String label;
    std::function<juce::String (float)> fmt;
    int radius;
    float from = 0.0f;
};

/** A cents-from-equal chart for 12 notes: bars up and down from a centre line, the value beside each. */
void paintCentsChart (juce::Graphics&, juce::Rectangle<int> area, const std::array<double, 12>& cents, double range);
} // namespace tf::iw
