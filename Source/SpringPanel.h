#pragma once

#include "SpringReverb.h"
#include "InstrumentWidgets.h"

namespace tf
{
/**
    TF SPRING REVERB's window: the tank (which springs, from above), TANK, MODEL, DWELL and TONE, and what
    a click sounds like through it (frequency over time: the chirps, and the echoes coming round).
*/
class SpringEditor : public juce::AudioProcessorEditor,
                     private juce::Timer
{
public:
    explicit SpringEditor (spring::Processor&);
    ~SpringEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int width = 860, height = 430;
    /** Brings the picture and controls up to date now (the window's timer does it as it runs). */
    void refreshForSnapshot() { timerCallback(); }

private:
    void timerCallback() override;
    void makePicture();
    void paintTank (juce::Graphics&, juce::Rectangle<int>);

    spring::Processor& proc;
    juce::OwnedArray<juce::Component> parts;
    std::vector<iw::Chips*> chipRows;
    std::vector<iw::Bar*> bars;
    juce::Rectangle<int> tankArea, pictureArea;
    juce::Image picture;
    int pictureFor = -1;
};
} // namespace tf
