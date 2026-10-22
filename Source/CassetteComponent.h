#pragma once

#include "Widgets.h"
#include "Tape.h"

namespace tf
{
/**
    A clear-shell compact cassette drawn at 3.98 px/mm (400 x 255 = 100.4 x 63.8 mm).
    Spool sizes follow how much tape is wound on each; the tape leaves each pack
    on the true tangent to the corner guide roller; spools turn at the speed a
    real cassette's would for the current tape travel.
*/
class CassetteComponent : public juce::Component
{
public:
    CassetteComponent();

    /** Call ~30 times a second. */
    void update (const Tape* tape, double positionCells);

    std::function<void (const juce::String&)> onNameChanged;
    void setTapeName (const juce::String& name, const juce::String& typeName);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    float fraction = 0.0f, fullPackPx = 175.0f;
    double lastPos = -1.0;
    float angleL = 0.0f, angleR = 0.0f;
    bool hasTape = false;
    juce::String typeName;
    juce::Label nameLabel;

    float packDiameter (float f) const;
    void drawPack (juce::Graphics&, juce::Point<float> c, float d) const;
    void drawHub (juce::Graphics&, juce::Point<float> c, float angle) const;
};
} // namespace tf
