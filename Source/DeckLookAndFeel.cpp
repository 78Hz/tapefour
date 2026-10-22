#include "DeckLookAndFeel.h"
#include "BinaryData.h"

namespace tf
{
juce::String utf (const char* s) { return juce::String::fromUTF8 (s); }

static DeckLookAndFeel* currentDeckLookAndFeel = nullptr;

DeckLookAndFeel::DeckLookAndFeel()
    : juce::LookAndFeel_V4 (juce::LookAndFeel_V4::ColourScheme (
          Pal::panel, Pal::body, Pal::body, Pal::line, Pal::cream,
          Pal::button, juce::Colour (0xff1a1917), Pal::orange, Pal::cream))
{
    semi   = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowCondensedSemiBold_ttf, BinaryData::BarlowCondensedSemiBold_ttfSize);
    bold   = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowCondensedBold_ttf, BinaryData::BarlowCondensedBold_ttfSize);
    xbi    = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowCondensedExtraBoldItalic_ttf, BinaryData::BarlowCondensedExtraBoldItalic_ttfSize);
    monoTf = juce::Typeface::createSystemTypefaceFor (BinaryData::ShareTechMonoRegular_ttf, BinaryData::ShareTechMonoRegular_ttfSize);
    handTf = juce::Typeface::createSystemTypefaceFor (BinaryData::CaveatVariable_ttf, BinaryData::CaveatVariable_ttfSize);

    currentDeckLookAndFeel = this;

    setColour (juce::PopupMenu::backgroundColourId, Pal::cream);
    setColour (juce::PopupMenu::textColourId, juce::Colour (0xff1a1917));
    setColour (juce::PopupMenu::headerTextColourId, Pal::menuHead);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff1a1917));
    setColour (juce::PopupMenu::highlightedTextColourId, Pal::lcdText);
    setColour (juce::TextEditor::backgroundColourId, Pal::body);
    setColour (juce::TextEditor::outlineColourId, Pal::line);
    setColour (juce::ComboBox::backgroundColourId, Pal::body);
    setColour (juce::ComboBox::outlineColourId, Pal::line);
    setColour (juce::Slider::trackColourId, Pal::orange);
    setColour (juce::Slider::thumbColourId, Pal::cream);
    setColour (juce::TabbedButtonBar::tabOutlineColourId, Pal::line);
    setColour (juce::TabbedButtonBar::frontOutlineColourId, Pal::orange);
    setColour (juce::ListBox::backgroundColourId, Pal::body);
    setColour (juce::TableHeaderComponent::backgroundColourId, Pal::panel);
    setColour (juce::TableHeaderComponent::textColourId, Pal::cream);
    setColour (juce::ToggleButton::tickColourId, Pal::orange);
}

DeckLookAndFeel& DeckLookAndFeel::get()
{
    jassert (currentDeckLookAndFeel != nullptr);
    return *currentDeckLookAndFeel;
}

DeckLookAndFeel::~DeckLookAndFeel()
{
    if (currentDeckLookAndFeel == this)
        currentDeckLookAndFeel = nullptr;
}

juce::Typeface::Ptr DeckLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    // Every font that isn't explicitly one of ours uses Barlow Condensed.
    if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
        return f.isBold() ? bold : semi;
    return juce::LookAndFeel_V4::getTypefaceForFont (f);
}

juce::Font DeckLookAndFeel::cond (float h, bool b) const
{
    return juce::Font (juce::FontOptions (b ? bold : semi).withHeight (h));
}

juce::Font DeckLookAndFeel::brand (float h) const   { return juce::Font (juce::FontOptions (xbi).withHeight (h)); }
juce::Font DeckLookAndFeel::mono (float h) const    { return juce::Font (juce::FontOptions (monoTf).withHeight (h)); }
juce::Font DeckLookAndFeel::hand (float h) const    { return juce::Font (juce::FontOptions (handTf).withHeight (h)); }

juce::Font DeckLookAndFeel::getPopupMenuFont()      { return cond (19.0f, true); }

void DeckLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& text)
{
    g.setFont (cond (14.0f, true).withExtraKerningFactor (0.18f));
    g.setColour (findColour (juce::PopupMenu::headerTextColourId));
    g.drawFittedText (text, area.reduced (12, 0), juce::Justification::bottomLeft, 1);
}

void DeckLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                        float startAngle, float endAngle, juce::Slider& s)
{
    auto d = (float) std::min (w, h) - 6.0f;
    auto c = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).getCentre().translated (0, -1.5f);
    auto r = juce::Rectangle<float> (d, d).withCentre (c);

    g.setColour (Pal::dark);
    g.fillEllipse (r.translated (0, 3.0f));
    g.fillEllipse (r);

    auto inner = r.reduced (2.0f);
    juce::ColourGradient grad (juce::Colour (0xff5d5a52), inner.getX() + inner.getWidth() * 0.35f, inner.getY() + inner.getHeight() * 0.3f,
                               juce::Colour (0xff2a2825), inner.getRight(), inner.getBottom(), true);
    g.setGradientFill (grad);
    g.fillEllipse (inner);

    auto angle = startAngle + pos * (endAngle - startAngle);
    auto radius = inner.getWidth() * 0.5f;
    juce::Path p;
    p.addRoundedRectangle (-1.5f, -radius + 3.0f, 3.0f, radius * 0.42f, 1.5f);
    bool accent = (bool) s.getProperties().getWithDefault ("accent", false);
    bool secondary = (bool) s.getProperties().getWithDefault ("secondary", false);
    g.setColour (secondary ? juce::Colour (0xff8fb0d6) : accent ? Pal::orange : Pal::cream);
    g.fillPath (p, juce::AffineTransform::rotation (angle).translated (c));
}

void DeckLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                        juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearVertical)
    {
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
        return;
    }

    auto cx = (float) x + (float) w * 0.5f;
    g.setColour (Pal::darker);
    g.fillRoundedRectangle (cx - 3.0f, (float) y, 6.0f, (float) h, 3.0f);

    auto capW = std::min (44.0f, (float) w - 6.0f), capH = 26.0f;
    auto cap = juce::Rectangle<float> (capW, capH).withCentre ({ cx, pos });
    g.setColour (Pal::dark);
    g.fillRoundedRectangle (cap.translated (0, 3.0f), 3.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffebe5d8), cap.getX(), cap.getY(),
                                             juce::Colour (0xffbbb4a4), cap.getX(), cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 3.0f);
    g.setColour (juce::Colour (0xff1a1917));
    g.fillRect (cap.getX(), cap.getCentreY() - 1.0f, cap.getWidth(), 2.0f);
}

void drawLamp (juce::Graphics& g, juce::Rectangle<float> r, bool on, juce::Colour c)
{
    if (on)
    {
        g.setColour (c.withAlpha (0.35f));
        g.fillEllipse (r.expanded (r.getWidth() * 0.45f));
        g.setColour (c);
        g.fillEllipse (r);
    }
    else
    {
        g.setColour (juce::Colour (0xff3a3833));
        g.fillEllipse (r);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.drawEllipse (r.reduced (0.5f), 1.0f);
    }
}
} // namespace tf
