#include "InstrumentEditor.h"
#include "InstrumentWidgets.h"

namespace tf
{
InstrumentEditor::InstrumentEditor (juce::AudioProcessor& p, presets::Presettable& i, const juce::String& t,
                                    std::function<std::unique_ptr<juce::Component> (Say)> make)
    : juce::AudioProcessorEditor (p), inst (i), title (t), makePanel (std::move (make)), display (i)
{
    display.onClick = [this] { showPresets (list == nullptr); };
    addAndMakeVisible (display);
    showPresets (false);
    setSize (width, height);
}

InstrumentEditor::~InstrumentEditor() = default;

void InstrumentEditor::showPresets (bool show)
{
    display.open = show;
    display.repaint();
    if (show)
    {
        panel.reset();
        list = std::make_unique<PresetList> (inst);
        list->onMessage = [this] (const juce::String& m) { say (m); };
        list->onLoaded = [this] { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<InstrumentEditor> (this)] { if (safe != nullptr) safe->showPresets (false); }); };
        addAndMakeVisible (*list);
    }
    else
    {
        list.reset();
        panel = makePanel ([safe = juce::Component::SafePointer<InstrumentEditor> (this)] (const juce::String& m) { if (safe != nullptr) safe->say (m); });
        addAndMakeVisible (*panel);
    }
    resized();
}

void InstrumentEditor::say (const juce::String& m)
{
    message = m.toUpperCase();
    startTimer (4000);
    repaint();
}

void InstrumentEditor::paint (juce::Graphics& g)
{
    g.fillAll (Pal::lcd);
    auto head = getLocalBounds().reduced (14, 10).removeFromTop (26).withTrimmedLeft (236);
    iw::text (g, title, head, Pal::lcdDim, 11.0f);
    if (message.isNotEmpty()) iw::text (g, message, head, Pal::lcdText, 11.0f, juce::Justification::centredRight);
}

void InstrumentEditor::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    auto head = r.removeFromTop (26);
    display.setBounds (head.removeFromLeft (220));
    r.removeFromTop (10);
    if (panel != nullptr) panel->setBounds (r);
    if (list != nullptr) list->setBounds (r);
}
} // namespace tf
