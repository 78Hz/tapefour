#pragma once

#include "PresetPanel.h"

namespace tf
{
/**
    A built-in instrument's editor, in a window of its own like a plug-in's: its preset (click for the
    list) along the top, and its own panel underneath, so the deck's mixer stays in reach.
*/
class InstrumentEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    using Say = std::function<void (const juce::String&)>;
    /** makePanel: the instrument's own panel; it can show a message in the header with the Say it's given. */
    InstrumentEditor (juce::AudioProcessor&, presets::Presettable&, const juce::String& title,
                      std::function<std::unique_ptr<juce::Component> (Say)> makePanel);
    ~InstrumentEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int width = 860, height = 390;

private:
    void showPresets (bool);
    void say (const juce::String&);
    void timerCallback() override { message.clear(); stopTimer(); repaint(); }

    presets::Presettable& inst;
    juce::String title;
    std::function<std::unique_ptr<juce::Component> (Say)> makePanel;
    juce::String message;
    PresetDisplay display;
    std::unique_ptr<juce::Component> panel;
    std::unique_ptr<PresetList> list;
};
} // namespace tf
