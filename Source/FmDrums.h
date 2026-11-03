#pragma once

#include "Common.h"
#include "InstrumentPresets.h"

/*
    TF FM DRUMS: a six-track FM drum machine in the manner of the Elektron Model:Cycles (not a copy of it):
    each track plays one of six "machines" (KICK, SNARE, METAL, PERC, TONE, CHORD), each a small FM
    algorithm with PITCH, DECAY and four macros (COLOR, SHAPE, SWEEP, CONTOUR) mapped so the whole of each
    knob's travel stays musical; drive, pan, a delay and a reverb on sends, an LFO a track; a 64-step
    sequencer with per-step parameter locks, chance and retrigs, 16 patterns. docs/fm-drums.md says where
    the choices come from (Chowning's FM percussion, the TR-808's cymbal ratios) and which are ours.

    Every setting is a small integer (0..127 for the knobs), so the window, presets and engine share it.
*/
namespace tf::fmd
{
inline constexpr int numTracks = 6, maxSteps = 64, numPatterns = 16;

enum Machine { kick = 0, snare, metal, perc, tone, chord, numMachines };
const char* machineName (int m);

/** A track's sound: what a step can lock, too. */
enum Param
{
    pMachine, pPitch, pDecay, pColor, pShape, pSweep, pContour,
    pVolume, pDrive, pPan, pDelay, pReverb,
    pLfoSpeed, pLfoDepth, pLfoDest, pLfoShape,
    numParams
};
struct ParamInfo { const char* id; const char* caption; int lo, hi, def; };
const ParamInfo& paramInfo (int p);
/** What a macro does on a machine (COLOR on KICK is "BITE", and so on): the window's caption. */
const char* macroMeaning (int machine, int param);
/** A value as the window shows it (a note for TONE's pitch, a chord's name, Hz, ms). */
juce::String valueText (int machine, int param, int value);

enum LfoDest { lPitch = 0, lDecay, lColor, lShape, lSweep, lContour, lVolume, lPan, numLfoDests };
const char* lfoDestName (int d);
enum LfoShape { lsSine = 0, lsTriangle, lsRamp, lsSquare, lsRandom, numLfoShapes };
const char* lfoShapeName (int s);

struct Step
{
    int8_t on = 0;
    int8_t velocity = 100;      // 1..127
    int8_t chance = 100;        // 1..100 %
    int8_t retrig = 0;          // extra hits in the step: 0..3
    int16_t lock[numParams];    // -1 none
    Step() { std::fill (std::begin (lock), std::end (lock), (int16_t) -1); }
    bool hasLocks() const { for (auto l : lock) if (l >= 0) return true; return false; }
};

struct Pattern
{
    int length = 16;            // 1..64 steps
    Step steps[numTracks][maxSteps];
    bool empty() const;
};

enum Division { d16 = 0, d8, d8dot, d4, d4dot, d2, numDivisions };   // the delay's time, in the tempo
const char* divisionName (int d);

struct State
{
    int sound[numTracks][numParams];
    bool mute[numTracks] {};
    std::array<Pattern, numPatterns> patterns;
    int pattern = 0;            // playing (and edited)
    int tempo = 1200;           // tenths of a BPM, 400..2400
    int swing = 50;             // 50..75 %
    int delayTime = d8dot, delayFeedback = 50, delayTone = 80;
    int reverbSize = 70, reverbDecay = 60, reverbTone = 70;
    int master = 100;
    void init();
};

/** Like a drum machine plugged into the recorder: its own tempo and its own PLAY, not the tape's transport. */
class Processor : public juce::AudioProcessor,
                  public presets::Presettable
{
public:
    Processor();
    ~Processor() override;

    // ---- the state (message thread): edit, then changed() ----
    State& state() noexcept { return edit; }
    void changed();

    // ---- the transport and pads (message thread) ----
    void setPlaying (bool);
    bool isPlaying() const noexcept { return playing.load(); }
    /** The step playing now (-1 stopped), for the window. */
    int playingStep() const noexcept { return shownStep.load(); }
    /** A track's hit from the window (as from MIDI): its sound, no locks. */
    void hit (int track, int velocity);
    /** How loud each track is now (0..1), for the window's lamps. */
    float trackLevel (int track) const noexcept { return levels[(size_t) std::clamp (track, 0, numTracks - 1)].load(); }

    // juce::AudioProcessor
    const juce::String getName() const override { return "TF FM DRUMS"; }
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    double getTailLengthSeconds() const override { return 6.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return editorFactory ? editorFactory (*this) : nullptr; }
    bool hasEditor() const override { return editorFactory != nullptr; }
    static inline std::function<juce::AudioProcessorEditor* (Processor&)> editorFactory;
    int editorTab = 0, editorTrack = 0, editorPage = 0, editorStep = -1;   // the window's place (editorStep: the step being locked, -1 none)
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // presets::Presettable: the kit, the 16 patterns and the settings
    std::unique_ptr<juce::XmlElement> toXml (bool withAge) const override;
    void fromXml (const juce::XmlElement&) override;
    const presets::Kind& presetKind() const override;

    struct Engine;

private:
    State edit;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<State> pending;
    juce::SpinLock pendingLock;
    std::atomic<int> generation { 0 };
    std::atomic<bool> playing { false };
    std::atomic<int> shownStep { -1 };
    std::array<std::atomic<float>, numTracks> levels {};
    struct Hit { int track, velocity; };
    juce::AbstractFifo hitFifo { 64 };
    std::array<Hit, 64> hits {};
};

const presets::Kind& presetKind();
} // namespace tf::fmd
