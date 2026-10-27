#pragma once

#include "Common.h"
#include "../Formats/Look.h"
#include "../Formats/Cassette.h"

namespace tf
{
/** A cassette's look, ready to draw: its description and its images (docs/formats/cass-v1.md, "The look"). */
struct CassetteArtwork
{
    tfmt::Look look;
    juce::Image back, frontA, frontB, hub;

    bool isValid() const noexcept { return back.isValid() && frontA.isValid() && hub.isValid(); }
    /** True if the shell's images are empty (fully transparent): the TF-4 0.2 on Windows packed looks like that. */
    bool isBlank() const;

    /** The look a packed cassette carries; invalid if it carries none (or one that can't be read). */
    static CassetteArtwork fromPacked (const tfmt::PackedCassette&);
    /** The look in an unpacked cassette's folder. */
    static CassetteArtwork fromFolder (const juce::File& cassetteFolder);
    /** From a look's description and its PNG files' bytes, by name. */
    static CassetteArtwork fromParts (const tfmt::Look&, const std::function<juce::MemoryBlock (const std::string& name)>& readFile);
};

/**
    Draws a cassette from its look, as any player of the format does: the shell behind the works,
    the tape packs, the tape round the guides, the turning hubs, and the front of the side facing up.
    Scales to fit, keeping the shell's proportions; the images are resized once per size, so it is
    cheap to animate.
*/
class LookView : public juce::Component
{
public:
    void setArtwork (CassetteArtwork);
    const CassetteArtwork& getArtwork() const noexcept { return art; }
    /** 0: side A faces up, 1: side B. */
    void setSide (int side);
    /** How much of the side has played (0 at its start: the tape all on the left hub). Call ~30 times a second;
        the hubs turn by the tape that passed since the last call. */
    void setPlayed (double fraction, double tapeSecondsMoved);

    /** Where the shell lies in the component. */
    juce::Rectangle<float> shellBounds() const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    CassetteArtwork art;
    int side = 0;
    double fraction = 0.0;
    float angleL = 0.0f, angleR = 0.0f;

    // the images at the size they're drawn
    juce::Image back, front, hub;
    float drawnScale = 0.0f;   // pixels per tenth of a mm the images above were made for
    void rescale();
};
} // namespace tf
