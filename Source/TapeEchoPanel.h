#pragma once

#include "TapeEcho.h"
#include "InstrumentWidgets.h"

namespace tf
{
/**
    TF TAPE ECHO's window: the tape path (the whole loop, and a strip of it to zoom along and drag the
    heads on), RATE with the tempo it makes, the echo's tone, the loop cassette, the loop's length and
    switches, and what's on the loop now.
*/
class TapeEchoPanel : public juce::Component,
                      private juce::Timer
{
public:
    explicit TapeEchoPanel (echo::Processor&);
    ~TapeEchoPanel() override;

    static constexpr int panelWidth = 832, panelHeight = 540;

    void paint (juce::Graphics&) override;
    void resized() override;
    std::function<void (const juce::String&)> onMessage;

    class Knob;
    class TapePath;
    class Nudge;

private:
    void timerCallback() override;
    LcdChip& chip (const juce::String& label, std::function<bool()> lit, std::function<void()> click, float size = 9.5f);
    Knob& knob (const juce::String& id, const juce::String& label, std::function<juce::String (float)> fmt, int radius = 17);
    void refresh();

    echo::Processor& proc;
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<LcdChip> chips;
    std::vector<std::function<bool()>> chipLit;
    std::unique_ptr<TapePath> path;
    juce::OwnedArray<Nudge> nudges;
    LcdChip* holdChip = nullptr;
    juce::Rectangle<int> pathArea, rateArea, echoArea, tapeArea, loopArea, nowArea, vuArea;
    float vuDb = -60.0f;
    juce::String shown;
};
} // namespace tf
