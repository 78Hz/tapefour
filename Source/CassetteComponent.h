#pragma once

#include "Widgets.h"
#include "Tape.h"

namespace tf
{
/**
    A compact cassette, drawn in its own shell design for each tape model
    (BASIC, SD, XR, MX). Geometry is worked in tenths of a millimetre
    (the shell is 100.4 x 63.8 mm, so 1004 x 638 units) and scaled to fit.

    Spool sizes follow how much tape is wound on each; the tape leaves each
    pack on the true tangent to the corner guide roller; spools turn at the
    speed a real cassette's would for the current tape travel.
*/
class CassetteComponent : public juce::Component
{
public:
    CassetteComponent();

    /** Call ~30 times a second. */
    void update (const Tape* tape, double positionCells);

    std::function<void (const juce::String&)> onNameChanged;
    void setTape (const Tape* tape);
    /** Which side faces up (0 A, 1 B): the letter printed on the label. */
    void setSide (int side);
    /** False: the label can't be written on here (it's shown, not edited). */
    void setLabelEditable (bool);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    float fraction = 0.0f, fullPackUnits = 440.0f;
    double lastPos = -1.0;
    const Tape* lastTape = nullptr;
    float angleL = 0.0f, angleR = 0.0f;
    bool hasTape = false;
    int model = defaultTapeModel, minutes = 30, side = 0;
    const char* sideLetter() const noexcept { return side == 0 ? "A" : "B"; }
    juce::Label nameLabel;

    float packDiameter (float f) const;
    void drawPack (juce::Graphics&, juce::Point<float> c, float d) const;
    void drawHub (juce::Graphics&, juce::Point<float> c, float angle) const;
    void drawPrint (juce::Graphics&) const;
};
} // namespace tf
