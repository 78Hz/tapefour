#pragma once

#include "AudioEngine.h"
#include "Widgets.h"

namespace tf
{
/**
    The patch bay: every source a row (the interface's inputs, each instrument's outputs, each track's
    chain out), the four tracks columns, and a point where they cross. The same routing as an
    instrument's OUTS, a track's inputs and its tape block, seen all at once.

      a filled dot      sent to that track
      a ring            in the source's own column: it stays on its own track too (dim: moved away)
      a filled square   the track's audio in (a track takes one interface input)
      struck through    not allowed: the track itself, a loop, or a bounce
    A row per track for its right side makes (or undoes) a stereo pair.
*/
class PatchBay : public juce::Component,
                 private juce::Timer
{
public:
    static constexpr int cardWidth = 840, maxHeight = 800;

    explicit PatchBay (AudioEngine& engine);

    std::function<void()> onClose;
    std::function<void (const juce::String&)> onMessage;

    /** How tall the card wants to be for what's in it now (up to maxHeight; past that it scrolls). */
    int wantedHeight() const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class Kind { heading, input, output, chainOut, rightSide };
    struct Row
    {
        Kind kind = Kind::heading;
        juce::String name, sub;
        int input = -1; bool pair = false;      // an interface input (or a pair, summed)
        AudioEngine::RouteSource src;           // an instrument's output, or a chain out
    };
    enum class Point { none, open, sent, home, homeAway, audioIn, barred };

    void timerCallback() override;
    juce::String signature() const;
    void rebuild();
    Point pointAt (const Row&, int track, juce::String* why = nullptr) const;
    void click (const Row&, int track);
    juce::String describe (const Row&) const;
    int rowAt (juce::Point<int>) const;
    int columnAt (juce::Point<int>) const;
    juce::Rectangle<int> rowsArea() const;

    AudioEngine& engine;
    std::vector<Row> rows;
    juce::String lastSignature;
    int scroll = 0, hoverRow = -1;
    LcdChip closeX { "X" };
};
} // namespace tf
