#pragma once

#include "Common.h"

namespace tf
{
/** The deck's palette, straight from the mock-up. */
namespace Pal
{
    inline const juce::Colour body      { 0xff1b1a17 };
    inline const juce::Colour panel     { 0xff2e2d29 };
    inline const juce::Colour panelHi   { 0xff3d3b36 };
    inline const juce::Colour master    { 0xff262521 };
    inline const juce::Colour dark      { 0xff121110 };
    inline const juce::Colour darker    { 0xff0c0b0a };
    inline const juce::Colour bay       { 0xff0f0e0d };
    inline const juce::Colour cream     { 0xffece4cf };
    inline const juce::Colour muted     { 0xffb3ad9e };
    inline const juce::Colour faint     { 0xffa39d8e };
    inline const juce::Colour line      { 0xff4a4741 };
    inline const juce::Colour button    { 0xff46443e };
    inline const juce::Colour orange    { 0xffe8702a };
    inline const juce::Colour lcd       { 0xff1c140a };
    inline const juce::Colour lcdText   { 0xfff0a640 };
    inline const juce::Colour lcdDim    { 0xffc98a35 };
    inline const juce::Colour green     { 0xff7fc46a };
    inline const juce::Colour amber     { 0xffe8a33a };
    inline const juce::Colour red       { 0xffe5482f };
    inline const juce::Colour paper     { 0xfff2ecd9 };
    inline const juce::Colour ink       { 0xff24357a };
    inline const juce::Colour inkRed    { 0xffb8321f };
    inline const juce::Colour rule      { 0xffb9c7d9 };
    inline const juce::Colour menuHead  { 0xff7a3d14 };
    inline const juce::Colour midiBlue  { 0xff6f93b8 };
    inline const juce::Colour bounce    { 0xffd8c26a };
}

class DeckLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DeckLookAndFeel();
    ~DeckLookAndFeel() override;

    static DeckLookAndFeel& get();

    juce::Font cond (float height, bool bold = false) const;
    juce::Font brand (float height) const;
    juce::Font mono (float height) const;
    juce::Font hand (float height) const;

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>&, const juce::String&) override;
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    // The Setup window speaks the deck's language: tabs, tick boxes and buttons are LED keys.
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h,
                      bool ticked, bool enabled, bool over, bool down) override;
    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool over, bool down) override;
    int getTabButtonBestWidth (juce::TabBarButton&, int tabDepth) override;
    void drawTabbedButtonBarBackground (juce::TabbedButtonBar&, juce::Graphics&) override {}
    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int, int) override {}
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;

    /** The LED key's body and LED, for anything that draws one itself. */
    static void paintKey (juce::Graphics&, juce::Rectangle<float> r, bool over, bool down, float alpha = 1.0f);
    static void paintLed (juce::Graphics&, juce::Point<float> centre, bool on, juce::Colour colour, float size = 8.0f);

private:
    juce::Typeface::Ptr semi, bold, xbi, monoTf, handTf;
};

void drawLamp (juce::Graphics& g, juce::Rectangle<float> r, bool on, juce::Colour c);
juce::String utf (const char* s);   // literal with non-ASCII characters
} // namespace tf
