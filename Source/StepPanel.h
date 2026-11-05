#pragma once

#include "StepSeq.h"
#include "InstrumentWidgets.h"

namespace tf
{
/**
    TF STEP's window: PLAY, the cycle's length, LENGTH, the keyboard (TRANSPOSE from a home note, or
    OFF), STEP TIMES ×½ / ×2; the 16 steps, a pitch row (drag; below the lowest note: a rest; shift-
    click: a tie) over a time row (drag; shift-drag: the gate); and the selected step's four settings.
*/
class StepPanel : public juce::Component,
                  private juce::Timer
{
public:
    explicit StepPanel (step::Processor&);
    ~StepPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    std::function<void (const juce::String&)> onMessage;

    class Steps;
    class Field;

private:
    void timerCallback() override;
    void edited();
    void layoutFields();

    step::Processor& seq;
    LcdChip play { "PLAY" }, transposeChip { "TRANSPOSE" }, keysOff { "OFF" }, homeDown { "-" }, homeUp { "+" }, halve { utf ("\xc3\x97\xc2\xbd") }, twice { utf ("\xc3\x97" "2") };
    juce::OwnedArray<LcdChip> lengthChips;
    std::unique_ptr<Steps> steps;
    juce::OwnedArray<Field> fields;
    juce::Rectangle<int> cycleArea, keysArea, timesArea, helpArea, selArea, chainArea;
};
} // namespace tf
