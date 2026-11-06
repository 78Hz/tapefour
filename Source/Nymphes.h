#pragma once

#include "Common.h"
#include "InstrumentPresets.h"

/*
    TF NYMPHES: an editor for a Dreadbox Nymphes (a six-voice analogue synth) over MIDI CC. A MIDI effect:
    put it in front of an external instrument block sending to the Nymphes. Notes pass through; each
    control is a CC from Dreadbox's chart (Nymphes Owner's Manual v2.0, section 8), sent when it changes,
    on channel 1 (with MPE, the zone's master channel; without, the external block moves it to its
    channel). The modulation matrix is CC 30 (the source) then a depth CC. The synth can't be read
    (its preset dump is SysEx in a format Dreadbox gives only on request), so the editor is in charge:
    SEND ALL, a loaded preset and a reopened deck send everything. KEEP IN STEP: CCs coming back (the
    synth's CC out, or a controller) move the knobs, and go on to the synth only if they're new.
*/
namespace tf::nym
{
enum class Kind { knob, choice, toggle };

/** One control: what it's called, its CC, its range (most are 0..127), its default. */
struct Control
{
    const char* id;
    const char* name;
    int cc;
    int hi;          // 127, or a choice's last index
    int def;
    Kind kind;
    const char* choices = nullptr;   // "POLY|UNI A|..."
};

enum : int
{
    oscWave, oscPw, oscLevel, oscSub, oscNoise, oscDetune, oscChord, oscGlide, oscLfo, oscEg,
    lpfCutoff, lpfRes, lpfEg, lpfTrack, lpfLfo, hpfCutoff,
    revSize, revDecay, revFilter, revMix,
    ampLevel,
    fegA, fegD, fegS, fegR,
    aegA, aegD, aegS, aegR,
    lfo1Rate, lfo1Wave, lfo1Delay, lfo1Fade, lfo1Type, lfo1Sync,
    lfo2Rate, lfo2Wave, lfo2Delay, lfo2Fade, lfo2Type, lfo2Sync,
    playMode, legato,
    numControls
};
const Control& control (int index);

/** The modulation matrix: four sources, 36 destinations (CC 30 picks the source, then the depth's CC). */
inline constexpr int numSources = 4, numDests = 36;
const char* sourceName (int s);          // LFO 2, MOD WHEEL, VELOCITY, PRESSURE
const char* destName (int d);            // short: WAVE, LEVEL, ... ; group by destGroup
const char* destGroup (int d);           // OSC, FILTER, FILTER EG, AMP EG, LFO 1, LFO 2, REVERB
int destCc (int d);
bool destPerNote (int d);                // responds per note with MPE or poly aftertouch (the manual's list)
bool destOnSynth (int source, int d);    // false: the synth has no such route (LFO 2 reaches OSC, FILTER and the EGs)
bool destUnsure (int source, int d);     // in the CC chart, not in the manual's 28: to be checked on the synth

struct State
{
    std::array<int, numControls> values {};
    std::array<std::array<int, numDests>, numSources> depth {};   // -1: not set (not sent)
    bool mpe = false;
    int bendRange = 48;          // the per-note pitch-bend range sent in the MPE set-up
    bool keepInStep = true;      // CCs coming back move the knobs
    bool depthCentred = false;   // depths are signed around 64 (else 0 = none); to be found out on the synth
    State();
};

/** What's sounding, from the MIDI going through: for the window's PLAYING NOW. */
struct Voice
{
    std::atomic<int> note { -1 }, channel { 0 };
    std::atomic<float> bend { 0.0f }, pressure { 0.0f }, slide { 0.0f };
};
inline constexpr int numVoices = 6;

class Processor : public juce::AudioProcessor,
                  public presets::Presettable
{
public:
    Processor();
    ~Processor() override;

    // ---- the window (message thread) ----
    int value (int c) const noexcept { return values[(size_t) c].load(); }
    void setValue (int c, int v);
    int depth (int s, int d) const noexcept { return depths[(size_t) s][(size_t) d].load(); }
    void setDepth (int s, int d, int v);           // -1: unset
    bool isMpe() const noexcept { return mpe.load(); }
    void setMpe (bool on);
    int bendRange() const noexcept { return bend.load(); }
    void setBendRange (int semis);
    bool keepsInStep() const noexcept { return keep.load(); }
    void setKeepInStep (bool on) { keep = on; }
    bool depthsCentred() const noexcept { return centred.load(); }
    void setDepthsCentred (bool on);
    /** Sends every control, the matrix's set cells and the MPE set-up at the next block. */
    void sendAll() { sendAllPending = true; }
    /** The last CC sent, for the window: (cc << 8) | value, or -1. */
    int lastSent() const noexcept { return lastSentCc.load(); }
    const Voice& voice (int i) const { return voices[(size_t) i]; }

    // juce::AudioProcessor
    const juce::String getName() const override { return "TF NYMPHES"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return editorFactory ? editorFactory (*this) : nullptr; }
    bool hasEditor() const override { return editorFactory != nullptr; }
    static inline std::function<juce::AudioProcessorEditor* (Processor&)> editorFactory;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    std::unique_ptr<juce::XmlElement> toXml (bool withAge) const override;
    void fromXml (const juce::XmlElement&) override;
    const presets::Kind& presetKind() const override;

    State snapshot() const;

private:
    class Param;
    void noteVoices (const juce::MidiMessage&);

    std::array<std::atomic<int>, numControls> values;
    std::array<std::array<std::atomic<int>, numDests>, numSources> depths;
    std::atomic<bool> mpe { false }, keep { true }, centred { false }, sendAllPending { false }, mpePending { false };
    std::atomic<int> bend { 48 }, lastSentCc { -1 };

    // the audio thread's: what the synth was last sent
    std::array<int, numControls> sent {};
    std::array<std::array<int, numDests>, numSources> sentDepth {};
    int synthSource = -1;              // the source CC 30 last chose
    int inSource = 0;                  // CC 30 coming back: which source the depths coming back are for
    std::array<Voice, numVoices> voices;
};

const presets::Kind& presetKind();
} // namespace tf::nym
