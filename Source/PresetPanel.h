#pragma once

#include "InstrumentPresets.h"
#include "Widgets.h"

namespace tf
{
//==============================================================================
/** An instrument's preset, in the chain panel's header: its name (* when changed since); click for the list. */
class PresetDisplay : public juce::Component,
                      private juce::Timer
{
public:
    explicit PresetDisplay (presets::Presettable&);
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override { if (onClick) onClick(); }

    bool open = false;
    std::function<void()> onClick;

private:
    void timerCallback() override;
    juce::String label() const;
    presets::Presettable& inst;
    juce::String shown;
};

/** The presets list, in place of the editor: save, save as new, rename, delete, show the folder; click one to load it. */
class PresetList : public juce::Component
{
public:
    explicit PresetList (presets::Presettable&);
    ~PresetList() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

    std::function<void()> onLoaded;                       // a preset was loaded: back to the editor
    std::function<void (const juce::String&)> onMessage;

private:
    enum class Mode { list, naming, renaming, deleting };
    void refresh();
    void setMode (Mode);
    void commitName();
    void layout();
    int rowAt (juce::Point<int>) const;

    presets::Presettable& inst;
    Mode mode = Mode::list;
    std::vector<presets::Entry> entries;
    juce::OwnedArray<LcdChip> chips;          // the top row
    juce::OwnedArray<LcdChip> lineChips;      // the naming / deleting row's own
    juce::TextEditor nameBox;
    juce::String error;
    juce::Rectangle<int> lineArea, listArea, headArea;
    int scroll = 0, hover = -1;
    static constexpr int rowH = 30;
};
} // namespace tf
