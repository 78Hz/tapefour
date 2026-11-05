#pragma once

#include "StripComponent.h"

namespace tf
{
class BlockPicker;
class ParamPicker;

/**
    A track's signal chain, dropped down from its display: one row of blocks with
    MIDI running along the top and audio along the bottom, and a status line.
    The first block is the track's inputs (its controller and its audio input, or
    a bounce of other tracks); the last is the tape.

    On a block: its lamp bypasses it, double-click opens its window, drag moves it,
    the × at its corner unloads it (twice: the first click arms it) and leaves an
    empty slot, the MIDI badge opens MIDI learn, the line beside it is its LEVEL or
    MIX. The card grows a panel underneath only for what needs one: the inputs,
    "+" and an empty slot (what to load), MIDI learn, and an external synth's routing.
*/
class ChainEditor : public juce::Component,
                    public Theme::Aware,
                    private juce::Timer
{
public:
    void themeChanged (const std::function<juce::Colour (juce::Colour)>& recolour) override
    {
        for (auto& c : captions) c.colour = recolour (c.colour);
        layoutFlow();
    }

    static constexpr int editorWidth = 840, editorHeight = 480, notchHeight = 10;
    static constexpr int compactHeight = 176;   // the chain and its status line, nothing underneath

    ChainEditor (AudioEngine& engine, int track, int notchX);
    ~ChainEditor() override;

    std::function<void()> onClose;
    std::function<void (int track, int blockId)> onEditBlock;
    std::function<void()> onFindPlugins;
    std::function<void (const juce::String&)> onMessage;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class Sel { none, inputs, block, add };
    struct Cell
    {
        juce::Rectangle<int> node;
        int blockId = 0;            // 0: not a block
        bool inputs = false, add = false, tape = false, empty = false;
        // a block's own controls: the × at its corner, the MIDI badge, the LEVEL / MIX line
        juce::Point<int> cross;
        bool hasBadge = false, hasSlider = false, isMix = false, wide = false;
        int midiCount = 0;
        juce::Rectangle<int> badge, slider, value, audioTagArea;
        // lanes and ticks
        juce::Colour midiL, midiR, audioL, audioR, midiTick, audioTick;
        juce::String midiTag, audioTag;
    };
    struct Caption { juce::String text; juce::Rectangle<int> area; juce::Colour colour; };

    void timerCallback() override;
    juce::String signature() const;
    void rebuild();
    void layoutFlow();
    void clearPanel();
    void buildInputsPanel (juce::Rectangle<int> area);
    void buildLearnPanel (const AudioEngine::BlockInfo&, juce::Rectangle<int> area);
    void buildExternalPanel (const AudioEngine::BlockInfo&, juce::Rectangle<int> area);
    void buildPickerPanel (juce::Rectangle<int> area, int slotId);
    bool hasPanel() const;
    juce::String statusText (juce::Colour& colour) const;
    void crossClicked (int blockId);
    void setNodeValue (const Cell&, int mouseX);
    void resetNodeValue (const Cell&);
    const AudioEngine::BlockInfo* infoFor (int blockId) const;
    int slotNumber (int blockId) const;
    void editBlock (int blockId);
    LcdChip* chip (const juce::String& text, bool chosen, std::function<void()> action, const juce::String& sub = {});
    int chipRow (int y, int x, int right, const juce::String& caption, const std::vector<LcdChip*>& chips, int chipH = 30, juce::Colour captionColour = Pal::lcdDim);
    const AudioEngine::BlockInfo* selected() const;
    int cellAt (juce::Point<int>) const;
    void changed();
    void pickParameter (int index);

    AudioEngine& engine;
    const int t, notchX;

    Sel sel = Sel::none;
    int selId = 0;
    std::vector<AudioEngine::BlockInfo> chain;
    std::vector<Cell> cells;
    juce::String lastSignature;

    juce::OwnedArray<juce::Component> panel;
    std::vector<Caption> captions;
    std::vector<std::pair<LcdChip*, int>> inputChips;   // chip, interface input (for the signal bars)
    std::unique_ptr<BlockPicker> picker;
    std::unique_ptr<ParamPicker> params;
    LcdChip closeX { "X" };

    bool learning = false;            // the MIDI badge: learning the selected block's controls
    int armedX = 0;                   // the block whose × was clicked once
    int levelDragId = 0;              // the block whose LEVEL / MIX line is being dragged
    bool downHandled = false;         // the mouse-down did its job (a badge, a line, a tag)
    std::map<int, float> sendShown;                          // block id: what it sends, as shown on its node
    int learnParam = -1;

    int dragId = 0, dragX = 0, dropIndex = -1;
    bool dragging = false;
};
} // namespace tf
