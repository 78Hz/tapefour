#include "PluginWindow.h"

namespace tf
{
PluginWindow::PluginWindow (juce::AudioProcessor& plugin, const juce::String& title, std::function<void()> onClose)
    : juce::DocumentWindow (title, Pal::panel, juce::DocumentWindow::closeButton),
      closeCallback (std::move (onClose))
{
    setUsingNativeTitleBar (true);
    if (auto* editor = plugin.createEditorIfNeeded())
    {
        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
    }
    centreWithSize (std::max (200, getWidth()), std::max (100, getHeight()));
    setAlwaysOnTop (true);   // stays in front of the deck while you tweak it
    setVisible (true);
}
} // namespace tf
