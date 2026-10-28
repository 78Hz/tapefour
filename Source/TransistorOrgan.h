#pragma once

#include "Common.h"
#include "Tuning.h"
#include "InstrumentPresets.h"

/*
    TF TRANSISTOR: a transistor combo organ after the single-manual Vox Continental (1962)
    (docs/transistor-organ.md has the sources, and which values are measured and which are set by ear).

      generators  twelve LC oscillators make the top octave (C7..B7); each feeds a chain of six
                  flip-flops, each halving it, so every tone below is locked exactly an octave down:
                  84 square waves, C1..B7, running all the time
      voicing     each tone's square wave rounded by its own RC filter before the keys: the reed tone
      keys        49 (C2..C6). Each key closes four contacts onto four busbars: 16', 8', 4', and IV
                  (2 2/3', 2', 1 3/5', 1'). Contacts switch a tone straight on mid-cycle, so they click;
                  they bounce, and they don't all close at the same instant
      drawbars    the four busbars' levels; then the flute tone (the mix through two more RC filters)
                  and the reed tone (the mix as it is), each on its own drawbar
      vibrato     a square wave, rounded by an RC filter, wobbling all twelve oscillators together

    Polyphonic, mono out, no velocity. The oscillators drift, and a technician can tune them up.
*/
namespace tf::organ
{
inline constexpr int lowestKey = 36, highestKey = 84;       // the 49 keys, C2..C6 at 8'
inline constexpr int lowestTone = 24, highestTone = 107;    // the generators' tones, C1..B7
inline constexpr int numTones = highestTone - lowestTone + 1;

/** The controls: the panel's, and the ones inside the case. */
struct Settings
{
    // the panel: drawbars 0..8
    float drawbar16 = 6.0f, drawbar8 = 8.0f, drawbar4 = 5.0f, drawbarIV = 3.0f;
    float flute = 4.0f, reed = 8.0f;
    bool vibrato = true;
    // inside the case
    float vibratoHz = 6.4f, vibratoCents = 18.0f;
    float driftCents = 3.0f;
    float click = 1.0f;          // 1: contacts switch instantly (as built), 0: softened over 5 ms
    float bounceMs = 1.0f;       // how long a contact chatters as it closes or opens
    float spreadMs = 3.0f;       // how far apart a key's four contacts close
};

/** A drawbar's position (0..8) as a gain: 3 dB a step, nothing at 0. */
double drawbarGain (double position);

/** The tones (MIDI numbers) a key's contacts switch: 16', 8', 4', and IV's four, folded into the generators' range. */
struct KeyTones { int foot16 = 0, foot8 = 0, foot4 = 0; std::array<int, 4> mixture {}; };
KeyTones keyTones (int key);

/** One key: contact c (0 16', 1 8', 2 4', 3 IV) closed t seconds after the key went down; released u seconds
    after it came up (u < 0: still down). The engine and the editor's diagram both use this. */
bool contactClosed (const Settings&, uint32_t seed, int key, int contact, double t, double u);
/** When contact c first touches, seconds after the key goes down (or starts to open after it comes up). */
double contactDelay (const Settings&, uint32_t seed, int key, int contact, bool opening);

/** The vibrato's wobble at time t (s): a square wave rounded by an RC filter, -1..1. */
double vibratoShape (double t, double hz);

/** Each tone's RC corner as a multiple of its frequency; the flute filter's two corners (Hz). */
inline constexpr double reedCornerRatio = 1.0, fluteCornerHz = 250.0, fluteMakeup = 2.0;

/** The tuning systems a divide-down organ can have: twelve oscillators, exact octaves. */
const juce::StringArray& systemChoices();
tuning::System systemFor (int choice);

//==============================================================================
class Processor : public juce::AudioProcessor,
                  public presets::Presettable
{
public:
    Processor();
    ~Processor() override;

    Settings settings() const;
    void setSettings (const Settings&);

    tuning::Settings tuningSettings() const;
    /** A 12-note Scala scale with a 2/1 octave (anything else is refused, with the reason). */
    bool setScale (const tuning::Scale&, const std::string& sclText, std::string& error);
    juce::String scaleDescription() const;

    /** The twelve oscillators (C7..B7), as tuned, and how far each has drifted now (cents). */
    std::array<double, 12> oscillatorHz() const;
    std::array<double, 12> driftNow() const;
    /** The technician retunes the twelve coils: the drift starts again from zero. */
    void tuneUp() noexcept { tuneRequest = true; }

    uint32_t seed() const noexcept { return contactSeed; }
    juce::RangedAudioParameter* param (const juce::String& id) const;

    // juce::AudioProcessor
    const juce::String getName() const override;
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 0.05; }
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

    // presets::Presettable
    std::unique_ptr<juce::XmlElement> toXml (bool withAge) const override;
    void fromXml (const juce::XmlElement&) override;
    const presets::Kind& presetKind() const override;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;
    uint32_t contactSeed = 0;
    std::atomic<bool> tuneRequest { false };
};

/** The organ's presets: one file each (.tfto) in Documents/TapeFour/Presets/Transistor. */
const presets::Kind& presetKind();
} // namespace tf::organ
