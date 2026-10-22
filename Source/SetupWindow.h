#pragma once

#include "AudioEngine.h"
#include "DeckLookAndFeel.h"

namespace tf
{
class SetupWindow : public juce::DocumentWindow
{
public:
    SetupWindow (AudioEngine& engine, juce::PropertiesFile& settings);
    ~SetupWindow() override;

    void showTab (int index);
    void closeButtonPressed() override { setVisible (false); }

private:
    std::unique_ptr<juce::TabbedComponent> tabs;
};
} // namespace tf
