#pragma once

#include "ElectricPiano.h"
#include "Widgets.h"

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

    class Bar;
    class Chips;

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

//==============================================================================
/** The electric piano's preset, in the chain panel's header: its name (* when changed since); click for the list. */
class PianoPresetDisplay : public juce::Component,
                           private juce::Timer
{
public:
    explicit PianoPresetDisplay (ep::Processor&);
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override { if (onClick) onClick(); }

    bool open = false;
    std::function<void()> onClick;

private:
    void timerCallback() override;
    juce::String label() const;
    ep::Processor& piano;
    juce::String shown;
};

/** The presets list, in place of the editor: save, save as new, rename, delete, show the folder; click one to load it. */
class PianoPresets : public juce::Component
{
public:
    explicit PianoPresets (ep::Processor&);
    ~PianoPresets() override;

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

    ep::Processor& piano;
    Mode mode = Mode::list;
    std::vector<ep::presets::Entry> entries;
    juce::OwnedArray<LcdChip> chips;          // the top row
    juce::OwnedArray<LcdChip> lineChips;      // the naming / deleting row's own
    juce::TextEditor nameBox;
    juce::String error;
    juce::Rectangle<int> lineArea, listArea, headArea;
    int scroll = 0, hover = -1;
    static constexpr int rowH = 30;
};
} // namespace tf
