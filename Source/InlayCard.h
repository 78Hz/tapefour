#pragma once

#include "Widgets.h"
#include "Tape.h"

namespace tf
{
/** The J-card, unfolded: back flap, spine and front. Everything on it is saved with the tape. */
class InlayCard : public juce::Component
{
public:
    static constexpr int flapW = 112, spineW = 92, frontW = 520, cardH = 760, closeW = 52, gap = 18;
    static constexpr int totalW = flapW + spineW + frontW + gap + closeW;

    explicit InlayCard (Tape& tape);

    std::function<void()> onClose;
    std::function<void (const juce::String& title)> onTitleChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::TextEditor* makeEditor (int field, juce::Font font, juce::Colour colour);
    void store (int field, const juce::String& text);

    Tape& tape;
    juce::OwnedArray<juce::TextEditor> editors;
    juce::TextEditor* title = nullptr;
    juce::TextEditor* counters[Inlay::numSongs] {};
    juce::TextEditor* songs[Inlay::numSongs] {};
    juce::TextEditor* trackNotes[numTracks] {};
    juce::TextEditor* speed = nullptr;
    juce::TextEditor* date = nullptr;
    ChipButton nrOn { "ON" }, nrOff { "OFF" };
    LedKey closeX { "X", {}, false };
};

/** The inlay standing in its case beside the cassette. Click to take it out. */
class InlayThumb : public juce::Button
{
public:
    InlayThumb() : juce::Button ("inlay") {}
    const Tape* tape = nullptr;
    void paintButton (juce::Graphics&, bool over, bool down) override;
};
} // namespace tf
