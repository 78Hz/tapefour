#pragma once

#include "StripComponent.h"
#include "CassetteComponent.h"
#include "TapeShelf.h"
#include "ChainEditor.h"
#include "InlayCard.h"
#include "TapeWriter.h"

namespace tf
{
class SetupWindow;

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

/**
    The whole front panel, laid out like a portastudio at a fixed design size
    (1600 x 900): the mixer on the left, the tape section in a column on the right.
    Scaled to fit the window.
*/
class DeckComponent : public juce::Component,
                      private juce::Timer
{
public:
    static constexpr int designWidth = 1600, designHeight = 900;

    DeckComponent (AudioEngine& engine, juce::PropertiesFile& settings);
    ~DeckComponent() override;

    void prepareToQuit();
    /** Shows a message in the status display. */
    void notice (const juce::String& message, double seconds) { say (message, seconds); }

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
    /** Queues a save. withDeck also stores the deck with the tape (asking plug-ins for their
        settings if fresh); while recording only the audio and track sheet are saved. */
    void saveTape (bool announce, bool withDeck = true, bool fresh = false);
    /** Saves the tape in the deck, then makes or loads the next one in the background and puts it in. */
    void switchTape (std::function<std::unique_ptr<Tape>()> make, std::function<void (Tape&)> after, const juce::String& busyText);
    void insertBlank (int type, int model);
    void loadFromShelf (const juce::String& id);
    void showEjectMenu();

    // tracks
    void openChain (int track);
    void openInlay();
    void showOverlay (std::unique_ptr<juce::Component> card, juce::Rectangle<int> cardBounds);
    void closeOverlay();
    void deckStateApplied();
    void closeBlockWindow (int track, int blockId);
    void openBlockWindow (int track, int blockId);
    void editSound (int track);

    // MIDI learn
    struct LearnTarget { juce::Component* component; MidiMapper::Target target; };
    void gatherLearnTargets();
    const LearnTarget* learnTargetFor (juce::Component* c) const;
    void setLearnMode (bool on);
    void learnTargetMenu (const LearnTarget&);
    void startLearning (const MidiMapper::Target&);
    juce::String targetName (const MidiMapper::Target&) const;
    void remoteControl (MidiMapper::Control c);
    friend class LearnLayer;
    void openSetup (int tab);

    void say (const juce::String& message, double seconds = 2.5);
    void transport (TapeEngine::Command c);
    juce::String speedLabel() const;
    void syncSpeed();

    AudioEngine& engine;
    juce::PropertiesFile& props;

    // header
    LcdButton setupLcd;
    LedKey learnBtn { "LEARN", Pal::amber };

    // cassette bay
    CassetteComponent cassette;
    LedKey eject { "EJECT", {}, false };
    InlayThumb inlayThumb;
    TapeSwitch tapeSwitch;

    // transport
    LedKey resetBtn { "RESET", {}, false }, rtzBtn { "RTZ", {}, false };
    LcdButton status;
    KeyButton recKey { KeyButton::rec, "REC" }, rewKey { KeyButton::rew, "REW" }, ffKey { KeyButton::ff, "F.FWD" },
              playKey { KeyButton::play, "PLAY" }, stopKey { KeyButton::stop, "STOP" };
    LabeledKnob pitch;
    LedKey speedLo { utf ("1\xe2\x85\x9e IPS"), Pal::amber }, speedHi { utf ("3\xc2\xbe IPS"), Pal::amber };
    LedKey nrButton { "NOISE REDUCTION", Pal::amber };
    LcdButton nrType;

    // mixer
    juce::OwnedArray<StripComponent> strips;
    MasterStrip master;

    std::unique_ptr<SetupWindow> setupWindow;
    std::map<std::pair<int, int>, std::unique_ptr<PluginWindow>> pluginWindows;   // (track, block id)
    std::unique_ptr<Overlay> overlay;
    int chainTrack = -1;                       // whose chain is open
    std::vector<LearnTarget> learnTargets;
    std::unique_ptr<juce::Component> learnLayer;
    TapeWriter writer;
    bool tapeBusy = false;      // a tape is being loaded or made

    juce::String message;
    double messageUntil = 0;
    int frame = 0, stoppedFrames = 0, recordingFrames = 0, midiHold = 0, midiOutHold = 0;
    juce::String counterText { "000" }, shownTransport;
    bool recLamp = false, playLamp = false, midiLamp = false, midiOutLamp = false;

    // computer keyboard as a MIDI keyboard
    juce::String keyMap { "awsedftgyhujkolp;" };
    int keyOctave = 4;
    std::array<bool, 17> keysDown {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckComponent)
};
} // namespace tf
