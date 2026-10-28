#pragma once

#include "StripComponent.h"
#include "ElectricPianoPanel.h"
#include "PresetPanel.h"
#include "TransistorPanel.h"

namespace tf
{
class BlockPicker;
class ParamPicker;

/**
    A track's signal chain, dropped down from its display: one row of blocks with
    MIDI running along the top and audio along the bottom, then whatever is
    selected underneath. The first block is the track's inputs (its controller and
    its audio input, or a bounce of other tracks); the last is the tape.

    Click a block to select it, its lamp to bypass it, drag it to move it,
    double-click it to open its window. "+" adds a block.
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
    enum class Sel { inputs, block, add };
    struct Cell
    {
        juce::Rectangle<int> node;
        int blockId = 0;            // 0: not a block
        bool inputs = false, add = false, tape = false;
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
    void buildBlockPanel (const AudioEngine::BlockInfo&, juce::Rectangle<int> area);
    void buildExternalPanel (const AudioEngine::BlockInfo&, juce::Rectangle<int> area);
    void buildPickerPanel (juce::Rectangle<int> area, bool replacing);
    void buildInstrumentPanel (const AudioEngine::BlockInfo&, juce::Rectangle<int> area, presets::Presettable&,
                               std::function<std::unique_ptr<juce::Component>()> makeEditor);
    static bool hasPanelEditor (Block*);
    void editBlock (int blockId);
    LcdChip* chip (const juce::String& text, bool chosen, std::function<void()> action, const juce::String& sub = {});
    int chipRow (int y, int x, int right, const juce::String& caption, const std::vector<LcdChip*>& chips, int chipH = 30, juce::Colour captionColour = Pal::lcdDim);
    const AudioEngine::BlockInfo* selected() const;
    int cellAt (juce::Point<int>) const;
    void changed();
    void pickParameter (int index);

    AudioEngine& engine;
    const int t, notchX;

    Sel sel = Sel::inputs;
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

    bool learning = false;            // LEARN: picking a parameter of the selected block
    bool instrumentEditing = false;   // a built-in instrument's editor fills the lower half
    int pianoTab = 0;
    int organTab = 0;
    std::unique_ptr<juce::Component> instrumentPanel;   // a built-in instrument's editor
    std::unique_ptr<PresetDisplay> presetDisplay;
    std::unique_ptr<PresetList> presetList;
    bool presetsOpen = false;         // the presets list in place of the piano's editor
    int learnParam = -1;

    int dragId = 0, dragX = 0, dropIndex = -1;
    bool dragging = false;
};
} // namespace tf
