#pragma once

#include "StripComponent.h"

namespace tf
{
/** Dims the deck and holds a card; clicking outside the card (or Escape) closes it. */
class Overlay : public juce::Component
{
public:
    Overlay (std::unique_ptr<juce::Component> content, juce::Rectangle<int> cardBounds);
    std::function<void()> onDismiss;
    bool cardShadow = true;   // off for cards that cast their own

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    juce::Component* getCard() const noexcept { return card.get(); }

private:
    std::unique_ptr<juce::Component> card;
};

/**
    A track's patch: what's plugged in (audio input, instrument, hardware synth
    or bounce), which controller plays it, where its MIDI goes and which input
    the audio comes back on.
*/
class PatchCard : public juce::Component
{
public:
    PatchCard (AudioEngine& engine, int track);

    std::function<void()> onDone;
    std::function<void (int track)> onShowPluginWindow, onClosePluginWindow;
    std::function<void()> onFindInstruments;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row
    {
        juce::String label, sub;
        int first = 0, count = 0;
        juce::Component* extra = nullptr;
        int y = 0, height = 0;
    };

    void rebuild();
    void addRow (const juce::String& label, const juce::String& sub);
    ChipButton* addChip (const juce::String& text, bool chosen, std::function<void()> action);
    void changed();

    AudioEngine& engine;
    const int t;

    juce::OwnedArray<ChipButton> sourceCards, chips;
    std::vector<Row> rows;
    juce::TextEditor channelBox;
    juce::Label channelLabel;
    FlatButton done { "DONE" }, closeX { "X" };
    TrackLines preview;
};
} // namespace tf
