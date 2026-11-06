#pragma once

#include "Nymphes.h"
#include "InstrumentWidgets.h"

namespace tf
{
/**
    TF NYMPHES's window: every control of a Dreadbox Nymphes as its own knob, in the synth's sections
    (oscillator, filter, reverb and amp, the two envelopes as graphs to drag, the two LFOs drawn),
    MPE, what's playing, and the modulation matrix: four sources by 36 destinations.
*/
class NymphesPanel : public juce::Component,
                     private juce::Timer
{
public:
    explicit NymphesPanel (nym::Processor&);
    ~NymphesPanel() override;

    static constexpr int panelWidth = 1212, panelHeight = 880;

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void (const juce::String&)> onMessage;

    class Knob;
    class Envelope;
    class Matrix;

private:
    void timerCallback() override;
    Knob& knob (int control, juce::Point<int> centre, int radius = 17);
    LcdChip& chip (const juce::String& label, std::function<bool()> lit, std::function<void()> click);
    void refresh();

    nym::Processor& syn;
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<LcdChip> chips;
    std::vector<std::function<bool()>> chipLit;
    std::unique_ptr<Envelope> filterEg, ampEg;
    std::unique_ptr<Matrix> matrix;
    nym::State shown;
    int shownSent = -2;
    juce::String voicesShown;

    // the sections
    juce::Rectangle<int> oscArea, filterArea, reverbArea, envArea, lfo1Area, lfo2Area, mpeArea, matrixArea;
    juce::Rectangle<int> waveView, filterView, reverbView, lfo1View, lfo2View, voicesView;
};
} // namespace tf
