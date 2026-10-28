#pragma once

#include "TransistorOrgan.h"
#include "InstrumentWidgets.h"

namespace tf
{
/**
    The transistor organ's editor, in the signal chain's lower half: the organ's own panel (six drawbars
    and the vibrato switch), then what's inside the case: TUNING, GENERATORS, KEYS, VIBRATO.
*/
class TransistorPanel : public juce::Component,
                        private juce::Timer
{
public:
    TransistorPanel (organ::Processor&, int& tab);
    ~TransistorPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    std::function<void (const juce::String&)> onMessage;

    class Drawbars;

private:
    void timerCallback() override;
    void build();
    iw::Bar& bar (const juce::String& id, const juce::String& caption, std::function<juce::String (float)> fmt);
    iw::Chips& chips (const juce::String& caption, juce::StringArray labels, std::function<int()> chosen, std::function<void (int)> pick);
    iw::Chips& paramChips (const juce::String& id, const juce::String& caption, juce::StringArray labels);
    void note (juce::Rectangle<int> r, const juce::String& t) { notes.push_back ({ r, t }); }
    void loadScale();

    organ::Processor& organ;
    int& tab;
    juce::OwnedArray<LcdChip> tabChips;
    juce::OwnedArray<juce::Component> parts;
    std::vector<iw::Bar*> bars;
    std::vector<iw::Chips*> chipRows;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> notes;
    juce::Rectangle<int> soundArea, chartArea, driftArea, traceArea, contactArea, wobbleArea;
    std::unique_ptr<juce::FileChooser> chooser;
    int builtTab = -1, builtSystem = -1, dragPitchClass = -1;
};
} // namespace tf
