#pragma once

#include "AudioEngine.h"
#include "Widgets.h"

namespace tf
{
/**
    The master deck, opened from the MASTER display on the master strip: the stereo cassette a
    mixdown goes to (or DIRECT, no tape), the files it writes, and what's on each side of it.

    The tape, length and condition are a cassette's own: changing them puts in a new blank master
    tape (straight away while the one in the deck is still blank; otherwise with NEW MASTER TAPE).
    Every setting is remembered.
*/
class MasterDeckCard : public juce::Component,
                       private juce::Timer
{
public:
    MasterDeckCard (AudioEngine& engine, ExportFormats formats);
    ~MasterDeckCard() override;

    static constexpr int width = 840, height = 720;

    std::function<void()> onClose;
    /** The tape, length, condition, NR or record level changed (engine.masterDeck.settings holds them). */
    std::function<void()> onSettingsChanged;
    std::function<void (const ExportFormats&)> onFormatsChanged;
    std::function<void()> onFlip, onNewMasterTape, onExportCassette, onShowFolder;
    std::function<void (int side)> onSaveSide;
    std::function<void (const MixTake&)> onListen;
    std::function<void (const juce::String& id)> onLoadMaster;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Takes;
    void timerCallback() override;
    void refresh();
    void setFormats (const ExportFormats&);
    LedKey* key (juce::OwnedArray<LedKey>& row, const juce::String& text, std::function<void()> click);

    AudioEngine& engine;
    ExportFormats formats;
    LedKey closeX { "X", {}, false };
    juce::OwnedArray<LedKey> tapeKeys, lengthKeys, nrKeys, gradeKeys, formatKeys, sideKeys;
    LcdButton wavDepth, flacDepth, mp3Rate, nextTape;
    LabeledKnob recLevel;
    LedKey saveSide { "SAVE SIDE AS ONE FILE", {}, false }, showFolder { "SHOW FOLDER", {}, false },
           newTape { "NEW MASTER TAPE", {}, false }, exportCass { "EXPORT .CASS", {}, false },
           masterTapes { utf ("MASTER TAPES \xe2\x96\xbe"), {}, false };
    std::unique_ptr<Takes> takes;
    juce::Rectangle<int> cassetteArea, mapArea, labelColumn;
    juce::String shown;
};
} // namespace tf
