#pragma once

#include "ElectricPiano.h"
#include "InstrumentWidgets.h"

namespace tf
{
/**
    The electric piano's editor, in the signal chain's lower half: eight tabs, as a technician
    works on the instrument (MODEL, TUNING, REEDS, HAMMERS, PICKUP, DAMPERS, AMP, PLAYING).
    Every control is one of the piano's parameters, so LEARN and the deck's saving see them.
*/
class ElectricPianoPanel : public juce::Component,
                           private juce::Timer
{
public:
    ElectricPianoPanel (ep::Processor&, int& tab);
    ~ElectricPianoPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    std::function<void (const juce::String&)> onMessage;

    using Bar = iw::Bar;
    using Chips = iw::Chips;

private:
    void timerCallback() override;
    void build();
    Bar& bar (const juce::String& id, const juce::String& caption, std::function<juce::String (float)> fmt);
    Chips& chips (const juce::String& caption, juce::StringArray labels, std::function<int()> chosen, std::function<void (int)> pick);
    Chips& paramChips (const juce::String& id, const juce::String& caption, juce::StringArray labels);
    void note (juce::Rectangle<int> r, const juce::String& text) { notes.push_back ({ r, text }); }
    juce::Rectangle<int> content() const;
    void loadScale (bool keyboardMap);
    void setCustomCents (int pitchClass, float cents);

    ep::Processor& piano;
    int& tab;
    juce::OwnedArray<LcdChip> tabChips;
    juce::OwnedArray<juce::Component> parts;
    std::vector<Bar*> bars;
    std::vector<Chips*> chipRows;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> notes;
    juce::Rectangle<int> chartArea, diagramArea, traceArea, summaryArea;
    std::unique_ptr<juce::TextEditor> edoBox;
    std::unique_ptr<juce::FileChooser> chooser;
    int builtTab = -1, builtSystem = -1;
    int dragPitchClass = -1;
    juce::String lastSummary;
};

} // namespace tf
