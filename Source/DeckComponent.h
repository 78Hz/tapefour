#pragma once

#include "StripComponent.h"
#include "CassetteComponent.h"
#include "TapeShelf.h"
#include "ChainEditor.h"
#include "InlayCard.h"
#include "TapeWriter.h"
#include "Exporter.h"
#include "PluginWindow.h"

namespace tf
{
class SetupWindow;

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
    void notice (const juce::String& text, double seconds) { say (text, seconds); }
    /** Puts a .cass file on the shelf and loads it (from the menu, the command line, or a double-click). */
    void openCassetteFile (const juce::File& cassFile);

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
    void switchTape (std::function<std::unique_ptr<Tape>()> make, std::function<void (Tape&)> after, const juce::String& whatItsDoing);
    void insertBlank (int type, int model, const juce::String& grade = "new");
    void loadFromShelf (const juce::String& id);
    void showEjectMenu();
    void exportCassette();
    void chooseCassetteFile();
    void toggleWriteProtect();
    void turnTapeOver (bool sideBUp);
    void exportTracks();

    // ---- the master deck and mixdowns (MasterDeckCard, MasterDeck) ----
    void openMasterDeck();
    void masterSettingsChanged();
    void flipMasterTape();
    void toggleMixdown();
    bool armMixdown (bool announce);
    void newMasterTape();
    void loadMasterTape (const juce::String& id);
    void putInMasterTape (std::function<std::unique_ptr<Tape>()> make, const juce::String& doneText);
    void exportMasterCassette();
    void saveMasterSide (int side);
    void collectMixdown();
    juce::File mixdownFolder() const;
    juce::String mixdownName() const;
    bool masterBusy = false;    // a master tape is being made or loaded
    /** The file formats exports and mixdowns write, as a menu of choices (shared, remembered). */
    juce::PopupMenu formatsMenu();
    void setExportFormats (const ExportFormats&);
    ExportFormats exportFormats;

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
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File pendingCassette;   // asked for while a tape was still loading
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
    juce::String learnShown;
    TapeWriter writer;
    bool tapeBusy = false;      // a tape is being loaded, made or exported
    juce::String busyText;      // what the deck is busy with

    juce::String message;
    double messageUntil = 0;
    int frame = 0, stoppedFrames = 0, recordingFrames = 0, midiHold = 0, midiOutHold = 0;
    bool hasRunSinceCapture = false;   // the tape has moved since plug-ins were last asked for their sound
    juce::String counterText { "000" }, shownTransport;
    bool midiLamp = false, midiOutLamp = false;

    // computer keyboard as a MIDI keyboard
    juce::String keyMap { "awsedftgyhujkolp;" };
    int keyOctave = 4;
    std::array<bool, 17> keysDown {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckComponent)
};
} // namespace tf
