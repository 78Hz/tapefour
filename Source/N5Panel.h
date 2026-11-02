#pragma once

#include "N5Synth.h"
#include "InstrumentWidgets.h"

namespace tf
{
/**
    TF N5's window: the program (or combination) at the top, then OSC, PITCH, VDF, VDA, CONTROL, WAVES,
    EFFECTS, COMBI and ARP, as the N5's own edit pages group them.
*/
class N5Panel : public juce::Component,
                private juce::Timer
{
public:
    N5Panel (n5::Processor&);
    ~N5Panel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void (const juce::String&)> onMessage;

    class FieldRow;
    class EnvelopeGraph;
    class WaveCanvas;
    class EffectRow;
    class TimbreTable;

private:
    void timerCallback() override;
    void build();
    /** Builds the page again once the click that asked for it is over (building deletes the controls). */
    void buildSoon();
    bool rebuildPending = false;
    void say (const juce::String& m) { if (onMessage) onMessage (m); }
    void edited();

    n5::ProgramData& program();
    n5::FxSettings& fx();
    int* layerValues();

    FieldRow& layerRow (int field, juce::Rectangle<int> r);
    FieldRow& globalRow (int field, juce::Rectangle<int> r);
    FieldRow& arpRow (int field, juce::Rectangle<int> r);
    LcdChip& button (const juce::String& label, std::function<void()> click);
    void note (juce::Rectangle<int> r, const juce::String& t) { notes.push_back ({ r, t }); }
    void chooseProgram();
    void rename();
    void chooseEffect (int unit);
    void drawnTool (int which);

    n5::Processor& synth;
    juce::OwnedArray<LcdChip> tabChips;
    juce::OwnedArray<juce::Component> parts;
    std::vector<juce::Component*> live;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> notes;
    juce::Rectangle<int> diagramArea, harmonicsArea;
    std::unique_ptr<juce::TextEditor> nameEditor;
    int builtTab = -1, builtLayer = -1, builtMode = -1, builtOsc = -1;
};
} // namespace tf
