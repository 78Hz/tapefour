#pragma once

#include "Widgets.h"
#include "Tape.h"
#include "../Formats/Look.h"

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

    /** A cassette's look as it is saved with it (art/look.json and its images): what any player
        needs to draw this cassette exactly as the TF-4 does, reels turning. */
    struct Art
    {
        tfmt::Look look;
        juce::MemoryBlock back, frontA, frontB, hub;   // PNG
    };
    /** What a look is drawn from: copied from a Tape, so it can be drawn on another thread. */
    struct ArtSpec
    {
        int model = defaultTapeModel, minutes = 30;
        float fullPackMm = 44.0f;
        juce::String label;
        static ArtSpec of (const Tape&);
        juce::String key() const;   // changes whenever the look would
    };
    static Art renderArt (const ArtSpec&);
    /** The same look as images, not PNG files: for drawing it straight away (a player). */
    struct ArtImages
    {
        tfmt::Look look;
        juce::Image back, frontA, frontB, hub;
    };
    static ArtImages drawArt (const ArtSpec&);
    /** The cassette's folder already holds its look as this version draws it, for its label as it is now. */
    static bool artIsCurrent (const ArtSpec&, const juce::File& cassetteFolder);
    /** Writes the look into the cassette's folder (art/), each file replaced whole. Any thread. */
    static bool writeArt (const ArtSpec&, const juce::File& cassetteFolder);

private:
    float fraction = 0.0f, fullPackUnits = 440.0f;
    double lastPos = -1.0;
    const Tape* lastTape = nullptr;
    float angleL = 0.0f, angleR = 0.0f, stepL = 0.0f, stepR = 0.0f;
    bool hasTape = false;
    int model = defaultTapeModel, minutes = 30, side = 0;
    const char* sideLetter() const noexcept { return side == 0 ? "A" : "B"; }
    juce::Label nameLabel;

    float packDiameter (float f) const;
    void drawPack (juce::Graphics&, juce::Point<float> c, float d) const;
    void drawHub (juce::Graphics&, juce::Point<float> c, float angle) const;
    void drawPrint (juce::Graphics&) const;
    // The layers, back to front (between them: the packs, the tape and the hubs, in paintWorks).
    void paintBack (juce::Graphics&) const;
    void paintWorks (juce::Graphics&) const;
    void paintFront (juce::Graphics&, const juce::String& writtenLabel) const;
};
} // namespace tf
