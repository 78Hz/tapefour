#pragma once

#include "AudioEngine.h"
#include "Widgets.h"

namespace tf
{
//==============================================================================
/** Everything that can go in a chain: built-ins, hardware synths, and every scanned plug-in,
    filtered by category and by a search box. Picking one calls onPick (asynchronously, because
    picking usually rebuilds the editor that owns this). */
class BlockPicker : public juce::Component,
                    private juce::ListBoxModel
{
public:
    enum Category { all = 0, instruments, midiFx, audioFx };
    enum class Kind { synth, fx, external, plugin };
    struct Item
    {
        bool header = false;
        juce::String name, sub;
        int category = audioFx;
        Kind kind = Kind::plugin;
        int index = -1;                 // the built-in's number, or the MIDI output's slot
        juce::PluginDescription desc;   // for Kind::plugin
        bool current = false;
    };

    std::function<void (const Item&)> onPick;
    std::function<void()> onFind;

    BlockPicker (AudioEngine& e, const juce::String& currentName, const juce::String& prompt);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    static int categoryOf (const juce::PluginDescription& d);
    void gather (const juce::String& current);
    void refilter();
    void pick (Item it);

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;

    AudioEngine& engine;
    juce::String promptText;
    int category = all;
    juce::OwnedArray<LcdChip> tabs;
    LcdChip find { utf ("FIND PLUG-INS\xe2\x80\xa6") };
    juce::TextEditor search;
    juce::ListBox list;
    std::vector<Item> items, shown;
};

//==============================================================================
/** A block's parameters, searchable, for MIDI learn. */
class ParamPicker : public juce::Component,
                    private juce::ListBoxModel
{
public:
    std::function<void (int index)> onPick;

    explicit ParamPicker (juce::AudioProcessor& p);

    /** Marks a parameter as the chosen one and scrolls to it. */
    void highlight (int index);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void refilter();

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;

    juce::StringArray names;
    std::vector<int> shown;
    int chosen = -1;
    juce::TextEditor search;
    juce::ListBox list;
};
} // namespace tf
