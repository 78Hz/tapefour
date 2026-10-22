#pragma once

#include "StripComponent.h"
#include "CassetteComponent.h"
#include "TapeShelf.h"
#include "PatchCard.h"
#include "InlayCard.h"

namespace tf
{
class SetupWindow;

class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& plugin, std::function<void()> onClose);
    void closeButtonPressed() override { if (closeCallback) closeCallback(); }

private:
    std::function<void()> closeCallback;
};

/**
    The whole front panel, laid out at a fixed design size (1440 x 1190)
    and scaled to fit the window.
*/
class DeckComponent : public juce::Component,
                      private juce::Timer
{
public:
    static constexpr int designWidth = 1440, designHeight = 1190;

    DeckComponent (AudioEngine& engine, juce::PropertiesFile& settings);
    ~DeckComponent() override;

    void prepareToQuit();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

private:
    void timerCallback() override;

    // tape handling
    void loadStartupTape();
    void tapeChanged();
    bool saveTape (bool announce);
    void insertBlank (int type, int model);
    void loadFromShelf (const juce::String& id);
    void showEjectMenu();

    // tracks
    void openPatch (int track);
    void openInlay();
    void showOverlay (std::unique_ptr<juce::Component> card, juce::Rectangle<int> cardBounds);
    void closeOverlay();
    void deckStateApplied();
    void closePluginWindow (int track);
    void openPluginWindow (int track);
    void openSetup (int tab);

    void say (const juce::String& message, double seconds = 2.5);
    void transport (TapeEngine::Command c);
    juce::String speedLabel() const;

    AudioEngine& engine;
    juce::PropertiesFile& props;

    // header
    LcdButton setupLcd;

    // cassette bay
    CassetteComponent cassette;
    FlatButton eject { "EJECT" };
    InlayThumb inlayThumb;

    // transport
    FlatButton resetBtn { "RESET" }, rtzBtn { "RTZ" };
    LcdButton status;
    KeyButton recKey { KeyButton::rec, "REC" }, rewKey { KeyButton::rew, "REW" }, ffKey { KeyButton::ff, "F.FWD" },
              playKey { KeyButton::play, "PLAY" }, stopKey { KeyButton::stop, "STOP" };
    LabeledKnob pitch;
    FlatButton speedLo { utf ("1\xe2\x85\x9e IPS") }, speedHi { utf ("3\xc2\xbe IPS") };
    FlatButton nrButton { "NOISE REDUCTION" };
    LcdButton nrType;

    // mixer
    juce::OwnedArray<StripComponent> strips;
    MasterStrip master;

    std::unique_ptr<SetupWindow> setupWindow;
    std::unique_ptr<PluginWindow> pluginWindows[numTracks];
    std::unique_ptr<Overlay> overlay;

    juce::String message;
    double messageUntil = 0;
    int frame = 0, stoppedFrames = 0, midiHold = 0, midiOutHold = 0;
    juce::String counterText { "000" };
    bool recLamp = false, playLamp = false, midiLamp = false, midiOutLamp = false;

    // computer keyboard as a MIDI keyboard
    juce::String keyMap { "awsedftgyhujkolp;" };
    int keyOctave = 4;
    std::array<bool, 17> keysDown {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckComponent)
};
} // namespace tf
