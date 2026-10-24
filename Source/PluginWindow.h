#pragma once

#include "DeckLookAndFeel.h"

namespace tf
{
/** A plug-in's own editor in a window of its own, kept in front of the deck. */
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioProcessor& plugin, const juce::String& title, std::function<void()> onClose);
    void closeButtonPressed() override { if (closeCallback) closeCallback(); }

    /** Transport keys typed while the plug-in has the keyboard still reach the deck. */
    std::function<bool (const juce::KeyPress&)> onDeckKey;
    bool keyPressed (const juce::KeyPress& k) override { return onDeckKey != nullptr && onDeckKey (k); }

private:
    std::function<void()> closeCallback;
};
} // namespace tf
