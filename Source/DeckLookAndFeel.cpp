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
//==============================================================================
void DeckLookAndFeel::paintKey (juce::Graphics& g, juce::Rectangle<float> r, bool over, bool down, float alpha)
{
    auto face = r.withTrimmedBottom (3.0f).translated (0, down ? 2.0f : 0.0f);
    if (! down)
    {
        g.setColour (Pal::darker.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (r.withTrimmedTop (3.0f), 5.0f);
    }
    g.setGradientFill (juce::ColourGradient (juce::Colour (over ? 0xff57554e : 0xff4d4b45).withMultipliedAlpha (alpha), 0, face.getY(),
                                             juce::Colour (over ? 0xff43413b : 0xff3b3934).withMultipliedAlpha (alpha), 0, face.getBottom(), false));
    g.fillRoundedRectangle (face, 5.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f * alpha));
    g.drawHorizontalLine ((int) face.getY() + 1, face.getX() + 4, face.getRight() - 4);
}

void DeckLookAndFeel::paintLed (juce::Graphics& g, juce::Point<float> centre, bool on, juce::Colour colour, float size)
{
    auto led = juce::Rectangle<float> (size, size).withCentre (centre);
    if (on)
    {
        g.setColour (colour.withAlpha (0.35f));
        g.fillEllipse (led.expanded (size * 0.4f));
        g.setColour (colour);
        g.fillEllipse (led);
    }
    else
    {
        g.setColour (juce::Colour (0xff2b2824));
        g.fillEllipse (led);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawEllipse (led, 1.0f);
    }
}

void DeckLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool down)
{
    // A small LED key, then the label.
    const float alpha = b.isEnabled() ? 1.0f : 0.45f;
    auto r = b.getLocalBounds().toFloat();
    auto key = r.removeFromLeft (30.0f).withSizeKeepingCentre (30.0f, std::min (26.0f, r.getHeight()));
    paintKey (g, key, over, down, alpha);
    paintLed (g, { key.getCentreX(), key.getCentreY() - 1.0f + (down ? 2.0f : 0.0f) }, b.getToggleState(), Pal::amber);
    if (b.getButtonText().isNotEmpty())
    {
        g.setColour (Pal::cream.withMultipliedAlpha (alpha));
        g.setFont (cond (16.0f));
        g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (12.0f).toNearestInt(), juce::Justification::centredLeft, 2, 0.8f);
    }
}

void DeckLookAndFeel::drawTickBox (juce::Graphics& g, juce::Component&, float x, float y, float w, float h,
                                   bool ticked, bool enabled, bool over, bool down)
{
    // Lists of channels and the like: the same LED key, sized to the row.
    auto key = juce::Rectangle<float> (x, y, w, h).withSizeKeepingCentre (std::max (w, h * 1.2f), h);
    paintKey (g, key, over, down, enabled ? 1.0f : 0.45f);
    paintLed (g, { key.getCentreX(), key.getCentreY() - 1.0f }, ticked, Pal::amber, std::max (5.0f, h * 0.32f));
}

int DeckLookAndFeel::getTabButtonBestWidth (juce::TabBarButton& b, int)
{
    return (int) juce::GlyphArrangement::getStringWidth (cond (14.0f, true).withExtraKerningFactor (0.14f), b.getButtonText()) + 50;
}

void DeckLookAndFeel::drawTabButton (juce::TabBarButton& b, juce::Graphics& g, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (4.0f, 6.0f);
    paintKey (g, r, over, down);
    auto face = r.withTrimmedBottom (3.0f).translated (0, down ? 2.0f : 0.0f);
    paintLed (g, { face.getX() + 15.0f, face.getCentreY() }, b.isFrontTab(), Pal::amber);
    g.setColour (Pal::cream);
    g.setFont (cond (14.0f, true).withExtraKerningFactor (0.14f));
    g.drawText (b.getButtonText(), face.withTrimmedLeft (26.0f).withTrimmedRight (8.0f), juce::Justification::centred);
}

void DeckLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    paintKey (g, b.getLocalBounds().toFloat(), over, down, b.isEnabled() ? 1.0f : 0.45f);
}

void DeckLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool down)
{
    g.setColour (Pal::cream.withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.45f));
    g.setFont (cond (14.0f, true).withExtraKerningFactor (0.1f));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().withTrimmedBottom (3).translated (0, down ? 2 : 0), juce::Justification::centred, 1, 0.8f);
}
} // namespace tf
