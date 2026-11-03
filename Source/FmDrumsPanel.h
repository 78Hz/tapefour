#pragma once

#include "FmDrums.h"
#include "InstrumentWidgets.h"

namespace tf
{
/**
    TF FM DRUMS' window. PATTERN: the transport (PLAY, tempo, swing), the 16 patterns, the six tracks'
    steps (click a step to set it; right-click, or alt-click, to lock its sound), and the selected track's
    sound, or the selected step's locks. FX: the delay, the reverb and the master level.
*/
class FmDrumsPanel : public juce::Component,
                     private juce::Timer
{
public:
    explicit FmDrumsPanel (fmd::Processor&);
    ~FmDrumsPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void (const juce::String&)> onMessage;

    class Grid;
    class Field;

private:
    void timerCallback() override;
    void build();
    void buildSoon();
    void edited() { synth.changed(); }
    void say (const juce::String& m) { if (onMessage) onMessage (m); }
    iw::Chips& chips (const juce::String& caption, juce::StringArray labels, std::function<int()> chosen, std::function<void (int)> pick);
    Field& field (int param, juce::Rectangle<int> r);
    Field& setting (const juce::String& caption, int lo, int hi, std::function<int()> get, std::function<void (int)> set,
                    std::function<juce::String (int)> text, juce::Rectangle<int> r);
    LcdChip& button (const juce::String& label, std::function<void()> click);
    void note (juce::Rectangle<int> r, const juce::String& t) { notes.push_back ({ r, t }); }
    fmd::Pattern& pattern() { return synth.state().patterns[(size_t) std::clamp (synth.state().pattern, 0, fmd::numPatterns - 1)]; }
    fmd::Step* lockedStep();

    fmd::Processor& synth;
    juce::OwnedArray<LcdChip> tabChips;
    juce::OwnedArray<juce::Component> parts;
    std::vector<iw::Chips*> chipRows;
    std::vector<std::pair<iw::Chips*, std::function<bool (int)>>> dimRows;
    std::vector<juce::Component*> live;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> notes;
    juce::Rectangle<int> headArea;
    LcdChip* playChip = nullptr;
    bool rebuildPending = false;
    int builtTab = -1, builtTrack = -1, builtStep = -1, builtMachine = -1;
};
} // namespace tf
