#pragma once

#include "Common.h"
#include "InstrumentPresets.h"

/*
    TF STEP: a 16-step MIDI sequencer with no tempo. Each step has its own time (10 ms to 10 s), a note
    (or a rest), a gate (how much of its time the note sounds), a velocity, and can be tied: held into
    the next step so a mono synth glides. It's a MIDI effect: everything after it in the chain plays
    it (a hardware synth through an external instrument block, or any instrument). Notes from the
    track's keyboard transpose the sequence. Its own PLAY, like a sequencer plugged into the recorder.
*/
namespace tf::step
{
inline constexpr int numSteps = 16;
inline constexpr int lowestNote = 12, highestNote = 108;     // C0..C8 (C4 = middle C, MIDI 60)
inline constexpr double shortestMs = 10.0, longestMs = 10000.0, defaultMs = 180.0;

struct Step
{
    int note = 36;              // C2
    bool rest = false;
    bool tie = false;           // held into the next step (legato)
    double ms = defaultMs;
    int gate = 50;              // 1..100 % of the step's time
    int velocity = 100;         // 1..127
};

struct State
{
    std::array<Step, numSteps> steps {};
    int length = numSteps;      // the steps played: 1..16
    bool transpose = true;      // the keyboard moves the sequence
    int homeNote = 36;          // the key that plays it as written (C2)
    double cycleMs() const;     // the steps played, end to end
};

juce::String noteName (int note);   // "C2", "D#4"
juce::String msText (double ms);    // "180 ms", "1.20 s"

/** Like a sequencer plugged into the recorder: its own PLAY, not the tape's transport. */
class Processor : public juce::AudioProcessor,
                  public presets::Presettable
{
public:
    Processor();
    ~Processor() override;

    // ---- the state (message thread): edit, then changed() ----
    State& state() noexcept { return edit; }
    void changed();

    void setPlaying (bool);
    bool isPlaying() const noexcept { return playing.load(); }
    /** The step playing now (-1 stopped), and the keyboard's transposition, for the window. */
    int playingStep() const noexcept { return shownStep.load(); }
    int transposition() const noexcept { return shift.load(); }

    // juce::AudioProcessor
    const juce::String getName() const override { return "TF STEP"; }
    void prepareToPlay (double sampleRate, int blockSize) override;
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
    int editorStep = 0;          // the window's selected step
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // presets::Presettable: the 16 steps and the settings
    std::unique_ptr<juce::XmlElement> toXml (bool withAge) const override;
    void fromXml (const juce::XmlElement&) override;
    const presets::Kind& presetKind() const override;

private:
    void startStep (juce::MidiBuffer& out, int at);
    void noteOff (juce::MidiBuffer& out, int at);

    State edit, run;              // the window's, and the audio thread's copy
    std::unique_ptr<State> pending;
    juce::SpinLock pendingLock;
    std::atomic<bool> playing { false };
    std::atomic<int> shownStep { -1 }, shift { 0 };

    // the audio thread's place
    double sr = 48000.0;
    bool running = false;
    juce::int64 now = 0;          // samples since PLAY
    double nextStepAt = 0.0;      // when the next step starts
    double offAt = -1.0;          // when the sounding note's gate ends (-1: held, or none)
    int index = -1;               // the step sounding
    int held = -1;                // the note sounding (-1 none)
};

const presets::Kind& presetKind();
} // namespace tf::step
