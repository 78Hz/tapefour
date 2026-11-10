#pragma once

#include "Plate.h"
#include "InstrumentWidgets.h"

namespace tf
{
/**
    TF PLATE's window: the plate from above in its frame (the driver, the pickups to drag, the damper),
    a click through it drawn as time against pitch, the plate and its damper, the pickups and the output,
    and the tail as it sounds.
*/
class PlatePanel : public juce::Component,
                   private juce::Timer
{
public:
    explicit PlatePanel (plate::Processor&);
    ~PlatePanel() override;

    static constexpr int panelWidth = 832, panelHeight = 500;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    std::function<void (const juce::String&)> onMessage;
    /** Brings the picture up to date now (it's drawn on another thread as the settings change). */
    void waitForPicture();

private:
    void timerCallback() override;
    LcdChip& chip (const juce::String& label, std::function<bool()> lit, std::function<void()> click, float size = 9.5f);
    iw::Knob& knob (const juce::String& id, const juce::String& label, std::function<juce::String (float)> fmt, int radius = 17);
    void refresh();
    juce::Rectangle<float> plateRect() const;
    juce::Point<float> pickupAt (int which) const;
    void startPicture();

    plate::Processor& proc;
    juce::OwnedArray<iw::Knob> knobs;
    juce::OwnedArray<LcdChip> chips;
    std::vector<std::function<bool()>> chipLit;
    juce::Rectangle<int> plateArea, clickArea, plateCtl, pickCtl, tailArea, viewArea, graphArea, curveArea;
    int dragging = -1;
    juce::String shown, pictureKey;
    std::vector<float> history;
    int historyPos = 0;

    // the click's picture, made on a thread
    juce::Image picture;
    std::unique_ptr<juce::Thread> painter;
    juce::CriticalSection pictureLock;
    juce::Image madePicture;
    std::atomic<bool> pictureReady { false };
};
} // namespace tf
