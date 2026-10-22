#pragma once

#include "Widgets.h"
#include "AudioEngine.h"

namespace tf
{
/** What a track's display says: source, routing, audio path. Shared with the patch card. */
struct TrackLines
{
    juce::String top, mid, low, tag;
    juce::Colour tagColour;
};
TrackLines describeTrack (const AudioEngine& engine, int t);

/** One channel strip, every control in a single column, top to bottom. */
class StripComponent : public juce::Component
{
public:
    StripComponent (AudioEngine& engine, int index);
    ~StripComponent() override;

    std::function<void (int track, juce::Component* target)> onSourceMenu;

    void refresh (bool blinkPhase);
    void syncFromEngine();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    AudioEngine& engine;
    const int t;

    LcdButton lcd;
    LabeledKnob gain, high, mid, low, pan;
    Meter meter;
    Fader fader;
    FlatButton arm { "REC ARM" };
    juce::String tag;
    juce::Colour tagColour;
    int midiHold = 0;
};

class MasterStrip : public juce::Component
{
public:
    explicit MasterStrip (AudioEngine& engine);

    void refresh();
    void syncFromEngine();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AudioEngine& engine;
    LabeledKnob phones;
    Meter meter { 2 };
    Fader fader;
};
} // namespace tf
