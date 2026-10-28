#pragma once

#include "Widgets.h"
#include "AudioEngine.h"

namespace tf
{
/** One channel strip, every control in a single column, top to bottom; mute, solo and arm beside the fader. */
class StripComponent : public juce::Component
{
public:
    StripComponent (AudioEngine& engine, int index);
    ~StripComponent() override;

    std::function<void (int track)> onOpenChain;   // click the display
    std::function<void (int track)> onEditSound;   // double-click the display

    void refresh (bool blinkPhase);
    void syncFromEngine();
    void setChainOpen (bool open) { lcd.highlighted = open; lcd.repaintIfChanged(); }
    juce::Rectangle<int> getDisplayBounds() const { return lcd.getBounds(); }

    /** Every control MIDI learn can map, with what it is. */
    void forEachLearnTarget (const std::function<void (juce::Component&, MidiMapper::Control)>& fn);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    AudioEngine& engine;
    const int t;

    LcdButton lcd;
    LabeledKnob gain, high, mid, freq, low, pan;
    Meter meter;
    Fader fader;
    LedKey mute { "MUTE", juce::Colour (0xffff8a3d) }, solo { "SOLO", juce::Colour (0xffffe27a) }, arm { "ARM", juce::Colour (0xffff4a32) };
    juce::String tag, shownTag;
    juce::Colour tagColour, shownTagColour;
    int midiHold = 0;
    juce::uint32 shownVersion = 0;   // the engine's viewVersion the display text was built from
    int framesSinceText = 0;
    void rebuildText();
};

class MasterStrip : public juce::Component
{
public:
    explicit MasterStrip (AudioEngine& engine);

    /** formats: what a mixdown writes ("WAV + MP3"); blink: the deck's blink phase. */
    void refresh (const juce::String& formats, bool blink);
    void syncFromEngine();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void forEachLearnTarget (const std::function<void (juce::Component&, MidiMapper::Control)>& fn);

    std::function<void()> onMasterDeck, onSide, onMixdown;   // the display, the SIDE key, the MIXDOWN key

private:
    AudioEngine& engine;
    LabeledKnob phones;
    LcdButton display;
    LedKey side { "SIDE A", Pal::accent }, mixdown { "MIXDOWN", Pal::red };
    Meter meter { 2 };
    Fader fader;
    juce::Rectangle<int> peakArea;
    juce::String peakText = "-INF  -INF";
};
} // namespace tf
