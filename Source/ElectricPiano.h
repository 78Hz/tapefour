#pragma once

#include "Common.h"
#include "Tuning.h"

/*
    TF ELECTRIC PIANO: a reed electric piano after the Wurlitzer 200 and 200A
    (docs/electric-piano.md has the sources, and which values are measured and which are set by ear).

      reeds     each key a steel reed: a fundamental and two higher bending modes (inharmonic), each
                ringing down at its own rate; low reeds ring longest. Tuned to the tuning system.
      hammers   a felt hammer hits the reed: the harder and the shorter the contact, the more of the
                higher modes. A hammer let off too early doesn't reach the reed on the softest notes.
      pickup    the reed's tip swings through a slot in a charged comb (160-170 V): reed and comb are
                a capacitor, greatest when the reed is level with the comb. Its output follows the
                capacitance, so a reed resting a little off level gives a clean fundamental when it
                swings a little, and folds over (the bark) when it swings through the comb.
      dampers   felt dampers stop the reeds when keys come up, unless the sustain pedal holds them.
      amp       preamp (drive), vibrato (5.75 Hz, an amplitude wobble), volume, and either the power
                amp and the two 4x8" oval speakers or the rear jack (the preamp's output); mains hum.

    One instrument, mono. It ages with playing time: reeds drift, felt hardens, reeds go short.
*/
namespace tf::ep
{
/** The controls, as numbers: everything that decides the sound, for the audio thread and the editor alike. */
struct Settings
{
    int model = 1;                 // 0: 200, 1: 200A
    bool ages = true;
    // reeds
    float tuningSpreadCents = 3.0f, ringTime = 1.0f;
    int shortRingers = 1;
    // hammers
    float felt = 0.5f, strikePoint = 0.0f, blowInches = 1.078f, letOffInches = 0.125f, velocityCurve = 0.0f;
    // pickup
    float reedHeight = 0.5f, gapMm = 0.5f, volts = 165.0f;
    // dampers and noises (dB)
    float damping = 0.7f, damperNoiseDb = -56.0f, actionNoiseDb = -50.0f, pedalNoiseDb = -58.0f;
    // amp
    float volume = 7.0f, vibrato = 0.4f, drive = 0.3f, humDb = -78.0f;
    bool rearJack = true, mains60 = false;
    // playing
    bool mpe = false, bend = true;
    int pressureTarget = 0;        // 0 off, 1 volume, 2 vibrato depth, 3 preamp drive, 4 damper
    float pressureAmount = 0.6f;
    int slideTarget = 0;           // 0 off, 1 hammer felt, 2 reed height
};

/** A condition: the technician's controls for a piano in that state (amp settings are left alone). */
void applyCondition (Settings&, int condition);   // 0 NEW, 1 SERVICED, 2 WORN, 3 NEGLECTED
const char* conditionName (int condition);

/** What each key's reed is like, from the settings, the piano's own random seed and its playing time. */
struct Reed
{
    double hz = 0.0;               // tuned frequency, drift included (0: the keyboard map leaves it unmapped)
    double driftCents = 0.0;       // how far it has wandered from the tuning system
    double ringSeconds = 0.0;      // time constant of the fundamental's decay
    bool shortRinger = false;
    bool onKeyboard = false;       // one of the real piano's 64 keys (A1..C7)
};
inline constexpr int lowestKey = 33, highestKey = 96;   // A1 .. C7, the 64 keys
std::array<Reed, 128> reeds (const Settings&, const std::array<double, 128>& tuned, uint32_t seed, double hours);

/** How the piano has aged: what playing time adds to its spread, felt, short ringers and dampers. */
struct Ageing { double spreadCents = 0, felt = 0, damping = 0; int shortRingers = 0; };
Ageing ageing (double hours);

/** The pickup: its output for a reed tip at displacement x (in units of a full-velocity swing), for the editor's trace. */
double pickup (const Settings&, double x, double reedHz);

/** C4's ring time to -40 dB, seconds (for the REEDS readout). */
double ringToMinus40 (const Reed&);

//==============================================================================
class Processor : public juce::AudioProcessor
{
public:
    Processor();
    ~Processor() override;

    /** The parameters, in the editor's terms; read on any thread. */
    Settings settings() const;
    /** Sets parameters from a settings struct (message thread: conditions). */
    void setSettings (const Settings&);
    void chooseCondition (int condition);   // sets the controls and the playing time back to zero
    int lastCondition() const noexcept { return condition.load(); }

    tuning::Settings tuningSettings() const;
    void setScale (const tuning::Scale&, const std::string& sclText);
    void setKeyboardMap (const tuning::KeyboardMap&, const std::string& kbmText);
    void clearKeyboardMap();
    juce::String scaleDescription() const;
    bool hasKeyboardMap() const;

    double hoursPlayed() const noexcept { return hours.load(); }
    uint32_t seed() const noexcept { return reedSeed; }
    std::array<Reed, 128> currentReeds() const;

    void applyMpe (bool zoneOn, int bendRange) noexcept;

    // juce::AudioProcessor
    const juce::String getName() const override;
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 6.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    /** The whole piano (controls, tuning files, seed, condition; its age if withAge), and back. */
    std::unique_ptr<juce::XmlElement> toXml (bool withAge) const;
    void fromXml (const juce::XmlElement&);

    /** The preset it was loaded from or saved as ("" none), and whether it has changed since. */
    juce::String currentPreset() const { return presetName; }
    void setPreset (const juce::String& name);
    bool isModified() const;
    void renamePreset (const juce::String& name) { presetName = name; }   // keeps whether it's modified

    /** The parameters by id, for the editor. */
    juce::RangedAudioParameter* param (const juce::String& id) const;

    struct Impl;

private:
    juce::String fingerprint() const;
    juce::String presetName, presetFingerprint;
    std::unique_ptr<Impl> impl;
    std::atomic<double> hours { 0.0 };
    std::atomic<int> condition { 1 };
    uint32_t reedSeed = 0;
    std::atomic<bool> zone { false };
    std::atomic<int> zoneBend { 48 };
};
/** Presets: one file each (.tfep) in Documents/TapeFour/Presets/Electric Piano, the whole piano in each. */
namespace presets
{
    struct Entry { juce::File file; juce::String name, summary; juce::Time saved; };
    juce::File folder();
    juce::File fileFor (const juce::String& name);
    std::vector<Entry> list();
    bool save (Processor&, const juce::String& name, juce::String& error);
    bool load (Processor&, const juce::File&, juce::String& error);
    bool rename (Processor&, const juce::String& newName, juce::String& error);
    bool remove (Processor&, juce::String& error);
}
} // namespace tf::ep
