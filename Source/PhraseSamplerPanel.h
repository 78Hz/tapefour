#pragma once

#include "PhraseSampler.h"
#include "InstrumentWidgets.h"

namespace tf
{
/**
    The phrase sampler's window: PLAY is its front panel (the LCD, the ribbon and its effect keys, the
    12 pads and four banks, the song keys), played with the mouse; SAMPLE, EDIT, SONGS and SETUP do
    what the original does through its menus.
*/
class PhraseSamplerPanel : public juce::Component,
                           private juce::Timer
{
public:
    PhraseSamplerPanel (sampler::Processor&, int& tab);
    ~PhraseSamplerPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void (const juce::String&)> onMessage;

    class Front;
    class Wave;
    class ValueBar;

private:
    void timerCallback() override;
    void build();
    iw::Chips& chips (const juce::String& caption, juce::StringArray labels, std::function<int()> chosen, std::function<void (int)> pick);
    iw::Chips& paramChips (const juce::String& id, const juce::String& caption, juce::StringArray labels);
    iw::Bar& bar (const juce::String& id, const juce::String& caption, std::function<juce::String (float)> fmt);
    LcdChip& button (const juce::String& label, std::function<void()> click);
    void note (juce::Rectangle<int> r, const juce::String& t) { notes.push_back ({ r, t }); }
    void say (const juce::String& m) { if (onMessage) onMessage (m); }
    void importFile();
    void choosePad (const juce::String& what, std::function<void (int)> then);
    void setRecord (std::function<void (sampler::Processor::RecordSettings&)> change);

    sampler::Processor& sampler;
    int& tab;
    juce::OwnedArray<LcdChip> tabChips;
    juce::OwnedArray<juce::Component> parts;
    std::vector<iw::Chips*> chipRows;
    std::vector<juce::Component*> live;   // repainted by the timer
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> notes;
    juce::Rectangle<int> meterArea, memoryArea, headArea, songArea;
    LcdChip* recordButton = nullptr;
    std::unique_ptr<juce::FileChooser> chooser;
    int builtTab = -1, builtPad = -1, builtPadFrames = -1;
    float meterShown = 0.0f, meterHeld = 0.0f;
    int meterHold = 0;
    double deleteArmedUntil = 0.0;
    int deleteArmedWhat = -1;
};
} // namespace tf
